/// @file non_finite_substitution_count_test.cpp
/// @brief The mastering owners that answer a non-finite sample with a finite,
///        in-range, plausible one must say so.
///
/// Every owner below leaves an output that is finite, inside its own ceiling and
/// free of any error whether or not the input was degraded, so a caller holding
/// the buffer has nothing to read. The substitution count is the one observable
/// that separates the two streams, and these cases hold each owner to it from
/// both sides: a long clean run must leave the count at zero, and a run carrying
/// one poisoned sample must move it.
///
/// A dormant owner would pass the second half on its own, so every case first
/// asserts that the owner is doing something to the clean fixture — its ceiling
/// sits under the fixture's peak, and the clean output peak is checked against
/// it. Reading the output against the input sample by sample would not do: every
/// owner here has lookahead, so the delay alone would look like activity.
///
/// The hard clipper is the exception in the population: it passes a NaN through
/// (both of std::clamp's comparisons are false for it) and substitutes only an
/// infinity, so its NaN case asserts the count does NOT move.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/saturation/hard_clipper.h"
#include "util/constants.h"
#include "util/db.h"

namespace {

using sonare::constants::kPiD;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr int kBlockCount = 8;
constexpr int kPoisonBlock = 2;
constexpr int kPoisonIndex = 100;
/// Fixture peak. Above every ceiling configured below, so no owner is dormant
/// over this signal: measured at 1.07 against the widest of them (the hard
/// clipper at 0.5) and against the -1 dBTP ceilings (0.891 linear).
constexpr float kPeak = 1.07f;
/// Ceiling the hard clipper runs at. Well under the fixture peak, so the clean
/// run is clipped on every cycle rather than passing through untouched.
constexpr float kClipperCeiling = 0.5f;

std::vector<float> program_block(int block_index) {
  std::vector<float> out(static_cast<std::size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    out[static_cast<std::size_t>(i)] = static_cast<float>(kPeak * std::sin(2.0 * kPiD * 220.0 * t));
  }
  return out;
}

/// Drives @p owner over the whole fixture. @p poison_block of -1 leaves the
/// fixture clean; otherwise one sample of that block is replaced by @p poison.
/// Returns the concatenated output.
template <typename Owner>
std::vector<float> drive(Owner& owner, int poison_block, float poison) {
  std::vector<float> all;
  all.reserve(static_cast<std::size_t>(kBlockCount * kBlockSize));
  for (int b = 0; b < kBlockCount; ++b) {
    std::vector<float> buffer = program_block(b);
    if (b == poison_block) buffer[static_cast<std::size_t>(kPoisonIndex)] = poison;
    float* channels[1] = {buffer.data()};
    owner.process(channels, 1, kBlockSize);
    all.insert(all.end(), buffer.begin(), buffer.end());
  }
  return all;
}

/// Largest magnitude among the finite samples. Folding with std::max over the
/// whole buffer would drop a non-finite one instead of reporting it, so the
/// non-finite samples are counted separately rather than compared.
float finite_peak(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) {
    if (!std::isfinite(sample)) continue;
    const float magnitude = std::abs(sample);
    if (magnitude > peak) peak = magnitude;
  }
  return peak;
}

