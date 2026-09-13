/// @file eq_non_finite_recovery_test.cpp
/// @brief One non-finite input sample must not outlive the block that carried it.
///
/// Every recursive state cell in the equalizer obeys one rule: a cell that has
/// taken a non-finite value is returned to its post-reset value before the next
/// block. Four families of cell reach across a block boundary and each is
/// checked on its own — the biquad history, the dynamic-band detector, the
/// auto-threshold follower and the auto-gain smoother.
///
/// The block that carried the sample is EXPECTED to come out non-finite; that is
/// the positive control, and asserting it keeps an implementation that quietly
/// sanitizes its input from passing. What is asserted afterwards is that every
/// later block is clean and that the stream rejoins a clean-run control exactly.
///
/// Every case asserts the equalizer is doing something to the signal before it
/// reads a recovery result. An unconfigured equalizer is a passthrough, and a
/// passthrough recovers instantly for reasons that say nothing about the state
/// under test.

#include <limits>

#include "eq_test_helpers.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;

// Blocks the stream is allowed to take to rejoin its control. A cell returns to
// its post-reset value at once; the stream rejoins only once the smoother behind
// that cell has re-converged, so each bound follows the slowest time constant in
// its path. Bounds sit at roughly twice their measured crossing (162 / 25 / 261 /
// 179 / 4): the crossing is where an exponential tail passes one float ULP, and a
// one-ULP difference in the platform's sin() moved the same fixture's by 45%.
constexpr int kBiquadRecoveryBlocks = 340;
constexpr int kDetectorRecoveryBlocks = 60;
constexpr int kAutoThresholdRecoveryBlocks = 530;
constexpr int kAutoGainRecoveryBlocks = 380;
constexpr int kLinearPhaseRecoveryBlocks = 12;
// The bare section and the cut filter run the same second-order sections as the
// biquad case, so they share its bound rather than carrying their own.
constexpr int kSectionRecoveryBlocks = kBiquadRecoveryBlocks;
// Blocks run past the bound, so a run that misses it still shows how far it got.
constexpr int kHorizonSlack = 20;

std::vector<float> stream_block(int block_index) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    out[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPiD * 440.0 * t) +
                                                     0.25 * std::sin(2.0 * kPiD * 97.0 * t));
  }
  return out;
}

/// Runs the stream block by block, optionally poisoning one sample of one block,
/// and returns every output block.
std::vector<std::vector<float>> run_stream(EqualizerProcessor& eq, int block_count,
                                           float poison_value, bool poison) {
  std::vector<std::vector<float>> outputs;
  outputs.reserve(static_cast<size_t>(block_count));
  for (int k = 0; k < block_count; ++k) {
    std::vector<float> block = stream_block(k);
    if (poison && k == kPoisonBlock) {
      block[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    float* channels[] = {block.data()};
    eq.process(channels, 1, kBlockSize);
    outputs.push_back(std::move(block));
  }
  return outputs;
}

/// The largest change the control run makes to the signal. Zero means the
/// equalizer is a passthrough and no recovery result from it is meaningful.
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

/// First block after the poison whose output is bit-identical to the control
/// run, or the block count when the stream never rejoins it.
int first_identical_block(const std::vector<std::vector<float>>& control,
                          const std::vector<std::vector<float>>& poisoned) {
  const int block_count = static_cast<int>(control.size());
  for (int k = kPoisonBlock + 1; k < block_count; ++k) {
    if (control[static_cast<size_t>(k)] == poisoned[static_cast<size_t>(k)]) {
      return k;
    }
  }
  return block_count;
}

/// Largest difference from the control over the blocks from @p from onward.
double residual_from(const std::vector<std::vector<float>>& control,
                     const std::vector<std::vector<float>>& poisoned, int from) {
  double worst = 0.0;
  for (size_t k = static_cast<size_t>(from); k < control.size(); ++k) {
    for (int i = 0; i < kBlockSize; ++i) {
      worst = std::max(worst, std::abs(static_cast<double>(control[k][static_cast<size_t>(i)]) -
                                       poisoned[k][static_cast<size_t>(i)]));
    }
  }
  return worst;
}

/// The load-bearing half of the invariant, and the half that does not depend on
/// floating-point luck: the sample stains no block past the window the
/// processor's own structure reaches.
/// @param last_affected_block Last block the sample may legitimately reach. The
///        carrying block for a recursive path; a zero-latency path that stains
///        anything later is holding the value in state.
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
  INFO("first identical " << first_identical_block(control, poisoned) << " of " << control.size());
  REQUIRE(first_identical_block(control, poisoned) <= recovery_blocks);
}

void require_bounded(const std::vector<std::vector<float>>& control,
                     const std::vector<std::vector<float>>& poisoned, int last_affected_block,
                     int recovery_blocks) {
  require_non_finite_bounded(poisoned, last_affected_block);
  require_rejoins_control(control, poisoned, recovery_blocks);
}

EqBand peak_band() { return EqBand{EqBandType::Peak, 1000.0f, 9.0f, 1.0f, true}; }

EqBand low_shelf_band() { return EqBand{EqBandType::LowShelf, 120.0f, -6.0f, kButterworthQ, true}; }

EqBand dynamic_peak_band(bool auto_threshold) {
  EqBand band = peak_band();
  band.dyn.enabled = true;
  band.dyn.auto_threshold = auto_threshold;
  band.dyn.threshold_db = -30.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = 12.0f;
  band.dyn.attack_ms = 5.0f;
  band.dyn.release_ms = 80.0f;
  return band;
}

}  // namespace

