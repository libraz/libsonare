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
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/loudness_optimize.h"
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

/// A tilt shelf on its own. The compressor fixture above cannot show a discard:
/// its overflowing makeup gain sits after the envelope, so the samples go
/// non-finite while every cell stays finite.
sonare::mastering::api::MasteringChainConfig tilt_chain_config() {
  sonare::mastering::api::MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 24.0f;
  return config;
}

/// Finite, so process_block accepts it, and close enough to FLT_MAX that the
/// shelf gain above takes the product out of range. Measured rather than
/// assumed: at 1e38 with a 6 dB tilt the product still fits and nothing
/// discards, which is why both numbers are what they are.
constexpr float kHugeFiniteSample = 3.0e38f;

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

// ---------------------------------------------------------------------------
// Named-processor aggregate
// ---------------------------------------------------------------------------
// A caller of the one-shot dispatch holds a result rather than a processor, so
// the count on that result is its only observable. The dispatch refuses a
// non-finite input just as the chain does, so the driver is again a parameter
// that makes a stage produce one: SoftKneeMax applies input_gain_db as a linear
// multiply and nothing bounds it, so a dB value whose linear form overflows a
// float hands the knee an infinity that never came from the caller.

/// Ceiling the soft-knee cases run at. Under the fixture peak, so the knee is
/// shaping on a clean run rather than passing the signal through.
constexpr float kSoftKneeCeilingDb = -20.0f;
/// db_to_linear of this is an infinity, so the knee's drive multiply overflows.
constexpr float kOverflowingInputGainDb = 1.0e6f;

