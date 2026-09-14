/// @file biquad_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        every processor that carries rt::BiquadState across a process call.
///
/// The state is shared but the rule cannot be: process() takes one sample, so a
/// check inside it is the per-sample scan a realtime contract exists to avoid.
/// Each owner applies the rule once per block over its own cells, and each is
/// therefore driven on its own through its public streaming entry point.
///
/// The block that carried the sample is EXPECTED to come out non-finite; that
/// is the positive control, and asserting it keeps an implementation that
/// quietly sanitizes its input from passing. What is asserted afterwards is
/// that every later block is clean and that the stream rejoins a clean-run
/// control.
///
/// Every case asserts the processor is doing something to the signal before it
/// reads a recovery result. An unconfigured processor is a passthrough, and a
/// passthrough recovers instantly for reasons that say nothing about the state
/// under test.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "editing/voice_changer/isp_limiter.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/tape.h"
#include "midi/synth/gs_master_eq.h"
#include "mixing/meter.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;

// Blocks the stream is allowed to take to rejoin its control. A cell returns to
// its post-reset value at once; the stream rejoins only once the state behind
// that cell has re-converged, so each bound follows the slowest time constant in
// its path. Bounds sit at roughly twice their measured crossing (3 / 552 / 158):
// the crossing is where an exponential tail passes one float ULP, and a one-ULP
// difference in the platform's math library moved a comparable fixture's by 45%.
// The de-esser's is the exception, at two and a half times a crossing too small
// for a proportional bound to be worth anything.
constexpr int kDeEsserRecoveryBlocks = 8;
constexpr int kAmpSimRecoveryBlocks = 1100;
constexpr int kGsMasterEqRecoveryBlocks = 320;
// Two owners rejoin nothing inside any practical horizon. The tape's difference
// falls to a rounding floor by block 6 and then neither decays nor grows, so
// whether a run coincides with its control is a property of the target's
// arithmetic: arm64 lands on it at block 90 and x86_64 never does. These are
// where the residual is read, not a crossing either has to reach.
constexpr int kTapeRecoveryBlocks = 180;
constexpr int kVoiceChangerRecoveryBlocks = 300;
// Bounds on those residuals, a decade above the largest measured on either
// target: 1.3e-5 for the tape, 2.6e-5 for the voice changer against a 0.15-peak
// signal. Both move with the fixture.
constexpr double kTapeResidual = 1.0e-4;
constexpr double kVoiceChangerResidual = 1.0e-3;
// The limiter driven alone holds the sample for its lookahead and its peak
// window, both well under one block, and its 50 ms gain release settles inside
// five more. Long enough that a discard it failed to make would still show.
constexpr int kIspLimiterBlocks = 24;
// An ordinary in-range sample, substituted where the poison goes: it cannot make
// any cell non-finite, so what it leaves behind is the chain's own memory.
constexpr float kFinitePerturbation = 4.0f;
// Blocks run past the bound, so a run that misses it still shows how far it got.
constexpr int kHorizonSlack = 20;

/// Speech-like fixture: a fundamental, a low partial and a sibilant band, so
/// the de-essers and shelf detectors under test have something to detect.
std::vector<float> stream_block(int block_index) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    out[static_cast<size_t>(i)] = static_cast<float>(0.45 * std::sin(2.0 * kPiD * 220.0 * t) +
                                                     0.20 * std::sin(2.0 * kPiD * 97.0 * t) +
                                                     0.30 * std::sin(2.0 * kPiD * 7000.0 * t));
  }
  return out;
}

using BlockProcessor = std::function<void(float*, int)>;

