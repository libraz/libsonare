/// @file band_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        the four band processors that carry their own recursive cells.
///
/// The crossover's split and compensation sections, the air band's shelf,
/// detector and followers, the presence enhancer's bandpass and the exciter's
/// bandpass, all-pass and DC tracker each apply the rule once per block over
/// their own cells, and each is driven here through its public entry point.
///
/// The block that carried the sample is EXPECTED to come out non-finite; that
/// is the positive control, and asserting it keeps an implementation that
/// quietly sanitizes its input from passing. What is asserted afterwards is
/// that every later block is clean and that the stream rejoins a clean-run
/// control.
///
/// Every case asserts the processor is doing something to the signal before it
/// reads a recovery result. A passthrough recovers instantly for reasons that
/// say nothing about the state under test.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "mastering/saturation/exciter.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/presence_enhancer.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kPoisonBlock = 1;
// Far enough inside the block that every delay reach derived below stays within
// it; a poison near the end of a block shifts each of those reaches one later.
constexpr int kPoisonIndex = 100;

// Blocks the stream is allowed to take to rejoin its control. A cell returns to
// its post-reset value at once; the stream rejoins only once the state behind
// that cell has re-converged, so each bound follows the slowest time constant in
// its path. Bounds sit at roughly twice their measured crossing (144 / 49 / 3 /
// 15): the crossing is where an exponential tail passes one float ULP, and a
// one-ULP difference in the platform's math library moved a comparable fixture's
// by 45%. The last two are a larger multiple of a crossing too small for a
// proportional bound to be worth anything.
constexpr int kCrossoverRecoveryBlocks = 300;
constexpr int kAirBandRecoveryBlocks = 100;
constexpr int kPresenceRecoveryBlocks = 12;
constexpr int kExciterRecoveryBlocks = 40;
// Blocks run past the bound, so a run that misses it still shows how far it got.
constexpr int kHorizonSlack = 20;

/// Fixture with content in every owner's band: below and above both crossover
/// splits, at the presence centre and above the air shelf.
std::vector<float> stream_block(int block_index) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    out[static_cast<size_t>(i)] = static_cast<float>(
        0.30 * std::sin(2.0 * kPiD * 97.0 * t) + 0.30 * std::sin(2.0 * kPiD * 220.0 * t) +
        0.25 * std::sin(2.0 * kPiD * 3200.0 * t) + 0.20 * std::sin(2.0 * kPiD * 7000.0 * t) +
        0.20 * std::sin(2.0 * kPiD * 14000.0 * t));
  }
  return out;
}

using Blocks = std::vector<std::vector<float>>;
/// Takes one input block and returns what the owner emitted for it. An in-place
/// owner returns its own buffer; the crossover returns its bands end to end.
using BlockProcessor = std::function<std::vector<float>(std::vector<float>)>;

std::array<float, 3> poison_values() {
  return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()};
}