TEST_CASE("one non-finite sample does not outlive its block in the equalizer's biquad state",
          "[mastering][eq]") {
  const auto configure = [](EqualizerProcessor& eq) {
    eq.set_band(0, peak_band());
    eq.set_band(1, low_shelf_band());
  };

  EqualizerProcessor control_eq({1});
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control = run_stream(control_eq, kBiquadRecoveryBlocks + kHorizonSlack, 0.0f, false);

  // Non-vacuity, before any recovery result is read.
  REQUIRE(control_effect(control) > 0.0f);

  const std::array<float, 3> poison_values{std::numeric_limits<float>::quiet_NaN(),
                                           std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity()};
  for (const float poison_value : poison_values) {
    INFO("poison value " << poison_value);
    EqualizerProcessor poisoned_eq({1});
    poisoned_eq.prepare(kSampleRate, kBlockSize);
    configure(poisoned_eq);
    const auto poisoned =
        run_stream(poisoned_eq, kBiquadRecoveryBlocks + kHorizonSlack, poison_value, true);
    require_bounded(control, poisoned, kPoisonBlock, kBiquadRecoveryBlocks);
  }
}

TEST_CASE("one non-finite sample does not outlive its block in the dynamic-band detector",
          "[mastering][eq]") {
  const auto configure = [](EqualizerProcessor& eq) { eq.set_band(0, dynamic_peak_band(false)); };

  EqualizerProcessor control_eq({1});
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control = run_stream(control_eq, kDetectorRecoveryBlocks + kHorizonSlack, 0.0f, false);
  const float control_gain_db = control_eq.last_applied_gain_db(0);

  REQUIRE(control_effect(control) > 0.0f);
  // The detector has to be moving the band off its static gain, or its state is
  // not under test.
  REQUIRE(control_gain_db != peak_band().gain_db);

  EqualizerProcessor poisoned_eq({1});
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  configure(poisoned_eq);
  const auto poisoned = run_stream(poisoned_eq, kDetectorRecoveryBlocks + kHorizonSlack,
                                   std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, kPoisonBlock, kDetectorRecoveryBlocks);
  REQUIRE(poisoned_eq.last_applied_gain_db(0) == control_gain_db);
}

TEST_CASE("one non-finite sample does not outlive its block in the auto-threshold follower",
          "[mastering][eq]") {
  // The follower is reinitialised by a floor-sentinel comparison, which a
  // non-finite value answers false to forever, so it needs the rule even though
  // it looks like it reseeds itself.
  const auto configure = [](EqualizerProcessor& eq) { eq.set_band(0, dynamic_peak_band(true)); };

  EqualizerProcessor control_eq({1});
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control =
      run_stream(control_eq, kAutoThresholdRecoveryBlocks + kHorizonSlack, 0.0f, false);
  const float control_gain_db = control_eq.last_applied_gain_db(0);

  REQUIRE(control_effect(control) > 0.0f);
  REQUIRE(control_gain_db != peak_band().gain_db);

  EqualizerProcessor poisoned_eq({1});
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  configure(poisoned_eq);
  const auto poisoned = run_stream(poisoned_eq, kAutoThresholdRecoveryBlocks + kHorizonSlack,
                                   std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, kPoisonBlock, kAutoThresholdRecoveryBlocks);
  REQUIRE(poisoned_eq.last_applied_gain_db(0) == control_gain_db);
}

