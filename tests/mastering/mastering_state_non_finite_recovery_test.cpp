/// @file mastering_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        the mastering owners whose recursive cells nothing returned to rest:
///        the gate's sidechain filter, the Pultec's component charges, the
///        low-end focus filters, the adaptive release envelopes, the true-peak
///        gain smoothers, both imagers' decorrelation allpasses and the mono
///        maker's cascade.
///
/// Each owner applies the rule once per block over its own cells. Which poison
/// value can reach a given cell is a property of the path in front of it — a
/// detector that folds with std::max drops a NaN and passes an infinity — so all
/// three are driven at every owner and only one of them need be the one that
/// strands it.
///
/// Three claims are read, in this order, and the first two are what keep the
/// third from being free:
///
///  1. Non-vacuity: the clean run does something to the signal, and does it in
///     the window where recovery is claimed. An owner resting there rejoins any
///     control it is given.
///  2. Positive control: the poison reached the owner. The block that carried it
///     is expected to come out non-finite, or — where the owner substitutes for
///     non-finite samples of its own accord — to raise that owner's substitution
///     count. An owner that quietly sanitized its input would pass everything
///     else while holding no state under test at all.
///  3. Recovery, bounded. "Rejoins eventually" is also true of a handle that
///     rejoins on its last measured block.
///
/// The three values are not interchangeable, and the peak envelopes are where
/// that bites. A cell folded as std::max(new, decayed) does drop a NaN, but it
/// keeps an infinity — and an infinity is exactly what an envelope over |x|
/// acquires. Poisoning with a NaN alone leaves those cells looking self-healing.
///
/// The adaptive release and the true peak limiter are read differently from the
/// rest: both substitute for non-finite samples of their own accord, so their
/// output stays finite and finiteness can never go red there. Their positive
/// control is the substitution counter, and the claim is the counter settling,
/// the accessors reading finite, and the stream rejoining its control to the
/// bit. The last of those is the only claim that reaches the true peak limiter's
/// crest peak, which has no accessor.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "mastering/dynamics/gate.h"
#include "mastering/eq/pultec.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kChannels = 2;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;
// Channel 1 sits below channel 0 so per-channel state stays distinguishable and
// the mid/side owners have a side signal to work on.
constexpr float kSecondChannelScale = 0.7f;
// Level the second clean run is driven at, to show the owner answers to its
// input over the blocks where recovery is claimed.
constexpr float kSensitivityScale = 0.25f;

// Blocks the stream is allowed to take to rejoin its control. A cell returns to
// its post-reset value at once; the stream rejoins only once the smoothers
// behind that cell have re-converged, so each bound follows the slowest time
// constant in its path and sits at roughly twice its measured crossing (7 / 833
// / 11 / 3 / 330 / 5 / 139 / 73) -- the crossing is where an exponential tail
// passes one float ULP, and that point moves with the platform's math library.
// The two maximizer bounds follow the slowest of the three poison values: an
// infinity survives the std::max fold in front of a peak envelope, so it takes
// the crest detector an order of magnitude longer to re-converge than a NaN,
// which that fold drops on arrival (16 blocks against 139 at the true peak).
constexpr int kGateRecoveryBlocks = 20;
constexpr int kPultecRecoveryBlocks = 1700;
constexpr int kLowEndFocusRecoveryBlocks = 30;
constexpr int kImagerRecoveryBlocks = 10;
constexpr int kMultibandImagerRecoveryBlocks = 700;
constexpr int kMonoMakerRecoveryBlocks = 15;
constexpr int kTruePeakRecoveryBlocks = 300;
constexpr int kAdaptiveReleaseBlocks = 160;
// Depth of the subharmonic in the low-end focus case that reads the divider.
constexpr float kSubharmonicAmount = 0.6f;
// Blocks run past the bound, so a run that misses it still shows how far it got,
// and more than one tremolo cycle of them: the non-vacuity reading lives in this
// window, and a window inside a single half-cycle finds an owner whose threshold
// that half never crosses resting rather than passing through.
constexpr int kHorizonSlack = 80;
// Floor on the clean run regardless of the bound, so a tight bound does not buy
// itself a short horizon. Two full tremolo cycles.
constexpr int kMinimumCleanBlocks = 150;

