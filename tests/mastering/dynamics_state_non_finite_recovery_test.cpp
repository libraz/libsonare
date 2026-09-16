/// @file dynamics_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        the dynamics and spectral owners that hold an envelope follower.
///
/// Each owner applies the rule once per block over its own cells: the follower
/// itself, and every recursive cell fed by it — the riders' smoothed gain, the
/// transient shaper's gain state, the spectral shaper's split filters and gain.
///
/// The block that carried the sample is EXPECTED to come out non-finite; that
/// is the positive control, and asserting it keeps an implementation that
/// quietly sanitizes its input from passing. What is asserted afterwards is a
/// value: every later block is bit-identical to a clean-run control, and the
/// owner's meter reads exactly what the clean run left there.
///
/// Every case asserts the owner is both doing something to the signal and
/// answering to it before it reads a recovery result. A passthrough, and an
/// owner that ignores its input, both recover instantly for reasons that say
/// nothing about the state under test.

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

#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"
#include "mastering/spectral/spectral_shaper.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kChannels = 2;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;
// Channel 1 sits below channel 0 so per-channel state stays distinguishable and
// linked detection, which folds the channels together, has something to fold.
constexpr float kSecondChannelScale = 0.7f;
// Level the input sensitivity control runs at. Far enough from unity that every
// owner's level-dependent path lands somewhere else on its curve.
constexpr float kSensitivityScale = 0.25f;

// Blocks the stream is allowed to take to rejoin its control. A cell returns to
// its post-reset value at once; the stream rejoins only once the smoothers
// behind that cell have re-converged, so each bound follows the slowest time
// constant in its path and sits at roughly twice its measured crossing (64 / 66
// / 41 / 75 / 122 / 293 / 32) -- the crossing is where an exponential tail
// passes one float ULP, and that point moves with the platform's math library.
// An infinity sets every one of them: it survives the std::max fold in front of
// a linked detector, where a NaN is dropped and crosses within a few blocks.
constexpr int kExpanderRecoveryBlocks = 130;
constexpr int kUpwardCompressorRecoveryBlocks = 140;
constexpr int kUpwardExpanderRecoveryBlocks = 90;
constexpr int kParallelCompRecoveryBlocks = 160;
constexpr int kTransientShaperRecoveryBlocks = 250;
constexpr int kVocalRiderRecoveryBlocks = 600;
constexpr int kSpectralShaperRecoveryBlocks = 70;
// Blocks run past the bound, so a run that misses it still shows how far it got,
// and more than one tremolo cycle of them: the non-vacuity reading lives in this
// window, and a window inside a single half-cycle finds an owner whose threshold
// that half never crosses resting rather than passing through.
constexpr int kHorizonSlack = 80;
// Floor on the clean run regardless of the bound, so a tight bound does not buy
// itself a short horizon: a stream that rejoins and then diverges again is only
// visible in blocks nobody asked the bound about. Two full tremolo cycles, so
// the blocks past any bound carry level crossings rather than one steady state.
constexpr int kMinimumCleanBlocks = 150;

/// Fixture with content in every owner's detection band, under a tremolo that
/// spends a third of a second near silence and a third at full level, so every
/// level-dependent owner crosses its threshold in both directions throughout the
/// horizon. A shallower or faster one leaves the detectors above every threshold
/// after their startup transient, and an owner at rest rejoins any control.
std::vector<float> stream_block(int block_index, float scale) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    const double tremolo = 0.03 + 0.97 * std::max(0.0, std::sin(2.0 * kPiD * 1.5 * t));
    const double tone =
        0.45 * std::sin(2.0 * kPiD * 220.0 * t) + 0.30 * std::sin(2.0 * kPiD * 440.0 * t) +
        0.30 * std::sin(2.0 * kPiD * 3200.0 * t) + 0.20 * std::sin(2.0 * kPiD * 7000.0 * t);
    out[static_cast<size_t>(i)] = static_cast<float>(scale * tremolo * tone);
  }
  return out;
}

using Blocks = std::vector<std::vector<float>>;
/// Takes one block of the mono fixture and returns both output channels end to
/// end, so per-channel state is compared as well as the detector's.
using BlockProcessor = std::function<std::vector<float>(const std::vector<float>&)>;

/// A prepared owner plus the meter it publishes, which is read after a run.
struct Owner {
  BlockProcessor process;
  std::function<float()> meter;
  /// The owner's own discard count, read through the accessor a caller has.
  /// Recovery is asserted on values above; without this the count could stay at
  /// zero for every owner here and every one of those assertions would hold.
  std::function<uint32_t()> discards;
};
using MakeOwner = std::function<Owner()>;