TEST_CASE("one non-finite sample does not outlive its block in the auto-gain smoother",
          "[mastering][eq]") {
  const auto configure = [](EqualizerProcessor& eq) {
    eq.set_band(0, peak_band());
    eq.set_band(1, low_shelf_band());
    eq.set_auto_gain_enabled(true);
  };

  EqualizerProcessor control_eq({1});
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control = run_stream(control_eq, kAutoGainRecoveryBlocks + kHorizonSlack, 0.0f, false);
  const float control_auto_gain_db = control_eq.last_auto_gain_db();

  REQUIRE(control_effect(control) > 0.0f);
  // Auto-gain has to be compensating something, or its smoother is not under test.
  REQUIRE(control_auto_gain_db != 0.0f);

  EqualizerProcessor poisoned_eq({1});
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  configure(poisoned_eq);
  const auto poisoned = run_stream(poisoned_eq, kAutoGainRecoveryBlocks + kHorizonSlack,
                                   std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, kPoisonBlock, kAutoGainRecoveryBlocks);
  REQUIRE(poisoned_eq.last_auto_gain_db() == control_auto_gain_db);
}

TEST_CASE("a linear-phase equalizer stream rejoins its control after a non-finite sample",
          "[mastering][eq]") {
  // The FIR path carries a bounded delay line rather than recursive state, so it
  // recovers on its own. It is the control class for the cases above: the sample
  // stains the blocks its overlap-add window reaches and no block past them.
  const auto configure = [](EqualizerProcessor& eq) {
    eq.set_phase_mode(PhaseMode::LinearPhase);
    eq.set_band(0, peak_band());
    eq.set_band(1, low_shelf_band());
  };

  EqualizerProcessor control_eq({1});
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control = run_stream(control_eq, kDetectorRecoveryBlocks + kHorizonSlack, 0.0f, false);
  // One block for the window the sample lands in, plus the blocks its latency
  // carries it into, plus the partial block that latency straddles.
  const int last_affected_block =
      kPoisonBlock + 1 + (control_eq.latency_samples() + kBlockSize - 1) / kBlockSize;

  REQUIRE(control_effect(control) > 0.0f);

  EqualizerProcessor poisoned_eq({1});
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  configure(poisoned_eq);
  const auto poisoned = run_stream(poisoned_eq, kDetectorRecoveryBlocks + kHorizonSlack,
                                   std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, last_affected_block, kLinearPhaseRecoveryBlocks);
}

TEST_CASE("the shared parametric section bounds a non-finite sample to its own block",
          "[mastering][eq]") {
  // Every processor that equalizes through this class inherits the rule from
  // here rather than from its own wrapper, so it is checked on the bare section.
  const auto configure = [](ParametricEq& eq) {
    eq.set_band(0, peak_band());
    eq.set_band(1, low_shelf_band());
  };
  const auto run = [&](ParametricEq& eq, float poison_value, bool poison) {
    std::vector<std::vector<float>> outputs;
    for (int k = 0; k < kSectionRecoveryBlocks + kHorizonSlack; ++k) {
      std::vector<float> block = stream_block(k);
      if (poison && k == kPoisonBlock) {
        block[static_cast<size_t>(kPoisonIndex)] = poison_value;
      }
      float* channels[] = {block.data()};
      eq.process(channels, 1, kBlockSize);
      outputs.push_back(std::move(block));
    }
    return outputs;
  };

  ParametricEq control_eq;
  control_eq.prepare(kSampleRate, kBlockSize);
  control_eq.prepare_channels(1);
  configure(control_eq);
  const auto control = run(control_eq, 0.0f, false);

  REQUIRE(control_effect(control) > 0.0f);

  ParametricEq poisoned_eq;
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  poisoned_eq.prepare_channels(1);
  configure(poisoned_eq);
  const auto poisoned = run(poisoned_eq, std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, kPoisonBlock, kSectionRecoveryBlocks);
}