/// Asserts the owner is not dormant over the clean fixture, then that it counts
/// nothing there and something on a run carrying @p poison.
/// @param output_bound Linear peak the clean output must be held under. It is the
///        owner's ceiling plus whatever its own reconstruction filter may ring by.
template <typename Owner>
void counts_only_the_poisoned_run(Owner& owner, float poison, float output_bound) {
  owner.prepare(kSampleRate, kBlockSize);
  REQUIRE(owner.non_finite_substitution_count() == 0u);

  const std::vector<float> clean = drive(owner, -1, 0.0f);
  // Non-vacuity: a ceiling above the fixture peak would leave the owner passing
  // the signal through, and nothing below would be measuring anything.
  REQUIRE(finite_peak(clean) < kPeak);
  REQUIRE(finite_peak(clean) <= output_bound);
  REQUIRE(owner.non_finite_substitution_count() == 0u);

  drive(owner, kPoisonBlock, poison);
  REQUIRE(owner.non_finite_substitution_count() > 0u);
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
/// The ceiling every limiter below is configured with, plus the margin a
/// polyphase reconstruction may ring by.
const float kLimiterBound = sonare::db_to_linear(-1.0f) * 1.05f;

// ---------------------------------------------------------------------------
// Chain-level aggregate
// ---------------------------------------------------------------------------
// A chain caller never holds the owners above, so the aggregate on the result is
// the only thing that separates a degraded stream from a clean one. The chain
// refuses a non-finite input outright, so these cases drive a stage into making
// one: a makeup gain whose linear form overflows a float hands the limiter an
// infinity that never came from the caller.

constexpr int kChainSampleRate = 48000;
constexpr std::size_t kChainLength = 24000;  // 0.5 s
constexpr float kChainPeak = 0.5f;
/// Ceiling the chain's limiter runs at. Well under the level the compressor
/// leaves, so the limiter is not dormant on a clean run either.
constexpr float kChainCeilingDb = -20.0f;
/// db_to_linear of this is an infinity, so the compressor's output multiply
/// overflows for every sample.
constexpr float kOverflowingMakeupDb = 1.0e6f;

std::vector<float> chain_program() {
  std::vector<float> out(kChainLength);
  for (std::size_t i = 0; i < kChainLength; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(kChainSampleRate);
    out[i] = static_cast<float>(kChainPeak * std::sin(2.0 * kPiD * 220.0 * t));
  }
  return out;
}

/// Compressor into true-peak limiter. @p makeup_db of kOverflowingMakeupDb is
/// what makes the compressor emit the non-finite samples the limiter replaces.
sonare::mastering::api::MasteringChainConfig chain_config(float makeup_db, int oversample) {
  sonare::mastering::api::MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.makeup_gain_db = makeup_db;
  config.maximizer.true_peak_limiter.enabled = true;
  config.maximizer.true_peak_limiter.config.ceiling_db = kChainCeilingDb;
  config.maximizer.true_peak_limiter.config.oversample_factor = oversample;
  return config;
}

/// Stages the chain cases below run, in order. Asserted rather than assumed:
/// the count has exactly one writer under this config, and a fixture that later
/// enabled loudness would add a second without any case failing.
const std::vector<std::string> kExpectedStages{"dynamics.compressor", "maximizer.truePeakLimiter"};

std::size_t non_finite_count(const std::vector<float>& samples) {
  std::size_t count = 0;
  for (float sample : samples) {
    if (!std::isfinite(sample)) ++count;
  }
  return count;
}

float stage_gain_reduction_db(const sonare::mastering::api::MonoChainResult& result,
                              const std::string& stage) {
  for (const auto& reduction : result.stage_gain_reductions) {
    if (reduction.stage == stage) return reduction.gain_reduction_db;
  }
  return 0.0f;
}

/// Runs @p chain over the fixture in kBlockSize blocks and returns the output.
std::vector<float> drive_streaming(sonare::mastering::api::StreamingMasteringChain& chain,
                                   const std::vector<float>& program) {
  std::vector<float> out;
  out.reserve(program.size());
  for (std::size_t offset = 0; offset < program.size();
       offset += static_cast<std::size_t>(kBlockSize)) {
    const std::size_t count =
        std::min(static_cast<std::size_t>(kBlockSize), program.size() - offset);
    std::vector<float> buffer(program.begin() + static_cast<std::ptrdiff_t>(offset),
                              program.begin() + static_cast<std::ptrdiff_t>(offset + count));
    float* channels[1] = {buffer.data()};
    chain.process_block(channels, 1, static_cast<int>(count));
    out.insert(out.end(), buffer.begin(), buffer.end());
  }
  return out;
}

}  // namespace

TEST_CASE("BrickwallLimiter counts the non-finite samples it replaces",
          "[mastering][dynamics][non-finite]") {
  SECTION("NaN") {
    sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kNaN, kLimiterBound);
  }
  SECTION("positive infinity") {
    sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kInf, kLimiterBound);
  }
  SECTION("negative infinity") {
    sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
    counts_only_the_poisoned_run(owner, -kInf, kLimiterBound);
  }
}