std::vector<sonare::mastering::api::Param> soft_knee_params(float input_gain_db) {
  return {{"inputGainDb", input_gain_db}, {"ceilingDb", kSoftKneeCeilingDb}};
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

TEST_CASE("MasteringChain's substitution count does not depend on the oversampling factor",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::MasteringChain;
  using sonare::mastering::api::MonoChainResult;

  // The limiter replaces a non-finite sample where it arrives, before the
  // reconstruction filter spreads it, so the count is of samples the caller's
  // stream carried rather than of oversampled positions one of them reached. A
  // 1x and a 4x limiter therefore agree on the same provoked run.
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
  REQUIRE(oversampled_result.non_finite_substitution_count ==
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

TEST_CASE("StreamingMasteringChain counts a call in which a stage discarded its state",
          "[mastering][chain][non-finite]") {
  using sonare::mastering::api::StreamingMasteringChain;

  const std::vector<float> program = chain_program();

  SECTION("ordinary audio leaves the count at zero") {
    StreamingMasteringChain chain(chain_config(0.0f, 4));
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    const std::vector<float> out = drive_streaming(chain, program);
    // Non-vacuity, as above: the limiter is holding the output under a ceiling
    // the input is over, so the stages ran and had state to lose.
    REQUIRE(finite_peak(program) > sonare::db_to_linear(kChainCeilingDb));
    REQUIRE(finite_peak(out) <= sonare::db_to_linear(kChainCeilingDb) * 1.05f);
    REQUIRE(chain.non_finite_discard_count() == 0u);
  }

  SECTION("the overflowing compressor substitutes without any stage discarding") {
    // Measured, not assumed, and it is what separates the two counters: the
    // makeup gain that overflows sits AFTER the compressor's envelope, so the
    // samples it emits are non-finite while no cell behind them is. The limiter
    // replaces those samples and reports it; nothing discarded any state.
    StreamingMasteringChain chain(chain_config(kOverflowingMakeupDb, 4));
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    drive_streaming(chain, program);
    REQUIRE(chain.non_finite_substitution_count() > 0u);
    REQUIRE(chain.non_finite_discard_count() == 0u);
  }

  SECTION("a call counts once however many stages discarded during it") {
    // A tilt shelf keeps recursive cells, which is what a discard is about. The
    // input stays finite -- process_block refuses anything else -- and is large
    // enough that the shelf's own multiply leaves float range.
    StreamingMasteringChain chain(tilt_chain_config());
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    REQUIRE(chain.stage_names() == std::vector<std::string>{"eq.tilt"});

    std::vector<float> ordinary(static_cast<std::size_t>(kBlockSize), 0.25f);
    float* ordinary_channels[1] = {ordinary.data()};
    chain.process_block(ordinary_channels, 1, kBlockSize);
    REQUIRE(chain.non_finite_discard_count() == 0u);

    std::vector<float> huge(static_cast<std::size_t>(kBlockSize), kHugeFiniteSample);
    float* huge_channels[1] = {huge.data()};
    chain.process_block(huge_channels, 1, kBlockSize);
    // Every cell the shelf pair holds was lost in this one call, and the call
    // still adds one. Summing the stages would report a count that grows with
    // the chain's length rather than with the damage.
    REQUIRE(chain.non_finite_discard_count() == 1u);
  }

  SECTION("prepare clears it, so it shares an epoch with the substitution count") {
    StreamingMasteringChain chain(tilt_chain_config());
    chain.prepare(kChainSampleRate, kBlockSize, 1);
    std::vector<float> huge(static_cast<std::size_t>(kBlockSize), kHugeFiniteSample);
    float* channels[1] = {huge.data()};
    chain.process_block(channels, 1, kBlockSize);
    REQUIRE(chain.non_finite_discard_count() > 0u);

    // reset() is not the epoch boundary and must not be mistaken for one: it
    // returns the stages' audio state while the count keeps describing what has
    // already happened. Both surfaces document this, so it is pinned here.
    chain.reset();
    REQUIRE(chain.non_finite_discard_count() > 0u);

    chain.prepare(kChainSampleRate, kBlockSize, 1);
    REQUIRE(chain.non_finite_discard_count() == 0u);
  }
}

TEST_CASE("apply_named_processor reports the substitutions its processor made",
          "[mastering][named-processor][non-finite]") {
  using sonare::mastering::api::apply_named_processor;

  const std::vector<float> program = chain_program();

  SECTION("ordinary audio leaves the count at zero") {
    const auto result =
        apply_named_processor("maximizer.softKneeMax", program.data(), program.size(),
                              kChainSampleRate, soft_knee_params(0.0f));
    // Non-vacuity: a ceiling above the fixture peak would leave the knee passing
    // the signal through, and the zero below would say nothing.
    REQUIRE(finite_peak(program) > sonare::db_to_linear(kSoftKneeCeilingDb));
    REQUIRE(finite_peak(result.samples) < finite_peak(program));
    REQUIRE(result.non_finite_substitution_count == 0u);
  }

  SECTION("a stage producing non-finite samples moves the count") {
    const auto result =
        apply_named_processor("maximizer.softKneeMax", program.data(), program.size(),
                              kChainSampleRate, soft_knee_params(kOverflowingInputGainDb));
    REQUIRE(result.non_finite_substitution_count > 0u);
    // The buffer the caller gets back is finite either way, which is why the
    // count is the only signal there is.
    REQUIRE(non_finite_count(result.samples) == 0u);
  }
}

TEST_CASE("apply_named_processor_stereo reports the substitutions its processor made",
          "[mastering][named-processor][non-finite]") {
  using sonare::mastering::api::apply_named_processor_stereo;

  // Driven through the stereo entry as well as the mono one: it reaches the
  // shared dispatch by a different route, and a result assembled there would
  // carry the count only if the stereo path threads the same outcome.
  const std::vector<float> program = chain_program();

  SECTION("ordinary audio leaves the count at zero") {
    const auto result =
        apply_named_processor_stereo("maximizer.softKneeMax", program.data(), program.data(),
                                     program.size(), kChainSampleRate, soft_knee_params(0.0f));
    REQUIRE(finite_peak(result.left) < finite_peak(program));
    REQUIRE(result.non_finite_substitution_count == 0u);
  }

  SECTION("a stage producing non-finite samples moves the count") {
    const auto result = apply_named_processor_stereo(
        "maximizer.softKneeMax", program.data(), program.data(), program.size(), kChainSampleRate,
        soft_knee_params(kOverflowingInputGainDb));
    REQUIRE(result.non_finite_substitution_count > 0u);
    REQUIRE(non_finite_count(result.left) == 0u);
    REQUIRE(non_finite_count(result.right) == 0u);
  }
}

TEST_CASE("loudness_optimize reports the substitutions its internal limiter made",
          "[mastering][loudness][non-finite]") {
  using sonare::mastering::maximizer::loudness_optimize;
  using sonare::mastering::maximizer::LoudnessOptimizeConfig;

  const std::vector<float> program = chain_program();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(program.data(), program.size(), kChainSampleRate);

  SECTION("ordinary audio leaves the count at zero") {
    LoudnessOptimizeConfig config;
    config.ceiling_db = kChainCeilingDb;
    const auto result = loudness_optimize(audio, config);
    // Non-vacuity: the ceiling sits under the fixture peak, so the limiter is
    // holding the output down rather than passing it through.
    REQUIRE(finite_peak(program) > sonare::db_to_linear(kChainCeilingDb));
    REQUIRE(finite_peak(std::vector<float>(result.audio.data(),
                                           result.audio.data() + result.audio.size())) <=
            sonare::db_to_linear(kChainCeilingDb) * 1.05f);
    REQUIRE(result.non_finite_substitution_count == 0u);
  }

  SECTION("a normalization gain that overflows moves the count") {
    // Both fields are validated for finiteness only, so a target this far above
    // the input survives and the ceiling headroom does not bound it back down:
    // db_to_linear of the resulting gain overflows and the static multiply hands
    // the limiter an infinity the caller never supplied.
    LoudnessOptimizeConfig config;
    config.target_lufs = 1.0e6f;
    config.ceiling_db = 1.0e6f;
    const auto result = loudness_optimize(audio, config);
    REQUIRE(result.non_finite_substitution_count > 0u);
  }
}

TEST_CASE("a stereo-only branch reports the substitutions it made",
          "[mastering][named-processor][non-finite]") {
  using sonare::mastering::api::apply_named_processor_stereo;

  // maximizer.loudnessOptimize has no mono branch, so it is dispatched by the
  // stereo entry directly rather than through the shared dispatch the cases
  // above reach. A result assembled there carries the count only if that branch
  // threads the same outcome the shared one does.
  const std::vector<float> program = chain_program();
  const std::vector<sonare::mastering::api::Param> params{{"targetLufs", 1.0e6},
                                                          {"ceilingDb", 1.0e6}};
  const auto result =
      apply_named_processor_stereo("maximizer.loudnessOptimize", program.data(), program.data(),
                                   program.size(), kChainSampleRate, params);
  REQUIRE(result.non_finite_substitution_count > 0u);
  REQUIRE(non_finite_count(result.left) == 0u);
  REQUIRE(non_finite_count(result.right) == 0u);
}

// ---------------------------------------------------------------------------
// The replacement value, and the statistics it must stay out of
// ---------------------------------------------------------------------------
// The count above is the one observable separating a degraded stream from a
// clean one, which only holds if the replacement is a value the stage does not
// produce on its own. A ceiling is exactly what a limiter writes when it is
// working. The cases below run silence carrying one non-finite sample: every
// output sample the stage had anything to do with is then zero, so any other
// value names the replacement, and every statistic the stage reports describes
// work it did not do.

namespace {

/// Ceiling the value cases run at. Nothing in their fixture reaches it.
constexpr float kSilenceCeilingDb = -1.0f;

/// The sample furthest from silence, non-finite ones included, so a failure
/// names the value that was written instead of reporting a position.
float furthest_from_silence(const std::vector<float>& samples) {
  float worst = 0.0f;
  for (float sample : samples) {
    if (!std::isfinite(sample)) return sample;
    if (std::abs(sample) > std::abs(worst)) worst = sample;
  }
  return worst;
}

/// What a stage reported over a run of silence carrying one non-finite sample.
struct SilenceRun {
  /// Output of the channel the poison arrived on, concatenated over the run.
  std::vector<float> poisoned_channel;
  /// Output of the channel that stayed clean throughout.
  std::vector<float> clean_channel;
  /// Cleared at the top of every process(), so it is read on the poisoned block.
  int hard_clip_count = 0;
  /// Smallest reduction claimed by any block of the run. The lookahead delays
  /// the poisoned sample, so the claim can surface a block after it arrived.
  float gain_reduction_db = 0.0f;
};

/// Only the brickwall limiter separates its clip stage from its gain stage, so
/// only it has a clip count to keep a substitution out of.
int hard_clip_count_of(const sonare::mastering::dynamics::BrickwallLimiter& owner) {
  return owner.hard_clip_count();
}
int hard_clip_count_of(const sonare::mastering::maximizer::TruePeakLimiter&) { return 0; }

/// Drives two channels of silence, one sample of channel 0 replaced by @p poison.
/// @param exclude_from_detector Keeps channel 0 out of the linked peak detector.
///        An infinity only reaches the substitution as an infinity this way: with
///        the detector watching, the linked peak goes infinite, the gain collapses
///        to exactly zero, and the product is a NaN before the substitution is
///        ever consulted.
template <typename Owner>
SilenceRun drive_silence(Owner& owner, float poison, bool exclude_from_detector) {
  owner.set_detector_excluded_channel(exclude_from_detector ? 0 : -1);
  SilenceRun run;
  for (int b = 0; b < kBlockCount; ++b) {
    std::vector<float> poisoned(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> clean(static_cast<std::size_t>(kBlockSize), 0.0f);
    if (b == kPoisonBlock) poisoned[static_cast<std::size_t>(kPoisonIndex)] = poison;
    float* channels[2] = {poisoned.data(), clean.data()};
    owner.process(channels, 2, kBlockSize);
    if (b == kPoisonBlock) run.hard_clip_count = hard_clip_count_of(owner);
    run.gain_reduction_db = std::min(run.gain_reduction_db, owner.last_gain_reduction_db());
    run.poisoned_channel.insert(run.poisoned_channel.end(), poisoned.begin(), poisoned.end());
    run.clean_channel.insert(run.clean_channel.end(), clean.begin(), clean.end());
  }
  return run;
}

}  // namespace

TEST_CASE("BrickwallLimiter answers a non-finite sample with silence rather than its ceiling",
          "[mastering][dynamics][non-finite]") {
  const float arrival =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity());
  const bool excluded = GENERATE(false, true);
  CAPTURE(arrival, excluded);
  sonare::mastering::dynamics::BrickwallLimiter owner({kSilenceCeilingDb, 1.0f, 50.0f});
  owner.prepare(kSampleRate, kBlockSize);
  const SilenceRun run = drive_silence(owner, arrival, excluded);

  CHECK(furthest_from_silence(run.poisoned_channel) == 0.0f);
  CHECK(furthest_from_silence(run.clean_channel) == 0.0f);
  CHECK(owner.non_finite_substitution_count() == 1u);
  // Nothing in this fixture is over the ceiling, so the stage clipped nothing
  // and reduced nothing. Counting the replacement as either would leave it
  // indistinguishable from the stage doing its job.
  CHECK(run.hard_clip_count == 0);
  CHECK(run.gain_reduction_db == 0.0f);
}

TEST_CASE("TruePeakLimiter answers a non-finite sample with silence rather than its ceiling",
          "[mastering][maximizer][non-finite]") {
  const float arrival =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity());
  const bool excluded = GENERATE(false, true);
  const bool detect_only = GENERATE(false, true);
  CAPTURE(arrival, excluded, detect_only);
  sonare::mastering::maximizer::TruePeakLimiter owner(
      {kSilenceCeilingDb, 1.0f, 50.0f, 4, detect_only});
  owner.prepare(kSampleRate, kBlockSize);
  const SilenceRun run = drive_silence(owner, arrival, excluded);

  CHECK(furthest_from_silence(run.poisoned_channel) == 0.0f);
  CHECK(furthest_from_silence(run.clean_channel) == 0.0f);
  // One sample arrived non-finite, so one substitution is on the record. A count
  // above one is the oversampled stencil that sample spread into, reported as if
  // the caller had delivered that many.
  CHECK(owner.non_finite_substitution_count() == 1u);
  CHECK(owner.minimum_gain_reduction_db() == 0.0f);
}

TEST_CASE("A limiter's own gain can go non-finite over a clean input",
          "[mastering][dynamics][maximizer][non-finite]") {
  // Non-vacuity for the guard each stage keeps after its gain multiply: without
  // a reachable route to a non-finite gain that guard is dead code. release_ms
  // is validated for sign only, and a NaN passes an ordered comparison, so it
  // reaches the release coefficient and the recursive smoother multiplies it
  // into its own state. Every sample the fixture delivers is finite.
  SECTION("BrickwallLimiter") {
    sonare::mastering::dynamics::BrickwallLimiter poisoned({-1.0f, 1.0f, kNaN});
    poisoned.prepare(kSampleRate, kBlockSize);
    const std::vector<float> out = drive(poisoned, -1, 0.0f);
    CHECK(non_finite_count(out) == 0u);
    CHECK(poisoned.non_finite_substitution_count() > 0u);

    // The control that makes the count above the coefficient's doing rather
    // than the fixture's: the same signal through a finite release.
    sonare::mastering::dynamics::BrickwallLimiter ordinary({-1.0f, 1.0f, 50.0f});
    ordinary.prepare(kSampleRate, kBlockSize);
    const std::vector<float> ordinary_out = drive(ordinary, -1, 0.0f);
    CHECK(ordinary.non_finite_substitution_count() == 0u);
    CHECK(finite_peak(ordinary_out) > 0.0f);
  }

  SECTION("TruePeakLimiter") {
    sonare::mastering::maximizer::TruePeakLimiter poisoned({-1.0f, 1.0f, kNaN, 4, false});
    poisoned.prepare(kSampleRate, kBlockSize);
    const std::vector<float> out = drive(poisoned, -1, 0.0f);
    CHECK(non_finite_count(out) == 0u);
    CHECK(poisoned.non_finite_substitution_count() > 0u);

    sonare::mastering::maximizer::TruePeakLimiter ordinary({-1.0f, 1.0f, 50.0f, 4, false});
    ordinary.prepare(kSampleRate, kBlockSize);
    const std::vector<float> ordinary_out = drive(ordinary, -1, 0.0f);
    CHECK(ordinary.non_finite_substitution_count() == 0u);
    CHECK(finite_peak(ordinary_out) > 0.0f);
  }

  SECTION("the automation path cannot reach that coefficient") {
    // Which route poisons the gain is not a property of the value: the in-place
    // release setter clamps with std::max, whose comparison against a NaN is
    // false, so it yields zero instead of passing the value on. prepare() has no
    // such clamp, which is the whole reason the sections above can reach the
    // guard at all.
    sonare::mastering::dynamics::BrickwallLimiter owner({-1.0f, 1.0f, 50.0f});
    owner.prepare(kSampleRate, kBlockSize);
    REQUIRE(owner.set_parameter(1, kNaN));
    const std::vector<float> out = drive(owner, -1, 0.0f);
    CHECK(non_finite_count(out) == 0u);
    CHECK(owner.non_finite_substitution_count() == 0u);
    CHECK(finite_peak(out) > 0.0f);
  }
}

TEST_CASE("A limiter passes a finite sample at its ceiling through uncounted",
          "[mastering][dynamics][maximizer][non-finite]") {
  // The negative control for both cases above: the replacement must fire on the
  // non-finite value and on nothing else, least of all on the value it used to
  // write. A stage that replaced everything would pass them and fail here.
  const float ceiling = sonare::db_to_linear(kSilenceCeilingDb);

  SECTION("BrickwallLimiter") {
    sonare::mastering::dynamics::BrickwallLimiter owner({kSilenceCeilingDb, 1.0f, 50.0f});
    owner.prepare(kSampleRate, kBlockSize);
    const SilenceRun run = drive_silence(owner, ceiling, /*exclude_from_detector=*/true);
    CHECK(owner.non_finite_substitution_count() == 0u);
    CHECK(run.hard_clip_count == 0);
    // At the ceiling rather than over it, so it survives the run at full
    // magnitude instead of being pulled down.
    CHECK(furthest_from_silence(run.poisoned_channel) == ceiling);
  }

  SECTION("TruePeakLimiter") {
    sonare::mastering::maximizer::TruePeakLimiter owner({kSilenceCeilingDb, 1.0f, 50.0f, 4, false});
    owner.prepare(kSampleRate, kBlockSize);
    const SilenceRun run = drive_silence(owner, ceiling, /*exclude_from_detector=*/true);
    CHECK(owner.non_finite_substitution_count() == 0u);
    // A reconstruction filter rings, so this one is bounded rather than exact;
    // what matters is that the sample is still there and was not replaced.
    CHECK(furthest_from_silence(run.poisoned_channel) > ceiling * 0.5f);
    CHECK(furthest_from_silence(run.poisoned_channel) <= kLimiterBound);
  }
}