template <typename Processor, typename Config, typename Meter>
MakeOwner make_owner(Config config, Meter meter) {
  return [config, meter]() {
    auto processor = std::make_shared<Processor>(config);
    processor->prepare(kSampleRate, kBlockSize);
    BlockProcessor process = [processor](const std::vector<float>& mono) {
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
    return Owner{std::move(process), [processor, meter]() { return meter(*processor); },
                 [processor]() { return processor->non_finite_discard_count(); }};
  };
}

std::array<float, 3> poison_values() {
  return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()};
}

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
///       and a wholly non-finite run reads as a residual of zero -- a bound this
///       value is compared against would then pass on the worst possible result.
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
  INFO("non-finite blocks " << non_finite_blocks << ", last at " << last_non_finite);
  // Positive control: an implementation that sanitized the input instead of its
  // own state would leave every block finite and pass everything below.
  REQUIRE(non_finite_blocks > 0);
  REQUIRE(last_non_finite <= kPoisonBlock);
}

/// @param recovery_blocks Block by which the stream must be bit-identical to the
///        control again. Bounding this is the whole claim: "rejoins eventually"
///        is also true of a handle that rejoins on its last measured block.
void require_rejoins_control(const Blocks& control, const Blocks& poisoned, int recovery_blocks) {
  // Both messages are in scope before either assertion, so a run that never
  // reaches identity still reports how far apart the streams stayed — which is
  // what separates a bound set too tight from a stream that does not converge.
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  INFO("residual " << residual_from(control, poisoned, recovery_blocks));
  REQUIRE(first_identical_block(control, poisoned) <= recovery_blocks);
  // Bit identity leaves nothing behind it, so the residual past the bound is
  // exactly zero rather than merely small. Read separately: it is the claim a
  // caller cares about, and it does not depend on how identity is scanned for.
  REQUIRE(residual_from(control, poisoned, recovery_blocks) == 0.0);
}

/// Drives one owner three times -- clean, clean at another level, and poisoned
/// -- and asserts the invariant against the first.
uint32_t check_owner(const MakeOwner& make, int recovery_blocks, float poison_value) {
  const int block_count = std::max(recovery_blocks + kHorizonSlack, kMinimumCleanBlocks);
  Owner control_owner = make();
  const Blocks control = run_stream(control_owner.process, block_count, 1.0f, 0.0f, false);
  const float control_meter = control_owner.meter();

  // Non-vacuity, before any recovery result is read, and read over the blocks
  // where identity is claimed rather than over the whole run: an owner whose
  // startup transient is the only thing it ever does satisfies a whole-run
  // reading while resting through every block the bound covers, and an owner at
  // rest rejoins any control it is given. The second reading is the load-bearing
  // one, and it is what the fixture's depth and the horizon's width are for.
  INFO("control effect " << control_effect(control, 1.0f, recovery_blocks));
  REQUIRE(control_effect(control, 1.0f, 0) > 0.0f);
  REQUIRE(control_effect(control, 1.0f, recovery_blocks) > 0.0f);
  // What the owner produces past the bound answers to what it was given there.
  const Blocks quiet = run_stream(make().process, block_count, kSensitivityScale, 0.0f, false);
  REQUIRE(residual_from(control, quiet, recovery_blocks) > 0.0);
  // The clean run has to be clean, and it is asserted rather than assumed: the
  // non-vacuity reductions fold with std::max, which answers false to a
  // non-finite operand and drops it instead of propagating it.
  REQUIRE_FALSE(std::any_of(control.begin(), control.end(), block_has_non_finite));
  REQUIRE(std::isfinite(control_meter));
  // A clean run discards nothing, so a count read after the poisoned run below
  // is the poison's and not the fixture's.
  REQUIRE(control_owner.discards() == 0);

  Owner poisoned_owner = make();
  const Blocks poisoned = run_stream(poisoned_owner.process, block_count, 1.0f, poison_value, true);
  require_non_finite_bounded(poisoned);
  require_rejoins_control(control, poisoned, recovery_blocks);
  // The meter is folded from the values the detector produced, so it reports a
  // stranded detector even over blocks whose audio has already rejoined.
  REQUIRE(poisoned_owner.meter() == control_meter);
  // The published unit is one process() call, and exactly one block carried the
  // sample -- so a count above one is a bump per channel, per cell or per
  // sample, which makes the number depend on the block size. Zero is legal here
  // and is not read as agreement: whether this value reaches the cell at all
  // depends on the fold in front of it, which is why the caller below requires
  // the three values together to drive it.
  const uint32_t discards = poisoned_owner.discards();
  INFO("discards " << discards);
  REQUIRE(discards <= 1);
  return discards;
}

/// Every owner is driven with all three poison values. Which of them can reach a
/// given cell is a property of the path in front of it -- a linked detector
/// folds its channels with std::max, which drops a NaN and passes an infinity --
/// so a single value tests one owner's state and merely its input handling at
/// the next.
void check_owner(const MakeOwner& make, int recovery_blocks) {
  uint32_t reached = 0;
  for (const float poison_value : poison_values()) {
    INFO("poison value " << poison_value);
    reached += check_owner(make, recovery_blocks, poison_value);
  }
  // Un-reach is a failure, not a silence. Every owner here recovers by
  // discarding a cell, so at least one of the three values must drive the count
  // off zero -- an owner whose counter is never bumped satisfies every value
  // assertion above while reporting, to a caller, that it lost nothing.
  INFO("values that reached the count " << reached << " of " << poison_values().size());
  REQUIRE(reached > 0);
}

}  // namespace