TEST_CASE("BrickwallLimiter separates substitution from ordinary clipping",
          "[mastering][dynamics][non-finite]") {
  // hard_clip_count() cannot stand in for the substitution count: it is cleared
  // at the top of every process() call, so by the block after the poisoned one it
  // is back to reporting nothing while the substitution is still on the record.
  sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
  owner.prepare(kSampleRate, kBlockSize);
  drive(owner, kPoisonBlock, kNaN);
  REQUIRE(owner.hard_clip_count() == 0);
  REQUIRE(owner.non_finite_substitution_count() > 0u);
}

TEST_CASE("BrickwallLimiter substitution count survives reset and clears on prepare",
          "[mastering][dynamics][non-finite]") {
  sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
  owner.prepare(kSampleRate, kBlockSize);
  drive(owner, kPoisonBlock, kNaN);
  const std::uint32_t after_poison = owner.non_finite_substitution_count();
  REQUIRE(after_poison > 0u);

  owner.reset();
  REQUIRE(owner.non_finite_substitution_count() == after_poison);

  owner.prepare(kSampleRate, kBlockSize);
  REQUIRE(owner.non_finite_substitution_count() == 0u);
}

TEST_CASE("TruePeakLimiter counts the non-finite samples it replaces",
          "[mastering][maximizer][non-finite]") {
  SECTION("NaN") {
    sonare::mastering::maximizer::TruePeakLimiter owner({-1.0f, 1.0f, 50.0f, 4, false});
    counts_only_the_poisoned_run(owner, kNaN, kLimiterBound);
  }
  SECTION("positive infinity") {
    sonare::mastering::maximizer::TruePeakLimiter owner({-1.0f, 1.0f, 50.0f, 4, false});
    counts_only_the_poisoned_run(owner, kInf, kLimiterBound);
  }
  SECTION("negative infinity, detect-only path") {
    sonare::mastering::maximizer::TruePeakLimiter owner({-1.0f, 1.0f, 50.0f, 4, true});
    counts_only_the_poisoned_run(owner, -kInf, kLimiterBound);
  }
}