/// Fixture with content in every owner's band, under a tremolo that spends a
/// third of a second near silence and a third at full level, so the level-
/// dependent owners cross their thresholds in both directions throughout the
/// horizon and the peak-driven ones are driven over their ceiling.
std::vector<float> stream_block(int block_index, float scale) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    const double tremolo = 0.03 + 0.97 * std::max(0.0, std::sin(2.0 * kPiD * 1.5 * t));
    const double tone =
        0.45 * std::sin(2.0 * kPiD * 60.0 * t) + 0.30 * std::sin(2.0 * kPiD * 220.0 * t) +
        0.30 * std::sin(2.0 * kPiD * 3200.0 * t) + 0.20 * std::sin(2.0 * kPiD * 7000.0 * t);
    out[static_cast<size_t>(i)] = static_cast<float>(scale * tremolo * tone);
  }
  return out;
}

using Blocks = std::vector<std::vector<float>>;
/// Takes one block of the mono fixture and returns both output channels end to
/// end, so per-channel state is compared as well as the detector's.
using BlockProcessor = std::function<std::vector<float>(const std::vector<float>&)>;

std::array<float, 3> poison_values() {
  return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()};
}

/// Spreads the mono fixture over both channels, runs one block through @p
/// processor and returns the two output channels end to end.
BlockProcessor wrap(const std::shared_ptr<sonare::rt::ProcessorBase>& processor) {
  return [processor](const std::vector<float>& mono) {
    std::vector<float> left = mono;
    std::vector<float> right(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) {
      right[i] = kSecondChannelScale * mono[i];
    }
    float* channels[] = {left.data(), right.data()};
    processor->process(channels, kChannels, static_cast<int>(mono.size()));
    std::vector<float> flat = std::move(left);
    flat.insert(flat.end(), right.begin(), right.end());
    return flat;
  };
}

/// A prepared owner plus the meter it publishes, which is read after a run. The
/// meter is empty for owners that publish none.
struct Owner {
  BlockProcessor process;
  std::function<float()> meter;
};
using MakeOwner = std::function<Owner()>;