/// Runs the stream block by block, optionally poisoning one sample of one block,
/// and returns every output block.
std::vector<std::vector<float>> run_stream(const BlockProcessor& process, int block_count,
                                           float poison_value, bool poison) {
  std::vector<std::vector<float>> outputs;
  outputs.reserve(static_cast<size_t>(block_count));
  for (int k = 0; k < block_count; ++k) {
    std::vector<float> block = stream_block(k);
    if (poison && k == kPoisonBlock) {
      block[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    process(block.data(), kBlockSize);
    outputs.push_back(std::move(block));
  }
  return outputs;
}

/// The largest change the control run makes to the signal. Zero means the
/// processor is a passthrough and no recovery result from it is meaningful.
float control_effect(const std::vector<std::vector<float>>& control) {
  float largest = 0.0f;
  for (size_t k = 0; k < control.size(); ++k) {
    const std::vector<float> source = stream_block(static_cast<int>(k));
    for (int i = 0; i < kBlockSize; ++i) {
      largest = std::max(
          largest, std::abs(control[k][static_cast<size_t>(i)] - source[static_cast<size_t>(i)]));
    }
  }
  return largest;
}

bool block_has_non_finite(const std::vector<float>& block) {
  return std::any_of(block.begin(), block.end(), [](float v) { return !std::isfinite(v); });
}

/// Block from which every later block is bit-identical to the control run, or
/// the block count when the stream never rejoins it. Scanned from the end: a
/// single coincidentally-identical block is not convergence, and a stage that
/// replays its history from a delay line diverges again after one.
int first_identical_block(const std::vector<std::vector<float>>& control,
                          const std::vector<std::vector<float>>& poisoned) {
  int k = static_cast<int>(control.size());
  while (k > kPoisonBlock + 1 &&
         control[static_cast<size_t>(k - 1)] == poisoned[static_cast<size_t>(k - 1)]) {
    --k;
  }
  return k;
}

/// Largest difference from the control over the blocks from @p from onward, or
/// infinity when any difference is non-finite.
/// @note The non-finite return is what keeps the instrument from carrying the
///       defect it measures. std::max returns its first argument when the second
///       is non-finite, so folding the difference into a running maximum skips it
///       and a wholly non-finite run reads as a residual of zero -- a bound this
///       value is compared against would then pass on the worst possible result.
double residual_from(const std::vector<std::vector<float>>& control,
                     const std::vector<std::vector<float>>& poisoned, int from) {
  double worst = 0.0;
  for (size_t k = static_cast<size_t>(from); k < control.size(); ++k) {
    for (int i = 0; i < kBlockSize; ++i) {
      const double difference = std::abs(static_cast<double>(control[k][static_cast<size_t>(i)]) -
                                         poisoned[k][static_cast<size_t>(i)]);
      if (!std::isfinite(difference)) {
        return std::numeric_limits<double>::infinity();
      }
      worst = std::max(worst, difference);
    }
  }
  return worst;
}

/// The load-bearing half of the invariant, and the half that does not depend on
/// floating-point luck: the sample stains no block past the one that carried it.
void require_non_finite_bounded(const std::vector<std::vector<float>>& poisoned,
                                int last_affected_block) {
  int last_non_finite = -1;
  int non_finite_blocks = 0;
  for (size_t k = 0; k < poisoned.size(); ++k) {
    if (block_has_non_finite(poisoned[k])) {
      last_non_finite = static_cast<int>(k);
      ++non_finite_blocks;
    }
  }
  INFO("non-finite blocks " << non_finite_blocks << ", last at " << last_non_finite);
  // Positive control: an implementation that sanitized the input instead of its
  // own state would leave every block finite and pass everything below.
  REQUIRE(non_finite_blocks > 0);
  REQUIRE(last_non_finite <= last_affected_block);
}

/// @param recovery_blocks Block by which the stream must be bit-identical to the
///        control again. Bounding this is the whole claim: "rejoins eventually"
///        is also true of a handle that rejoins on its last measured block.
void require_rejoins_control(const std::vector<std::vector<float>>& control,
                             const std::vector<std::vector<float>>& poisoned, int recovery_blocks) {
  // The residual is in scope before the assertion, so a run that never reaches
  // identity still reports how far apart the streams stayed — which is what
  // separates a bound set too tight from a stream that does not converge.
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  INFO("residual " << residual_from(control, poisoned, recovery_blocks));
  REQUIRE(first_identical_block(control, poisoned) <= recovery_blocks);
}

/// The same claim for an owner that reaches a rounding floor instead of bit
/// identity: past @p from the streams stay within @p ceiling of each other.
void require_rejoins_within(const std::vector<std::vector<float>>& control,
                            const std::vector<std::vector<float>>& poisoned, int from,
                            double ceiling) {
  const double residual = residual_from(control, poisoned, from);
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  INFO("residual " << residual);
  REQUIRE(residual < ceiling);
}

/// Drives one owner twice — clean and poisoned — and asserts the invariant.
/// @param make Builds a prepared, configured processor as a block callable.
/// @param last_affected_block Last block the sample may legitimately reach.
///        The carrying block, unless the owner holds a delay line whose history
///        straddles the boundary.
/// @param residual_ceiling Set for an owner that reaches a rounding floor rather
///        than bit identity; @p recovery_blocks is then where its residual is
///        read.
void check_owner(const std::function<BlockProcessor()>& make, int recovery_blocks,
                 float poison_value, int last_affected_block = kPoisonBlock,
                 std::optional<double> residual_ceiling = std::nullopt) {
  const int block_count = recovery_blocks + kHorizonSlack;
  const auto control = run_stream(make(), block_count, 0.0f, false);

  // Non-vacuity, before any recovery result is read.
  REQUIRE(control_effect(control) > 0.0f);

  const auto poisoned = run_stream(make(), block_count, poison_value, true);
  require_non_finite_bounded(poisoned, last_affected_block);
  if (residual_ceiling) {
    require_rejoins_within(control, poisoned, recovery_blocks, *residual_ceiling);
  } else {
    require_rejoins_control(control, poisoned, recovery_blocks);
  }
}

}  // namespace

TEST_CASE("the de-esser bounds a non-finite sample to its own block", "[mastering][dynamics]") {
  using sonare::mastering::dynamics::DeEsser;
  using sonare::mastering::dynamics::DeEsserConfig;

  const auto make = []() {
    DeEsserConfig config;
    config.frequency_hz = 7000.0f;
    config.threshold_db = -40.0f;
    config.ratio = 8.0f;
    config.range_db = 12.0f;
    auto processor = std::make_shared<DeEsser>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return BlockProcessor([processor](float* buffer, int n) {
      float* channels[] = {buffer};
      processor->process(channels, 1, n);
    });
  };

  const std::array<float, 3> poison_values{std::numeric_limits<float>::quiet_NaN(),
                                           std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity()};
  for (const float poison_value : poison_values) {
    INFO("poison value " << poison_value);
    check_owner(make, kDeEsserRecoveryBlocks, poison_value);
  }
}

TEST_CASE("the tape stage bounds a non-finite sample to its own block", "[mastering][saturation]") {
  using sonare::mastering::saturation::Tape;
  using sonare::mastering::saturation::TapeConfig;

  const auto make = []() {
    TapeConfig config;
    config.drive_db = 6.0f;
    config.saturation = 0.6f;
    config.hysteresis = 0.3f;
    auto processor = std::make_shared<Tape>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return BlockProcessor([processor](float* buffer, int n) {
      float* channels[] = {buffer};
      processor->process(channels, 1, n);
    });
  };

  check_owner(make, kTapeRecoveryBlocks, std::numeric_limits<float>::quiet_NaN(), kPoisonBlock,
              kTapeResidual);
}

TEST_CASE("the amp sim bounds a non-finite sample to its own block", "[mastering][saturation]") {
  using sonare::mastering::saturation::AmpSim;
  using sonare::mastering::saturation::AmpSimConfig;

  const auto make = []() {
    AmpSimConfig config;
    config.drive = 0.6f;
    config.mid_db = 3.0f;
    auto processor = std::make_shared<AmpSim>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return BlockProcessor([processor](float* buffer, int n) {
      float* channels[] = {buffer};
      processor->process(channels, 1, n);
    });
  };

  // The drive stage is oversampled and its polyphase FIR keeps a history of the
  // samples behind the block boundary, so the sample reaches the block after the
  // one that carried it. That history is read at an offset rather than fed back,
  // which is why it flushes without the rule.
  check_owner(make, kAmpSimRecoveryBlocks, std::numeric_limits<float>::quiet_NaN(),
              kPoisonBlock + 1);
}

TEST_CASE("the GS master EQ bounds a non-finite sample to its own block", "[midi][synth]") {
  using sonare::midi::synth::GsMasterEq;
  using sonare::midi::synth::GsMasterEqFilter;

  const auto make = []() {
    auto filter = std::make_shared<GsMasterEqFilter>();
    filter->prepare(kSampleRate);
    GsMasterEq eq;
    eq.low_gain = 0x48;   // +8 dB
    eq.high_gain = 0x38;  // -8 dB
    filter->set(eq);
    REQUIRE(filter->active());
    return BlockProcessor([filter](float* buffer, int n) { filter->process(buffer, nullptr, n); });
  };

  check_owner(make, kGsMasterEqRecoveryBlocks, std::numeric_limits<float>::quiet_NaN());
}

TEST_CASE("the realtime voice changer bounds a non-finite sample to its own block",
          "[editing][voice-changer]") {
  using sonare::editing::voice_changer::RealtimeVoiceChanger;
  using sonare::editing::voice_changer::RealtimeVoiceChangerConfig;

  const auto base_config = []() {
    RealtimeVoiceChangerConfig config;
    config.eq.body_db = 3.0f;
    config.eq.air_db = 4.0f;
    // The reverb follows the rule too, but to a weaker bound — one comb delay
    // period rather than the carrying block. Muting it keeps the cases below on
    // the cells that are bounded to their block; the reverb has its own case.
    config.reverb.mix = 0.0f;
    return config;
  };

  // The same chain with every dynamics stage set to no reduction, so the filters
  // are the only state that can hold the sample. Every detector still runs and
  // still takes the poison; none of them reaches the output. The output limiter
  // cannot be switched off — its ceiling is clamped to at most -1 dBFS — so the
  // trim in front of it goes down instead. The input trim must stay at unity:
  // the sample has to reach the high-pass at full scale to overflow it.
  const auto linear_config = [&base_config]() {
    RealtimeVoiceChangerConfig config = base_config();
    config.output_gain_db = -12.0f;
    config.limiter.enable_isp_limiter = false;
    config.gate.range_db = 0.0f;
    config.compressor.ratio = 1.0f;
    config.compressor.makeup_gain_db = 0.0f;
    config.deesser.range_db = 0.0f;
    return config;
  };

  // Two ways in: make_from hides the processor behind the callable, which is all
  // a section reading the output samples needs. A section reading the chain's
  // discard count has to keep the instance, so it builds one and wraps it.
  const auto prepared = [](const RealtimeVoiceChangerConfig& config) {
    auto processor = std::make_shared<RealtimeVoiceChanger>(config);
    processor->prepare(kSampleRate, kBlockSize, 1);
    return processor;
  };
  const auto block_processor = [](const std::shared_ptr<RealtimeVoiceChanger>& processor) {
    return BlockProcessor([processor](float* buffer, int n) {
      float* channels[] = {buffer};
      processor->process_block(channels, 1, n);
    });
  };
  const auto make_from = [&prepared, &block_processor](RealtimeVoiceChangerConfig config) {
    return [config, &prepared, &block_processor]() { return block_processor(prepared(config)); };
  };

  // A non-finite input sample is flushed to zero before it reaches any filter,
  // so the state is reachable only by overflowing a recurrence from a finite
  // sample: the high-pass feeds back -2*x, which leaves the float range here.
  const float poison_value = std::numeric_limits<float>::max();

  // The retune stage replays its input from a history ring four grains long, so
  // a grain emitted later in the stream reads the sample again. The ring's
  // capacity is the whole of that reach: it is a delay line, read at an offset
  // rather than fed back, so it flushes without the rule.
  RealtimeVoiceChanger probe{base_config()};
  probe.prepare(kSampleRate, kBlockSize, 1);
  const int ring_samples = 4 * probe.config().retune.grain_size;
  const int last_affected = kPoisonBlock + 1 + (ring_samples + kBlockSize - 1) / kBlockSize;

  const int block_count = kVoiceChangerRecoveryBlocks + kHorizonSlack;

  SECTION("driven below its dynamics thresholds") {
    const auto control = run_stream(make_from(linear_config())(), block_count, 0.0f, false);
    REQUIRE(control_effect(control) > 0.0f);
    const auto poisoned = run_stream(make_from(linear_config())(), block_count, poison_value, true);
    require_non_finite_bounded(poisoned, last_affected);

    // Bit-identity is not reachable here and its absence is not this defect:
    // this chain does not forget a one-sample perturbation of ANY kind inside
    // the horizon. The control is the same run with an ordinary in-range sample
    // substituted, which cannot make a cell non-finite and cannot engage the
    // rule at all. It leaves a residual of the same order, so what the residual
    // measures is the chain's memory rather than the state under test.
    const auto perturbed =
        run_stream(make_from(linear_config())(), block_count, kFinitePerturbation, true);
    const double perturbed_residual =
        residual_from(control, perturbed, kVoiceChangerRecoveryBlocks);
    INFO("finite-perturbation residual " << perturbed_residual);
    REQUIRE(perturbed_residual > 0.0);
    REQUIRE(residual_from(control, poisoned, kVoiceChangerRecoveryBlocks) <
            2.0 * perturbed_residual);
  }

  SECTION("driven into its dynamics thresholds") {
    RealtimeVoiceChangerConfig config = base_config();
    config.limiter.enable_isp_limiter = false;
    const auto control = run_stream(make_from(config)(), block_count, 0.0f, false);
    REQUIRE(control_effect(control) > 0.0f);
    const auto poisoned = run_stream(make_from(config)(), block_count, poison_value, true);
    require_non_finite_bounded(poisoned, last_affected);
    REQUIRE(residual_from(control, poisoned, kVoiceChangerRecoveryBlocks) < kVoiceChangerResidual);
  }

  SECTION("with its reverb in the path") {
    RealtimeVoiceChangerConfig config = linear_config();
    config.reverb.mix = RealtimeVoiceChangerConfig{}.reverb.mix;

    // Derived from the tank's geometry, not from a measured crossing. The comb
    // lines hold the feedback and the guard reads their damping cells once per
    // block, so the sample stains every block until the read tap has swept the
    // longest line: a comb delay is a fraction of the decay time, and the guard
    // can be one block behind the sweep that surfaces it.
    const int comb_samples =
        static_cast<int>(config.reverb.time_ms * static_cast<float>(kSampleRate) / 1000.0f);
    const int reverb_reach = std::max(ring_samples, comb_samples);
    const int reverb_last_affected =
        kPoisonBlock + 2 + (reverb_reach + kBlockSize - 1) / kBlockSize;

    const auto control = run_stream(make_from(config)(), block_count, 0.0f, false);
    REQUIRE(control_effect(control) > 0.0f);
    const auto poisoned = run_stream(make_from(config)(), block_count, poison_value, true);
    require_non_finite_bounded(poisoned, reverb_last_affected);
    // The shipped default has the peak limiter on, and every non-finite sample
    // it substitutes leaves the output finite. The discard count is what the
    // positive control reads with that stage in the path; the residual then says
    // the stream rejoined its control rather than merely reporting.
    RealtimeVoiceChangerConfig limited = config;
    limited.limiter.enable_isp_limiter = true;
    auto limited_control_changer = prepared(limited);
    const auto limited_control =
        run_stream(block_processor(limited_control_changer), block_count, 0.0f, false);
    REQUIRE(control_effect(limited_control) > 0.0f);
    REQUIRE(limited_control_changer->non_finite_discard_count() == 0u);
    auto limited_poisoned_changer = prepared(limited);
    const auto limited_poisoned =
        run_stream(block_processor(limited_poisoned_changer), block_count, poison_value, true);
    REQUIRE(limited_poisoned_changer->non_finite_discard_count() > 0u);
    REQUIRE(residual_from(limited_control, limited_poisoned, kVoiceChangerRecoveryBlocks) <
            kVoiceChangerResidual);
  }

  SECTION("with a sample the output trim takes out of range") {
    // The output trim runs in front of a clamp that cannot be switched off, so a
    // finite sample it takes past float range is folded onto the ceiling while
    // every filter cell is still finite. Nothing on the samples can see that:
    // the output is in range and no cell is holding anything to discard. The
    // same poison at unity trim is the control -- it reports nothing, so what
    // the raised trim reports is this fold and not a recurrence upstream.
    RealtimeVoiceChangerConfig at_unity = linear_config();
    at_unity.output_gain_db = 0.0f;
    RealtimeVoiceChangerConfig with_trim = at_unity;
    with_trim.output_gain_db = 12.0f;

    // Large enough that 12 dB of trim leaves float range, small enough that the
    // high-pass's -2x feedback does not.
    const float large = 1.0e38f;
    auto unity_changer = prepared(at_unity);
    const auto unity = run_stream(block_processor(unity_changer), block_count, large, true);
    INFO("unity-trim discards " << unity_changer->non_finite_discard_count());
    REQUIRE(unity_changer->non_finite_discard_count() == 0u);

    auto trimmed_changer = prepared(with_trim);
    const auto trimmed = run_stream(block_processor(trimmed_changer), block_count, large, true);
    for (const auto& block : trimmed) {
      REQUIRE_FALSE(block_has_non_finite(block));
    }
    INFO("trimmed discards " << trimmed_changer->non_finite_discard_count());
    REQUIRE(trimmed_changer->non_finite_discard_count() > 0u);
  }

  SECTION("with the inter-sample-peak limiter") {
    // The ISP stage replaces a non-finite sample with silence or full scale, so
    // the output is finite whether the state behind it is usable or not and no
    // assertion on the samples can separate the two runs. The chain reports the
    // block it discarded its own state in, and that report is the positive
    // control: the clean run must produce none of them.
    RealtimeVoiceChangerConfig config = linear_config();
    config.limiter.enable_isp_limiter = true;
    auto control_changer = prepared(config);
    const auto control = run_stream(block_processor(control_changer), block_count, 0.0f, false);
    REQUIRE(control_effect(control) > 0.0f);
    REQUIRE(control_changer->non_finite_discard_count() == 0u);

    auto poisoned_changer = prepared(config);
    const auto poisoned =
        run_stream(block_processor(poisoned_changer), block_count, poison_value, true);
    INFO("discards " << poisoned_changer->non_finite_discard_count());
    REQUIRE(poisoned_changer->non_finite_discard_count() > 0u);
    REQUIRE(residual_from(control, poisoned, kVoiceChangerRecoveryBlocks) < kVoiceChangerResidual);
  }
}

TEST_CASE("the inter-sample-peak limiter reports the sample it substituted",
          "[editing][voice-changer]") {
  using sonare::editing::voice_changer::IspLimiter;

  // Driven alone, so nothing between the poison and the substitution under test:
  // the sample goes into the lookahead as it stands, where every other owner in
  // this file needs a recurrence overflowed to reach its state at all.
  const auto run = [](float poison_value, bool poison) {
    IspLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize);
    limiter.set_config({-1.0f, 50.0f});
    std::vector<std::vector<float>> outputs;
    int discards = 0;
    for (int k = 0; k < kIspLimiterBlocks; ++k) {
      std::vector<float> block = stream_block(k);
      if (poison && k == kPoisonBlock) {
        block[static_cast<size_t>(kPoisonIndex)] = poison_value;
      }
      limiter.process_block(block.data(), kBlockSize);
      if (limiter.discard_non_finite()) ++discards;
      outputs.push_back(std::move(block));
    }
    return std::make_pair(outputs, discards);
  };

  const auto clean = run(0.0f, false);
  // Non-vacuity: the fixture peaks above the ceiling, so the limiter is shaping
  // gain rather than passing the signal through.
  REQUIRE(control_effect(clean.first) > 0.0f);
  // A clean stream must not report, or the count says nothing about a poisoned
  // one. This is the half of the claim a substitution cannot fake.
  REQUIRE(clean.second == 0);

  const std::array<float, 3> poison_values{std::numeric_limits<float>::quiet_NaN(),
                                           std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity()};
  for (const float poison_value : poison_values) {
    INFO("poison value " << poison_value);
    const auto poisoned = run(poison_value, true);
    INFO("discards " << poisoned.second);
    REQUIRE(poisoned.second > 0);

    // What the report is worth: the stream it describes is finite everywhere and
    // in range, so every assertion available without it passes on both runs.
    for (const auto& block : poisoned.first) {
      REQUIRE_FALSE(block_has_non_finite(block));
    }
    // And it did change the samples. A count that rose while the output stayed
    // identical would be reporting something other than this substitution.
    REQUIRE(residual_from(clean.first, poisoned.first, kPoisonBlock) > 0.0);
  }
}