TEST_CASE("Maximizer reports the substitutions its inner limiter made",
          "[mastering][maximizer][non-finite]") {
  SECTION("NaN") {
    sonare::mastering::maximizer::Maximizer owner({0.0f, -1.0f, 1.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kNaN, kLimiterBound);
  }
  SECTION("positive infinity") {
    sonare::mastering::maximizer::Maximizer owner({0.0f, -1.0f, 1.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kInf, kLimiterBound);
  }
}

TEST_CASE("SoftKneeMax counts its own knee substitution as well as the inner one",
          "[mastering][maximizer][non-finite]") {
  // The knee runs before the maximizer and tanh folds an infinity onto twice the
  // knee, so an inner-only count would stay at zero for this case.
  SECTION("positive infinity is folded by the knee") {
    sonare::mastering::maximizer::SoftKneeMax owner({0.0f, -1.0f, 6.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kInf, kLimiterBound);
  }
  SECTION("NaN passes the knee and is counted downstream") {
    sonare::mastering::maximizer::SoftKneeMax owner({0.0f, -1.0f, 6.0f, 50.0f});
    counts_only_the_poisoned_run(owner, kNaN, kLimiterBound);
  }
}

TEST_CASE("AdaptiveRelease reports the substitutions its inner limiter made",
          "[mastering][maximizer][non-finite]") {
  SECTION("NaN") {
    sonare::mastering::maximizer::AdaptiveRelease owner;
    counts_only_the_poisoned_run(owner, kNaN, kLimiterBound);
  }
  SECTION("negative infinity") {
    sonare::mastering::maximizer::AdaptiveRelease owner;
    counts_only_the_poisoned_run(owner, -kInf, kLimiterBound);
  }
}

TEST_CASE("HardClipper counts an infinity and leaves a NaN alone",
          "[mastering][saturation][non-finite]") {
  using sonare::mastering::saturation::HardClipper;
  using sonare::rt::AliasingControl;

  SECTION("positive infinity is replaced by the ceiling and counted once") {
    HardClipper owner({kClipperCeiling, AliasingControl::None});
    counts_only_the_poisoned_run(owner, kInf, kClipperCeiling * 1.001f);
    // The clamp path is per sample with no filter around it, so the poisoned
    // sample is the only one replaced.
    REQUIRE(owner.non_finite_substitution_count() == 1u);
  }

  SECTION("negative infinity") {
    HardClipper owner({kClipperCeiling, AliasingControl::None});
    counts_only_the_poisoned_run(owner, -kInf, kClipperCeiling * 1.001f);
    REQUIRE(owner.non_finite_substitution_count() == 1u);
  }

  SECTION("oversampled path counts the infinity the interpolation spread") {
    // The clamp runs at the oversampled rate and the decimation FIR rings above
    // it, so the base-rate output is bounded a little wider than the ceiling.
    HardClipper owner({kClipperCeiling, AliasingControl::Oversample4x});
    counts_only_the_poisoned_run(owner, kInf, kClipperCeiling * 1.15f);
  }

  SECTION("NaN passes through uncounted") {
    HardClipper owner({kClipperCeiling, AliasingControl::None});
    owner.prepare(kSampleRate, kBlockSize);
    const std::vector<float> clean = drive(owner, -1, 0.0f);
    REQUIRE(finite_peak(clean) < kPeak);
    REQUIRE(finite_peak(clean) <= kClipperCeiling * 1.05f);
    REQUIRE(owner.non_finite_substitution_count() == 0u);

    const std::vector<float> poisoned = drive(owner, kPoisonBlock, kNaN);
    // The NaN is still in the caller's buffer, so the degradation is already
    // visible without a count and substituting nothing is the correct behaviour.
    const std::size_t poisoned_position =
        static_cast<std::size_t>(kPoisonBlock * kBlockSize + kPoisonIndex);
    REQUIRE(std::isnan(poisoned[poisoned_position]));
    REQUIRE(owner.non_finite_substitution_count() == 0u);
  }
}

TEST_CASE("Final-stage quantizers report the non-finite samples they replaced",
          "[mastering][final][non-finite]") {
  using namespace sonare::mastering::final;

  // Off the 8-bit grid, so a clean call has to move every sample: a target word
  // length long enough to leave the fixture untouched would measure nothing.
  const std::vector<float> clean_samples{0.301f, -0.452f, 0.613f, -0.204f};
  const auto clean_audio = sonare::Audio::from_buffer(clean_samples.data(), clean_samples.size(),
                                                      static_cast<int>(kSampleRate));
  std::vector<float> poisoned_samples = clean_samples;
  poisoned_samples[0] = kNaN;
  poisoned_samples[2] = kInf;
  const auto poisoned_audio = sonare::Audio::from_buffer(
      poisoned_samples.data(), poisoned_samples.size(), static_cast<int>(kSampleRate));

  SECTION("bit_depth") {
    std::size_t count = 12345;
    const auto clean = bit_depth(clean_audio, {8, true}, &count);
    REQUIRE(count == 0u);
    REQUIRE(clean[0] != clean_samples[0]);  // the quantizer is not dormant

    const auto poisoned = bit_depth(poisoned_audio, {8, true}, &count);
    REQUIRE(count == 2u);
    REQUIRE(std::isfinite(poisoned[0]));
    REQUIRE(std::isfinite(poisoned[2]));
  }

  SECTION("dither, every mode") {
    for (auto type :
         {DitherType::None, DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped}) {
      std::size_t count = 12345;
      dither(clean_audio, {type, 8, 4321}, &count);
      REQUIRE(count == 0u);

      dither(poisoned_audio, {type, 8, 4321}, &count);
      REQUIRE(count == 2u);
    }
  }

  SECTION("output_chain") {
    std::size_t count = 12345;
    const auto clean = output_chain(clean_audio, {8, DitherType::None, true}, &count);
    REQUIRE(count == 0u);
    REQUIRE(clean[0] != clean_samples[0]);

    const auto poisoned = output_chain(poisoned_audio, {8, DitherType::None, true}, &count);
    REQUIRE(count == 2u);
    REQUIRE(std::isfinite(poisoned[0]));
    REQUIRE(std::isfinite(poisoned[2]));
  }
}

TEST_CASE("MasteringChain reports no substitution over ordinary audio",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::MasteringChain;
  using sonare::mastering::api::MonoChainResult;

  const std::vector<float> program = chain_program();
  MasteringChain chain(chain_config(0.0f, 4));
  const MonoChainResult result =
      chain.process_mono(program.data(), program.size(), kChainSampleRate);

  REQUIRE(result.stages == kExpectedStages);
  // Non-vacuity: a limiter passing the signal through would report zero on its
  // own. Its ceiling sits under the level the compressor leaves, so it limits.
  REQUIRE(stage_gain_reduction_db(result, "maximizer.truePeakLimiter") < 0.0f);
  REQUIRE(finite_peak(result.samples) <= sonare::db_to_linear(kChainCeilingDb) * 1.05f);
  REQUIRE(result.non_finite_substitution_count == 0u);
}

TEST_CASE("MasteringChain counts the non-finite samples a stage of its own produced",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::MasteringChain;
  using sonare::mastering::api::MonoChainResult;

  // dynamics.compressor is the producer: its makeup multiply overflows, and
  // maximizer.truePeakLimiter is the stage that replaces the result.
  const std::vector<float> program = chain_program();
  MasteringChain chain(chain_config(kOverflowingMakeupDb, 4));
  const MonoChainResult result =
      chain.process_mono(program.data(), program.size(), kChainSampleRate);

  REQUIRE(result.stages == kExpectedStages);
  REQUIRE(result.non_finite_substitution_count > 0u);
  // The buffer the caller gets back is finite and in range either way, which is
  // why the count is the only signal there is.
  REQUIRE(non_finite_count(result.samples) == 0u);
  REQUIRE(finite_peak(result.samples) <= sonare::db_to_linear(kChainCeilingDb) * 1.05f);
}

TEST_CASE("MasteringChain's substitution count follows the stage that made the replacements",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::MasteringChain;
  using sonare::mastering::api::MonoChainResult;

  // The limiter sanitizes at its oversampled rate, so the same provoked run
  // through a 1x limiter counts a different number of replacements than a 4x
  // one. A field fixed to one value cannot satisfy both.
  const std::vector<float> program = chain_program();
  MasteringChain oversampled(chain_config(kOverflowingMakeupDb, 4));
  MasteringChain base_rate(chain_config(kOverflowingMakeupDb, 1));
  const MonoChainResult oversampled_result =
      oversampled.process_mono(program.data(), program.size(), kChainSampleRate);
  const MonoChainResult base_rate_result =
      base_rate.process_mono(program.data(), program.size(), kChainSampleRate);

  REQUIRE(oversampled_result.stages == kExpectedStages);
  REQUIRE(base_rate_result.stages == kExpectedStages);
  REQUIRE(oversampled_result.non_finite_substitution_count > 0u);
  REQUIRE(base_rate_result.non_finite_substitution_count > 0u);
  REQUIRE(oversampled_result.non_finite_substitution_count !=
          base_rate_result.non_finite_substitution_count);
}

TEST_CASE("StreamingMasteringChain accumulates its substitutions across blocks",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::StreamingMasteringChain;

  const std::vector<float> program = chain_program();

  SECTION("ordinary audio leaves the count at zero") {
    StreamingMasteringChain chain(chain_config(0.0f, 4));
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    REQUIRE(chain.stage_names() == kExpectedStages);
    const std::vector<float> out = drive_streaming(chain, program);
    // Non-vacuity, as above: the limiter is holding the output under a ceiling
    // the input is over.
    REQUIRE(finite_peak(program) > sonare::db_to_linear(kChainCeilingDb));
    REQUIRE(finite_peak(out) <= sonare::db_to_linear(kChainCeilingDb) * 1.05f);
    REQUIRE(chain.non_finite_substitution_count() == 0u);
  }

  SECTION("the count keeps rising while the stages keep replacing") {
    StreamingMasteringChain chain(chain_config(kOverflowingMakeupDb, 4));
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    REQUIRE(chain.stage_names() == kExpectedStages);

    std::vector<float> first(program.begin(), program.begin() + kBlockSize);
    float* channels[1] = {first.data()};
    chain.process_block(channels, 1, kBlockSize);
    const std::uint32_t after_one_block = chain.non_finite_substitution_count();
    REQUIRE(after_one_block > 0u);

    drive_streaming(chain, program);
    REQUIRE(chain.non_finite_substitution_count() > after_one_block);
  }
}