/// Runs the stream block by block, optionally poisoning one sample of one block,
/// and returns every output block.
Blocks run_stream(const BlockProcessor& process, int block_count, float scale, float poison_value,
                  bool poison) {
  Blocks outputs;
  outputs.reserve(static_cast<size_t>(block_count));
  for (int k = 0; k < block_count; ++k) {
    std::vector<float> block = stream_block(k, scale);
    if (poison && k == kPoisonBlock) {
      block[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    outputs.push_back(process(block));
  }
  return outputs;
}

/// Both channels of the fixture end to end, as the owner received them.
std::vector<float> source_block(int block_index, float scale) {
  const std::vector<float> mono = stream_block(block_index, scale);
  std::vector<float> flat = mono;
  flat.reserve(mono.size() * kChannels);
  for (const float sample : mono) {
    flat.push_back(kSecondChannelScale * sample);
  }
  return flat;
}

/// The largest change the control run makes to the signal from @p from onward.
/// Zero means the owner is a passthrough over those blocks and no recovery
/// result read there is meaningful.
float control_effect(const Blocks& control, float scale, int from) {
  float largest = 0.0f;
  for (size_t k = static_cast<size_t>(from); k < control.size(); ++k) {
    const std::vector<float> source = source_block(static_cast<int>(k), scale);
    for (size_t i = 0; i < source.size(); ++i) {
      largest = std::max(largest, std::abs(control[k][i] - source[i]));
    }
  }
  return largest;
}

bool block_has_non_finite(const std::vector<float>& block) {
  return std::any_of(block.begin(), block.end(), [](float v) { return !std::isfinite(v); });
}

/// Block from which every later block is bit-identical to the control run, or
/// the block count when the stream never rejoins it. Scanned from the end: a
/// single coincidentally-identical block is not convergence.
int first_identical_block(const Blocks& control, const Blocks& poisoned) {
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
///       and a wholly non-finite run reads as a residual of zero.
double residual_from(const Blocks& control, const Blocks& poisoned, int from) {
  double worst = 0.0;
  for (size_t k = static_cast<size_t>(from); k < control.size(); ++k) {
    for (size_t i = 0; i < control[k].size(); ++i) {
      const double difference =
          std::abs(static_cast<double>(control[k][i]) - static_cast<double>(poisoned[k][i]));
      if (!std::isfinite(difference)) {
        return std::numeric_limits<double>::infinity();
      }
      worst = std::max(worst, difference);
    }
  }
  return worst;
}

/// Position, in samples from the start of the stream, of the last non-finite
/// output sample, or -1 when the run is wholly finite. Both channels are held
/// end to end, so the column within a block is the sample index in either.
int last_non_finite_sample(const Blocks& poisoned) {
  int last = -1;
  for (size_t k = 0; k < poisoned.size(); ++k) {
    for (size_t i = 0; i < poisoned[k].size(); ++i) {
      if (!std::isfinite(poisoned[k][i])) {
        const int column = static_cast<int>(i) % kBlockSize;
        last = std::max(last, static_cast<int>(k) * kBlockSize + column);
      }
    }
  }
  return last;
}

/// The load-bearing half of the invariant, and the half that does not depend on
/// floating-point luck: the sample stains no block past the one that carried it.
/// None of these owners holds a delay long enough to replay it into the next.
void require_non_finite_bounded(const Blocks& poisoned) {
  int last_non_finite = -1;
  int non_finite_blocks = 0;
  for (size_t k = 0; k < poisoned.size(); ++k) {
    if (block_has_non_finite(poisoned[k])) {
      last_non_finite = static_cast<int>(k);
      ++non_finite_blocks;
    }
  }
  // The block count is what the claim is written against; the sample distance is
  // what the owner's own tail costs, and it is the number to compare against a
  // filter length or a lookahead rather than against a block size.
  const int poison_sample = kPoisonBlock * kBlockSize + kPoisonIndex;
  INFO("non-finite blocks " << non_finite_blocks << ", last at block " << last_non_finite
                            << ", last non-finite sample "
                            << last_non_finite_sample(poisoned) - poison_sample
                            << " after the poisoned one");
  // Positive control: an implementation that sanitized the input instead of its
  // own state would leave every block finite and pass everything below.
  REQUIRE(non_finite_blocks > 0);
  REQUIRE(last_non_finite <= kPoisonBlock);
}

/// @param recovery_blocks Block by which the stream must be bit-identical to the
///        control again.
void require_rejoins_control(const Blocks& control, const Blocks& poisoned, int recovery_blocks) {
  // Both messages are in scope before either assertion, so a run that never
  // reaches identity still reports how far apart the streams stayed — which is
  // what separates a bound set too tight from a stream that does not converge.
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  INFO("residual " << residual_from(control, poisoned, recovery_blocks));
  INFO("residual over the last ten blocks "
       << residual_from(control, poisoned, static_cast<int>(control.size()) - 10));
  REQUIRE(first_identical_block(control, poisoned) <= recovery_blocks);
  // Bit identity leaves nothing behind it, so the residual past the bound is
  // exactly zero rather than merely small. Read separately: it is the claim a
  // caller cares about, and it does not depend on how identity is scanned for.
  REQUIRE(residual_from(control, poisoned, recovery_blocks) == 0.0);
}

/// Drives one owner three times -- clean, clean at another level, and poisoned
/// -- and asserts the invariant against the first.
void check_owner(const MakeOwner& make, int recovery_blocks, float poison_value) {
  const int block_count = std::max(recovery_blocks + kHorizonSlack, kMinimumCleanBlocks);
  Owner control_owner = make();
  const Blocks control = run_stream(control_owner.process, block_count, 1.0f, 0.0f, false);

  // Non-vacuity, before any recovery result is read, and read over the blocks
  // where identity is claimed rather than over the whole run: an owner whose
  // startup transient is the only thing it ever does satisfies a whole-run
  // reading while resting through every block the bound covers.
  INFO("control effect " << control_effect(control, 1.0f, recovery_blocks));
  REQUIRE(control_effect(control, 1.0f, 0) > 0.0f);
  REQUIRE(control_effect(control, 1.0f, recovery_blocks) > 0.0f);
  // What the owner produces past the bound answers to what it was given there.
  const Blocks quiet = run_stream(make().process, block_count, kSensitivityScale, 0.0f, false);
  REQUIRE(residual_from(control, quiet, recovery_blocks) > 0.0);
  // The instrument itself: two runs of the same owner over the same stream agree
  // to the bit, so a residual below is the poison's and not the harness's.
  REQUIRE(residual_from(control, run_stream(make().process, block_count, 1.0f, 0.0f, false), 0) ==
          0.0);
  // The clean run has to be clean, and it is asserted rather than assumed: the
  // non-vacuity reductions fold with std::max, which answers false to a
  // non-finite operand and drops it instead of propagating it.
  REQUIRE_FALSE(std::any_of(control.begin(), control.end(), block_has_non_finite));

  Owner poisoned_owner = make();
  const Blocks poisoned = run_stream(poisoned_owner.process, block_count, 1.0f, poison_value, true);
  require_non_finite_bounded(poisoned);
  require_rejoins_control(control, poisoned, recovery_blocks);
  if (control_owner.meter && poisoned_owner.meter) {
    // The meter is folded from the values the detector produced, so it reports a
    // stranded detector even over blocks whose audio has already rejoined.
    const float control_meter = control_owner.meter();
    REQUIRE(std::isfinite(control_meter));
    REQUIRE(poisoned_owner.meter() == control_meter);
  }
}

/// Every owner is driven with all three poison values, in a section apiece: a
/// REQUIRE stops at the first failure, so a shared case would hide the second
/// and third behind the first.
void check_owner(const MakeOwner& make, int recovery_blocks) {
  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) { check_owner(make, recovery_blocks, poison_value); }
  }
}

}  // namespace