TEST_CASE("the expander bounds a non-finite sample to its own block", "[mastering][dynamics]") {
  using sonare::mastering::dynamics::Expander;
  using sonare::mastering::dynamics::ExpanderConfig;

  ExpanderConfig config;
  config.threshold_db = -20.0f;
  config.ratio = 2.0f;
  config.range_db = -40.0f;

  const auto make = make_owner<Expander>(
      config, [](const Expander& owner) { return owner.last_gain_reduction_db(); });

  check_owner(make, kExpanderRecoveryBlocks);
}

TEST_CASE("the upward compressor bounds a non-finite sample to its own block",
          "[mastering][dynamics]") {
  using sonare::mastering::dynamics::UpwardCompressor;
  using sonare::mastering::dynamics::UpwardCompressorConfig;

  UpwardCompressorConfig config;
  config.threshold_db = -20.0f;
  config.ratio = 2.0f;

  const auto make = make_owner<UpwardCompressor>(
      config, [](const UpwardCompressor& owner) { return owner.last_gain_db(); });

  check_owner(make, kUpwardCompressorRecoveryBlocks);
}

TEST_CASE("the upward expander bounds a non-finite sample to its own block",
          "[mastering][dynamics]") {
  using sonare::mastering::dynamics::UpwardExpander;
  using sonare::mastering::dynamics::UpwardExpanderConfig;

  UpwardExpanderConfig config;
  config.threshold_db = -20.0f;

  const auto make = make_owner<UpwardExpander>(
      config, [](const UpwardExpander& owner) { return owner.last_gain_db(); });

  check_owner(make, kUpwardExpanderRecoveryBlocks);
}

TEST_CASE("the parallel compressor bounds a non-finite sample to its own block",
          "[mastering][dynamics]") {
  using sonare::mastering::dynamics::ParallelComp;
  using sonare::mastering::dynamics::ParallelCompConfig;

  ParallelCompConfig config;
  config.threshold_db = -24.0f;

  // The two detection modes advance different followers and apply the rule at
  // separate call sites, so a green one says nothing about the other.
  const auto make_from = [](ParallelCompConfig configuration) {
    return make_owner<ParallelComp>(
        configuration, [](const ParallelComp& owner) { return owner.last_gain_reduction_db(); });
  };

  SECTION("linked detection") {
    config.linked_detection = true;
    check_owner(make_from(config), kParallelCompRecoveryBlocks);
  }

  SECTION("unlinked detection") {
    config.linked_detection = false;
    check_owner(make_from(config), kParallelCompRecoveryBlocks);
  }
}

TEST_CASE("the transient shaper bounds a non-finite sample to its own block",
          "[mastering][dynamics]") {
  using sonare::mastering::dynamics::TransientShaper;
  using sonare::mastering::dynamics::TransientShaperConfig;

  TransientShaperConfig config;
  config.attack_gain_db = 6.0f;
  config.sustain_gain_db = -3.0f;
  config.gain_smoothing_ms = 5.0f;

  const auto make = make_owner<TransientShaper>(
      config, [](const TransientShaper& owner) { return owner.last_gain_db(); });

  check_owner(make, kTransientShaperRecoveryBlocks);
}

TEST_CASE("the vocal rider bounds a non-finite sample to its own block", "[mastering][dynamics]") {
  using sonare::mastering::dynamics::VocalRider;
  using sonare::mastering::dynamics::VocalRiderConfig;

  VocalRiderConfig config;
  config.target_db = -14.0f;

  // The two detection modes hold separate gain state and apply the rule at
  // separate call sites, so a green one says nothing about the other.
  const auto make_from = [](VocalRiderConfig configuration) {
    return make_owner<VocalRider>(configuration,
                                  [](const VocalRider& owner) { return owner.last_gain_db(); });
  };

  SECTION("linked detection") {
    config.linked_detection = true;
    check_owner(make_from(config), kVocalRiderRecoveryBlocks);
  }

  SECTION("unlinked detection") {
    config.linked_detection = false;
    check_owner(make_from(config), kVocalRiderRecoveryBlocks);
  }
}

TEST_CASE("the spectral shaper bounds a non-finite sample to its own block",
          "[mastering][spectral]") {
  using sonare::mastering::spectral::SpectralShaper;
  using sonare::mastering::spectral::SpectralShaperConfig;

  SpectralShaperConfig config;
  config.threshold = 0.05f;
  config.amount = 0.8f;

  const auto make = make_owner<SpectralShaper>(
      config, [](const SpectralShaper& owner) { return owner.last_reduction_db(); });

  check_owner(make, kSpectralShaperRecoveryBlocks);
}