/// Runs the stream block by block, optionally poisoning one sample of one block,
/// and returns every output block.
Blocks run_stream(const BlockProcessor& process, int block_count, float poison_value, bool poison) {
  Blocks outputs;
  outputs.reserve(static_cast<size_t>(block_count));
  for (int k = 0; k < block_count; ++k) {
    std::vector<float> block = stream_block(k);
    if (poison && k == kPoisonBlock) {
      block[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    outputs.push_back(process(std::move(block)));
  }
  return outputs;
}

/// The largest change the control run makes to the signal. Zero means the
/// processor is a passthrough and no recovery result from it is meaningful.
float control_effect(const Blocks& control) {
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

/// Index one past the last non-finite sample of @p block, or zero when it is
/// clean.
int non_finite_extent(const std::vector<float>& block) {
  int extent = 0;
  for (size_t i = 0; i < block.size(); ++i) {
    if (!std::isfinite(block[i])) {
      extent = static_cast<int>(i) + 1;
    }
  }
  return extent;
}

/// How far the owner's own structure lets one non-finite sample reach.
struct Reach {
  /// Last block the sample may legitimately stain. The carrying block, unless a
  /// delay line replays that block's tail into the next one.
  int last_block = kPoisonBlock;
  /// Set only where the stain in that block must be delay-line residue, which is
  /// confined to the width of the history holding it. Left unset where a
  /// recursive cell is re-poisoned there: the rule runs at the end of a block, so
  /// such a cell stains the rest of it.
  std::optional<int> confined_to;
};

/// The load-bearing half of the invariant, and the half that does not depend on
/// floating-point luck: the sample stains no block past the window the owner's
/// own structure reaches.
void require_non_finite_bounded(const Blocks& poisoned, const Reach& reach) {
  int last_non_finite = -1;
  int non_finite_blocks = 0;
  for (size_t k = 0; k < poisoned.size(); ++k) {
    if (block_has_non_finite(poisoned[k])) {
      last_non_finite = static_cast<int>(k);
      ++non_finite_blocks;
    }
  }
  const int final_extent = non_finite_extent(poisoned[static_cast<size_t>(reach.last_block)]);
  INFO("non-finite blocks " << non_finite_blocks << ", last at " << last_non_finite << ", extent "
                            << final_extent);
  // Positive control: an implementation that sanitized the input instead of its
  // own state would leave every block finite and pass everything below.
  REQUIRE(non_finite_blocks > 0);
  REQUIRE(last_non_finite <= reach.last_block);
  if (reach.confined_to) {
    REQUIRE(final_extent <= *reach.confined_to);
  }
}

/// @param recovery_blocks Block by which the stream must be bit-identical to the
///        control again. Bounding this is the whole claim: "rejoins eventually"
///        is also true of a handle that rejoins on its last measured block.
void require_rejoins_control(const Blocks& control, const Blocks& poisoned, int recovery_blocks) {
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  REQUIRE(first_identical_block(control, poisoned) <= recovery_blocks);
  // Bit identity leaves nothing behind it, so the residual past the bound is
  // exactly zero rather than merely small. Read separately: it is the claim a
  // caller cares about, and it does not depend on how identity is scanned for.
  INFO("residual " << residual_from(control, poisoned, recovery_blocks));
  REQUIRE(residual_from(control, poisoned, recovery_blocks) == 0.0);
}

using NonVacuity = std::function<void(const Blocks&)>;

/// An in-place owner is non-vacuous when it changed the signal at all.
void require_processor_effect(const Blocks& control) { REQUIRE(control_effect(control) > 0.0f); }

/// Drives one owner twice -- clean and poisoned -- and asserts the invariant.
/// @param make Builds a prepared, configured processor as a block callable.
/// @param reach The window the owner's structure reaches; see Reach.
void check_owner(const std::function<BlockProcessor()>& make, int recovery_blocks,
                 float poison_value, const Reach& reach = {},
                 const NonVacuity& require_non_vacuous = require_processor_effect) {
  const int block_count = recovery_blocks + kHorizonSlack;
  const auto control = run_stream(make(), block_count, 0.0f, false);

  // Non-vacuity, before any recovery result is read.
  require_non_vacuous(control);
  // The clean run has to be clean, and it is asserted rather than assumed: the
  // non-vacuity reductions fold with std::max, which answers false to a
  // non-finite operand and drops it instead of propagating it.
  REQUIRE_FALSE(std::any_of(control.begin(), control.end(), block_has_non_finite));

  const auto poisoned = run_stream(make(), block_count, poison_value, true);
  require_non_finite_bounded(poisoned, reach);
  require_rejoins_control(control, poisoned, recovery_blocks);
}

}  // namespace

TEST_CASE("the crossover bounds a non-finite sample to its own block", "[mastering][multiband]") {
  using sonare::mastering::multiband::Crossover;
  using sonare::mastering::multiband::CrossoverConfig;

  // The shipped three-band Linkwitz-Riley split. Its low and high sections and
  // the compensation all-passes the multi-way sum needs are every recursive cell
  // the owner carries; the linear-phase mode holds none.
  const CrossoverConfig config{};
  const int bands = static_cast<int>(config.cutoffs_hz.size()) + 1;

  const auto make = [config]() {
    auto crossover = std::make_shared<Crossover>(config);
    crossover->prepare(kSampleRate, kBlockSize, 1);
    return BlockProcessor([crossover](std::vector<float> block) {
      float* channels[] = {block.data()};
      const auto split = crossover->split(channels, 1, static_cast<int>(block.size()));
      std::vector<float> flat;
      flat.reserve(block.size() * static_cast<size_t>(split.num_bands()));
      for (const auto& band : split.bands) {
        flat.insert(flat.end(), band[0].begin(), band[0].end());
      }
      return flat;
    });
  };

  // Non-vacuity: every band has to carry signal and none of them may be the
  // input block, or the sections are passing the stream through rather than
  // splitting it.
  const NonVacuity require_split = [bands](const Blocks& control) {
    for (int band = 0; band < bands; ++band) {
      float peak = 0.0f;
      float difference = 0.0f;
      for (size_t k = 0; k < control.size(); ++k) {
        const std::vector<float> source = stream_block(static_cast<int>(k));
        for (int i = 0; i < kBlockSize; ++i) {
          const float value = control[k][static_cast<size_t>(band * kBlockSize + i)];
          peak = std::max(peak, std::abs(value));
          difference = std::max(difference, std::abs(value - source[static_cast<size_t>(i)]));
        }
      }
      INFO("band " << band << " peak " << peak << " difference " << difference);
      REQUIRE(peak > 0.0f);
      REQUIRE(difference > 0.0f);
    }
  };

  for (const float poison_value : poison_values()) {
    INFO("poison value " << poison_value);
    check_owner(make, kCrossoverRecoveryBlocks, poison_value, Reach{}, require_split);
  }
}

TEST_CASE("the air band bounds a non-finite sample to its own block", "[mastering][spectral]") {
  using sonare::mastering::spectral::AirBand;
  using sonare::mastering::spectral::AirBandConfig;

  AirBandConfig config;
  config.amount = 0.8f;
  config.shelf_frequency_hz = 10000.0f;
  config.dynamic_threshold_db = -48.0f;
  config.dynamic_range_db = 6.0f;

  const auto make = [config]() {
    auto processor = std::make_shared<AirBand>(config);
    processor->prepare(kSampleRate, kBlockSize, 1);
    return BlockProcessor([processor](std::vector<float> block) {
      float* channels[] = {block.data()};
      processor->process(channels, 1, static_cast<int>(block.size()));
      return block;
    });
  };

  AirBand probe{config};
  probe.prepare(kSampleRate, kBlockSize, 1);
  // The oversampler's history replays the carrying block's tail into the next
  // block, where the harmonic filter and the followers behind it read it and are
  // re-poisoned, so that block may stain to its end. It is the last: what those
  // cells write reaches the output, never a delay line's input.
  REQUIRE(probe.latency_samples() > 0);
  const Reach reach{kPoisonBlock + 1, std::nullopt};

  for (const float poison_value : poison_values()) {
    INFO("poison value " << poison_value);
    check_owner(make, kAirBandRecoveryBlocks, poison_value, reach);
  }
}

TEST_CASE("the presence enhancer bounds a non-finite sample to its own block",
          "[mastering][spectral]") {
  using sonare::mastering::spectral::PresenceEnhancer;
  using sonare::mastering::spectral::PresenceEnhancerConfig;

  PresenceEnhancerConfig config;
  config.amount = 0.8f;
  config.drive = 4.0f;

  const auto make_from = [](PresenceEnhancerConfig configuration) {
    return [configuration]() {
      auto processor = std::make_shared<PresenceEnhancer>(configuration);
      processor->prepare(kSampleRate, kBlockSize);
      return BlockProcessor([processor](std::vector<float> block) {
        float* channels[] = {block.data()};
        processor->process(channels, 1, static_cast<int>(block.size()));
        return block;
      });
    };
  };

  // The two paths apply the rule at separate call sites, so a green one says
  // nothing about the other.
  SECTION("at the base rate") {
    config.aliasing = sonare::rt::AliasingControl::None;
    for (const float poison_value : poison_values()) {
      INFO("poison value " << poison_value);
      check_owner(make_from(config), kPresenceRecoveryBlocks, poison_value);
    }
  }

  SECTION("oversampled") {
    config.aliasing = sonare::rt::AliasingControl::Oversample4x;
    PresenceEnhancer probe{config};
    probe.prepare(kSampleRate, kBlockSize);
    // The oversampler's history replays the carrying block's tail into the next
    // block. Its residue reaches no recursive cell -- the bandpass sits in front
    // of the oversampler -- so it stains only as far as that history is wide:
    // the round trip plus its two FIR stencils, neither wider than the round trip.
    const Reach reach{kPoisonBlock + 1, 3 * probe.latency_samples()};
    REQUIRE(*reach.confined_to > 0);
    REQUIRE(*reach.confined_to < kBlockSize);
    for (const float poison_value : poison_values()) {
      INFO("poison value " << poison_value);
      check_owner(make_from(config), kPresenceRecoveryBlocks, poison_value, reach);
    }
  }
}

TEST_CASE("the exciter bounds a non-finite sample to its own block", "[mastering][saturation]") {
  using sonare::mastering::saturation::Exciter;
  using sonare::mastering::saturation::ExciterConfig;

  ExciterConfig config;
  config.amount = 0.8f;
  config.drive_db = 12.0f;

  const auto make_from = [](ExciterConfig configuration) {
    return [configuration]() {
      auto processor = std::make_shared<Exciter>(configuration);
      processor->prepare(kSampleRate, kBlockSize);
      return BlockProcessor([processor](std::vector<float> block) {
        float* channels[] = {block.data()};
        processor->process(channels, 1, static_cast<int>(block.size()));
        return block;
      });
    };
  };

  SECTION("at the base rate") {
    config.aliasing = sonare::rt::AliasingControl::None;
    for (const float poison_value : poison_values()) {
      INFO("poison value " << poison_value);
      check_owner(make_from(config), kExciterRecoveryBlocks, poison_value);
    }
  }

  SECTION("oversampled") {
    config.aliasing = sonare::rt::AliasingControl::Oversample4x;
    Exciter probe{config};
    probe.prepare(kSampleRate, kBlockSize);
    // Two blocks of reach, and each one is a stage. The oversampler's history
    // replays the carrying block's tail into the next block, where the DC
    // tracker reads it and is re-poisoned; that tracker writes the downsampler's
    // input, so its block stains to the end and the downsample history replays
    // that tail once more. The third block sees residue only, confined to the
    // width of that history: the round trip plus its two FIR stencils, neither
    // wider than the round trip.
    const Reach reach{kPoisonBlock + 2, 3 * probe.latency_samples()};
    REQUIRE(*reach.confined_to > 0);
    REQUIRE(*reach.confined_to < kBlockSize);
    for (const float poison_value : poison_values()) {
      INFO("poison value " << poison_value);
      check_owner(make_from(config), kExciterRecoveryBlocks, poison_value, reach);
    }
  }
}