TEST_CASE("the gate bounds a non-finite sample to its own block", "[mastering][dynamics]") {
  using sonare::mastering::dynamics::Gate;
  using sonare::mastering::dynamics::GateConfig;

  // The sidechain highpass is the cell under test, so it must be enabled: with
  // it bypassed the gate holds no state a non-finite sample can reach, because
  // the detector's std::max fold drops a NaN and its threshold comparison
  // answers an infinity with a fully open gate.
  GateConfig config;
  config.threshold_db = -20.0f;
  config.close_threshold_db = -26.0f;
  config.range_db = -40.0f;
  config.key_hpf_hz = 120.0f;

  const auto make = [config]() {
    auto processor = std::make_shared<Gate>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return Owner{wrap(processor), [processor]() { return processor->last_gain_reduction_db(); }};
  };

  check_owner(make, kGateRecoveryBlocks);
}

TEST_CASE("the pultec equalizer bounds a non-finite sample to its own block", "[mastering][eq]") {
  using sonare::mastering::eq::PultecComponentModel;
  using sonare::mastering::eq::PultecEq;

  const auto make = []() {
    auto processor = std::make_shared<PultecEq>();
    // The component charges exist only in the modelled mode; the curve-only mode
    // runs the enclosed ParametricEq, which returns its own sections.
    processor->set_component_model(PultecComponentModel::Eqp1aWdf);
    processor->set_low_frequency(60.0f);
    processor->set_low_boost(4.0f);
    processor->set_low_attenuation(2.0f);
    processor->set_high_boost(8000.0f, 3.0f, 0.5f);
    processor->set_high_attenuation(10000.0f, 2.0f);
    processor->prepare(kSampleRate, kBlockSize);
    return Owner{wrap(processor), {}};
  };

  check_owner(make, kPultecRecoveryBlocks);
}