TEST_CASE("the cut filter bounds a non-finite sample to its own block", "[mastering][eq]") {
  // CutFilter carries its own cascade rather than equalizing through
  // ParametricEq, so a green section case says nothing about it.
  const auto configure = [](CutFilter& filter) {
    filter.set_high_pass(120.0f, kButterworthQ, CutFilterSlope::Db24PerOct, true);
    filter.set_low_pass(6000.0f, kButterworthQ, CutFilterSlope::Db12PerOct, true);
  };
  const auto run = [&](CutFilter& filter, float poison_value, bool poison) {
    std::vector<std::vector<float>> outputs;
    for (int k = 0; k < kSectionRecoveryBlocks + kHorizonSlack; ++k) {
      std::vector<float> block = stream_block(k);
      if (poison && k == kPoisonBlock) {
        block[static_cast<size_t>(kPoisonIndex)] = poison_value;
      }
      float* channels[] = {block.data()};
      filter.process(channels, 1, kBlockSize);
      outputs.push_back(std::move(block));
    }
    return outputs;
  };

  CutFilter control_filter;
  control_filter.prepare(kSampleRate, kBlockSize);
  control_filter.prepare_channels(1);
  configure(control_filter);
  const auto control = run(control_filter, 0.0f, false);

  REQUIRE(control_effect(control) > 0.0f);

  CutFilter poisoned_filter;
  poisoned_filter.prepare(kSampleRate, kBlockSize);
  poisoned_filter.prepare_channels(1);
  configure(poisoned_filter);
  const auto poisoned = run(poisoned_filter, std::numeric_limits<float>::quiet_NaN(), true);

  require_bounded(control, poisoned, kPoisonBlock, kSectionRecoveryBlocks);
}

TEST_CASE("the dynamic equalizer bounds a non-finite sample to its own block", "[mastering][eq]") {
  // DynamicEq inherits the section rule through its ParametricEq, but its own
  // detector filters and envelope are separate recursive state.
  const auto configure = [](DynamicEq& eq) {
    DynamicEqBand band;
    band.type = EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.static_gain_db = 9.0f;
    band.q = 1.0f;
    band.threshold_db = -30.0f;
    band.ratio = 4.0f;
    band.range_db = -12.0f;
    band.attack_ms = 5.0f;
    band.release_ms = 80.0f;
    band.enabled = true;
    eq.set_band(0, band);
  };
  const auto run = [&](DynamicEq& eq, float poison_value, bool poison) {
    std::vector<std::vector<float>> outputs;
    for (int k = 0; k < kDetectorRecoveryBlocks + kHorizonSlack; ++k) {
      std::vector<float> block = stream_block(k);
      if (poison && k == kPoisonBlock) {
        block[static_cast<size_t>(kPoisonIndex)] = poison_value;
      }
      float* channels[] = {block.data()};
      eq.process(channels, 1, kBlockSize);
      outputs.push_back(std::move(block));
    }
    return outputs;
  };

  DynamicEq control_eq;
  control_eq.prepare(kSampleRate, kBlockSize);
  configure(control_eq);
  const auto control = run(control_eq, 0.0f, false);
  const float control_gain_db = control_eq.last_applied_gain_db(0);

  REQUIRE(control_effect(control) > 0.0f);
  // The detector has to be moving the band off its static gain, or its state is
  // not under test.
  REQUIRE(control_gain_db != 9.0f);

  DynamicEq poisoned_eq;
  poisoned_eq.prepare(kSampleRate, kBlockSize);
  configure(poisoned_eq);
  const auto poisoned = run(poisoned_eq, std::numeric_limits<float>::quiet_NaN(), true);

  require_non_finite_bounded(poisoned, kPoisonBlock);
  // The detector and the gain it drives come back exactly.
  REQUIRE(std::isfinite(poisoned_eq.last_band_detector_db(0)));
  REQUIRE(poisoned_eq.last_applied_gain_db(0) == control_gain_db);
  // Bit-identity is not reachable here and its absence is not this defect: the
  // coefficient update is skipped until the gain moves kGainEpsilonDb, so the two
  // runs latch coefficients at different points inside that band and hold them.
  // The residual that leaves floors near 1e-5 rather than decaying to zero.
  REQUIRE(residual_from(control, poisoned, kDetectorRecoveryBlocks) < 1.0e-4);
}