TEST_CASE("the mixer meter bounds a non-finite sample to its own block", "[mixing][meter]") {
  using sonare::mixing::MeterConfig;
  using sonare::mixing::MeterProcessor;
  using sonare::mixing::MeterSnapshot;

  // The meter publishes nothing until its windows have filled — 400 ms for
  // momentary, 3 s for short-term — so a sample poisoning a window that is not
  // yet reported measures nothing. The poison lands past the longer window, and
  // the run continues long enough for both to refill after it.
  constexpr int kMeterPoisonBlock = 320;
  constexpr int kMeterBlocks = 900;

  const auto run = [](float poison_value, bool poison) {
    MeterConfig config;
    config.measure_lufs = true;
    MeterProcessor meter(config);
    meter.prepare(kSampleRate, kBlockSize);
    std::vector<MeterSnapshot> snapshots;
    snapshots.reserve(static_cast<size_t>(kMeterBlocks));
    for (int k = 0; k < kMeterBlocks; ++k) {
      std::vector<float> block = stream_block(k);
      if (poison && k == kMeterPoisonBlock) {
        block[static_cast<size_t>(kPoisonIndex)] = poison_value;
      }
      float* channels[] = {block.data()};
      meter.process(channels, 1, kBlockSize);
      snapshots.push_back(meter.snapshot());
    }
    return snapshots;
  };

  const auto control = run(0.0f, false);
  // Non-vacuity: the meter has to have measured a loudness, not sat at its
  // floor, before a recovery result from it means anything. The meter does not
  // write to the buffer, so its readout is the only observable.
  const MeterSnapshot& settled = control.back();
  REQUIRE(std::isfinite(settled.momentary_lufs));
  REQUIRE(settled.momentary_lufs > sonare::constants::kFloorDb);
  REQUIRE(std::isfinite(settled.integrated_lufs));
  REQUIRE(settled.integrated_lufs > sonare::constants::kFloorDb);

  const auto poisoned = run(std::numeric_limits<float>::quiet_NaN(), true);

  int last_non_finite = -1;
  int non_finite_blocks = 0;
  for (size_t k = 0; k < poisoned.size(); ++k) {
    const MeterSnapshot& s = poisoned[k];
    if (!std::isfinite(s.momentary_lufs) || !std::isfinite(s.short_term_lufs) ||
        !std::isfinite(s.integrated_lufs)) {
      last_non_finite = static_cast<int>(k);
      ++non_finite_blocks;
    }
  }
  INFO("non-finite snapshots " << non_finite_blocks << ", last at " << last_non_finite);
  REQUIRE(non_finite_blocks > 0);
  REQUIRE(last_non_finite <= kMeterPoisonBlock);

  // Bit-identity is not reachable here and its absence is not this defect. The
  // momentary and short-term readings come from sliding sums, which cannot drop
  // one term: the value subtracted when a sample leaves the window is the same
  // non-finite value that was added. The window is therefore discarded whole and
  // re-accumulated, and two sums over the same terms in a different order differ
  // in their low bits for as long as they run.
  const MeterSnapshot& recovered = poisoned.back();
  REQUIRE(std::abs(recovered.momentary_lufs - settled.momentary_lufs) < 1.0e-3f);
  REQUIRE(std::abs(recovered.short_term_lufs - settled.short_term_lufs) < 1.0e-3f);
}