TEST_CASE("low end focus bounds a non-finite sample to its own block", "[mastering][spectral]") {
  using sonare::mastering::spectral::LowEndFocus;
  using sonare::mastering::spectral::LowEndFocusConfig;

  LowEndFocusConfig config;
  config.cutoff_hz = 150.0f;
  config.width = 1.5f;
  config.transient_tightness = 0.5f;

  const auto make_with = [config](float subharmonic_amount) {
    LowEndFocusConfig configuration = config;
    configuration.subharmonic_amount = subharmonic_amount;
    return [configuration]() {
      auto processor = std::make_shared<LowEndFocus>(configuration);
      processor->prepare(kSampleRate, kBlockSize);
      return Owner{wrap(processor), {}};
    };
  };

  // The divider's polarity is a latch the low-pass cell's zero crossings toggle.
  // A run that re-enters from rest can settle an odd number of crossings away
  // from its control and hold the subharmonic inverted from then on — the phase
  // a divider starts in, not a cell left stranded — so the two paths are read
  // apart: bit identity with the divider out of the output, and the divider's
  // own contribution as the bound with it in.
  SECTION("without the subharmonic divider") {
    check_owner(make_with(0.0f), kLowEndFocusRecoveryBlocks);
  }

  SECTION("with the subharmonic divider") {
    const int block_count =
        std::max(kLowEndFocusRecoveryBlocks + kHorizonSlack, kMinimumCleanBlocks);
    const Blocks control =
        run_stream(make_with(kSubharmonicAmount)().process, block_count, 1.0f, 0.0f, false);
    const Blocks without = run_stream(make_with(0.0f)().process, block_count, 1.0f, 0.0f, false);
    // Non-vacuity: the divider reaches the output over the blocks the bound
    // covers, so the bound below is a length rather than zero.
    const double contribution = residual_from(control, without, kLowEndFocusRecoveryBlocks);
    INFO("subharmonic contribution " << contribution);
    REQUIRE(contribution > 0.0);
    REQUIRE_FALSE(std::any_of(control.begin(), control.end(), block_has_non_finite));

    for (const float poison_value : poison_values()) {
      DYNAMIC_SECTION("poison " << poison_value) {
        const Blocks poisoned = run_stream(make_with(kSubharmonicAmount)().process, block_count,
                                           1.0f, poison_value, true);
        require_non_finite_bounded(poisoned);
        // An inverted divider costs exactly twice its contribution and nothing
        // more; a stranded cell costs the signal. The two sides are float
        // accumulations of that magnitude, one of them negated, so the bound
        // carries a float's worth of rounding and no other slack.
        const double residual = residual_from(control, poisoned, kLowEndFocusRecoveryBlocks);
        const double bound =
            2.0 * contribution * (1.0 + static_cast<double>(std::numeric_limits<float>::epsilon()));
        INFO("residual " << residual << " against twice the contribution " << bound);
        REQUIRE(residual <= bound);
      }
    }
  }
}

TEST_CASE("the stereo imager bounds a non-finite sample to its own block", "[mastering][stereo]") {
  using sonare::mastering::stereo::Imager;
  using sonare::mastering::stereo::ImagerConfig;

  // The decorrelation allpass runs whatever the settings are, but only reaches
  // the output when widening with a non-zero amount, which is where a stranded
  // stage is observable at all.
  ImagerConfig config;
  config.width = 1.6f;
  config.decorrelation_amount = 1.0f;

  const auto make = [config]() {
    auto processor = std::make_shared<Imager>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return Owner{wrap(processor), {}};
  };

  check_owner(make, kImagerRecoveryBlocks);
}

TEST_CASE("the multiband imager bounds a non-finite sample to its own block",
          "[mastering][multiband]") {
  using sonare::mastering::multiband::MultibandImager;
  using sonare::mastering::multiband::MultibandImagerConfig;

  MultibandImagerConfig config;
  for (auto& band : config.bands) {
    band.width = 1.6f;
    band.decorrelation_amount = 1.0f;
  }

  const auto make = [config]() {
    auto processor = std::make_shared<MultibandImager>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return Owner{wrap(processor), {}};
  };

  check_owner(make, kMultibandImagerRecoveryBlocks);
}

TEST_CASE("the mono maker bounds a non-finite sample to its own block", "[mastering][stereo]") {
  using sonare::mastering::stereo::MonoMaker;
  using sonare::mastering::stereo::MonoMakerConfig;

  MonoMakerConfig config;
  config.amount = 1.0f;
  config.frequency_hz = 150.0f;

  const auto make = [config]() {
    auto processor = std::make_shared<MonoMaker>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return Owner{wrap(processor), {}};
  };

  check_owner(make, kMonoMakerRecoveryBlocks);
}

namespace {

/// Blocks the poison may still be replayed into, from an owner's own reported
/// latency: a stage holding a lookahead delay line hands the carrying sample to
/// its output stage one or more blocks later, and that is the delay flushing
/// rather than a cell stranded.
int first_recovered_block(int latency_samples) {
  return kPoisonBlock + 1 + (std::max(latency_samples, 0) + kBlockSize - 1) / kBlockSize + 1;
}

/// The substitution counter sampled after every block, and the two values the
/// owner publishes, sampled after the run.
struct SubstitutionRun {
  std::vector<std::uint32_t> counts;
  Blocks outputs;
};

template <typename Processor>
SubstitutionRun run_substituting_owner(Processor& processor, int block_count, float poison_value,
                                       bool poison) {
  SubstitutionRun run;
  for (int k = 0; k < block_count; ++k) {
    std::vector<float> left = stream_block(k, 1.0f);
    if (poison && k == kPoisonBlock) {
      left[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    std::vector<float> right(left.size());
    for (size_t i = 0; i < left.size(); ++i) {
      right[i] = kSecondChannelScale * left[i];
    }
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, kChannels, static_cast<int>(left.size()));
    std::vector<float> flat = std::move(left);
    flat.insert(flat.end(), right.begin(), right.end());
    run.outputs.push_back(std::move(flat));
    run.counts.push_back(processor.non_finite_substitution_count());
  }
  return run;
}

/// The claim for an owner that substitutes rather than emitting a non-finite
/// sample. A stranded gain smoother is not visible as a non-finite output — the
/// substitution turns it into silence — so what is read is the counter that
/// records the substitution, and it must stop.
void require_substitution_stops(const SubstitutionRun& poisoned, int recovered_block) {
  REQUIRE(recovered_block < static_cast<int>(poisoned.counts.size()));
  // Positive control: the poison reached this owner at all.
  INFO("count at the carrying block " << poisoned.counts[static_cast<size_t>(kPoisonBlock)]);
  REQUIRE(poisoned.counts[static_cast<size_t>(kPoisonBlock)] > 0);
  const std::uint32_t settled = poisoned.counts[static_cast<size_t>(recovered_block)];
  INFO("count at the recovered block " << settled << ", at the last block "
                                       << poisoned.counts.back());
  REQUIRE(poisoned.counts.back() == settled);
}

}  // namespace

TEST_CASE(
    "the true peak limiter stops substituting after the block that carried a non-finite"
    " sample",
    "[mastering][maximizer]") {
  using sonare::mastering::maximizer::TruePeakLimiter;
  using sonare::mastering::maximizer::TruePeakLimiterConfig;

  TruePeakLimiterConfig config;
  config.ceiling_db = -1.0f;
  config.release_ms = 50.0f;

  const int block_count = std::max(kTruePeakRecoveryBlocks + kHorizonSlack, kMinimumCleanBlocks);

  TruePeakLimiter control(config);
  control.prepare(kSampleRate, kBlockSize);
  const SubstitutionRun clean = run_substituting_owner(control, block_count, 0.0f, false);
  // Non-vacuity: the limiter is working on this fixture, and on a clean stream it
  // substitutes nothing, so any later count is the poison's.
  INFO("clean count " << clean.counts.back());
  REQUIRE(clean.counts.back() == 0u);
  REQUIRE(control_effect(clean.outputs, 1.0f, 0) > 0.0f);
  REQUIRE_FALSE(std::any_of(clean.outputs.begin(), clean.outputs.end(), block_has_non_finite));

  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) {
      TruePeakLimiter processor(config);
      processor.prepare(kSampleRate, kBlockSize);
      const int recovered = first_recovered_block(processor.latency_samples());
      INFO("latency " << processor.latency_samples() << " samples, recovered block " << recovered);
      const SubstitutionRun poisoned =
          run_substituting_owner(processor, block_count, poison_value, true);
      require_substitution_stops(poisoned, recovered);
      // The output is finite throughout — the substitution sees to that — so what
      // is read instead is that the stream rejoins its control to the bit. This
      // is the only claim that reaches the crest peak: it has no accessor, and an
      // infinity left in it pins the release at its shortest, which is a gain
      // trajectory that differs from the control's for the rest of the handle
      // without a single non-finite sample to show for it.
      INFO("first identical " << first_identical_block(clean.outputs, poisoned.outputs) << " of "
                              << block_count);
      REQUIRE(first_identical_block(clean.outputs, poisoned.outputs) <= kTruePeakRecoveryBlocks);
      REQUIRE(residual_from(clean.outputs, poisoned.outputs, kTruePeakRecoveryBlocks) == 0.0);
    }
  }
}

TEST_CASE(
    "the adaptive release publishes a finite release after the block that carried a"
    " non-finite sample",
    "[mastering][maximizer]") {
  using sonare::mastering::maximizer::AdaptiveRelease;
  using sonare::mastering::maximizer::AdaptiveReleaseConfig;

  AdaptiveReleaseConfig config;
  config.ceiling_db = -1.0f;

  const int block_count = std::max(kAdaptiveReleaseBlocks + kHorizonSlack, kMinimumCleanBlocks);

  AdaptiveRelease control(config);
  control.prepare(kSampleRate, kBlockSize);
  const SubstitutionRun clean = run_substituting_owner(control, block_count, 0.0f, false);
  // Non-vacuity: the crest mapping moved the release away from its seed, so the
  // cell under test is one this fixture actually drives.
  INFO("clean release " << control.current_release_ms() << ", clean crest "
                        << control.current_crest_factor());
  REQUIRE(clean.counts.back() == 0u);
  REQUIRE(control.current_release_ms() != config.min_release_ms);
  REQUIRE(control.current_crest_factor() > 0.0f);
  REQUIRE(control_effect(clean.outputs, 1.0f, 0) > 0.0f);

  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) {
      AdaptiveRelease processor(config);
      processor.prepare(kSampleRate, kBlockSize);
      const int recovered = first_recovered_block(processor.latency_samples());
      INFO("latency " << processor.latency_samples() << " samples, recovered block " << recovered);
      const SubstitutionRun poisoned =
          run_substituting_owner(processor, block_count, poison_value, true);
      require_substitution_stops(poisoned, recovered);
      // The release is handed to the inner limiter on a control grid inside the
      // block, so a stranded one is not merely reported wrong: it reconfigures
      // the limiter for the rest of the handle. The crest factor is read as well
      // and it is the sharper of the two — it divides by the peak envelope, so an
      // infinity there reaches this accessor and nothing else, the audio staying
      // finite the whole time.
      INFO("poisoned release " << processor.current_release_ms() << ", poisoned crest "
                               << processor.current_crest_factor());
      REQUIRE(std::isfinite(processor.current_release_ms()));
      REQUIRE(processor.current_release_ms() >= config.min_release_ms);
      REQUIRE(processor.current_release_ms() <= config.max_release_ms);
      REQUIRE(std::isfinite(processor.current_crest_factor()));
      // And the stream itself rejoins its control to the bit.
      INFO("first identical " << first_identical_block(clean.outputs, poisoned.outputs) << " of "
                              << block_count);
      REQUIRE(first_identical_block(clean.outputs, poisoned.outputs) <= kAdaptiveReleaseBlocks);
      REQUIRE(residual_from(clean.outputs, poisoned.outputs, kAdaptiveReleaseBlocks) == 0.0);
    }
  }
}
