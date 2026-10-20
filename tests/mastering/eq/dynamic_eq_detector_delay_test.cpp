/// @file dynamic_eq_detector_delay_test.cpp
/// @brief Detector-delay validation and saturation for DynamicEq and EqualizerProcessor.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare::mastering::eq;
using sonare::ErrorCode;
using sonare::SonareException;
using sonare::test::max_abs_difference;

namespace {

constexpr int kSampleRate = 48000;
constexpr float kMaxDelayMs = 20.0f;
constexpr float kHugeDelayMs = 1.0e9f;  // x 48 samples/ms exceeds INT_MAX samples

/// Silence, then a loud burst, so a detector delay changes when the cut engages.
std::vector<float> burst_signal() {
  std::vector<float> out(kSampleRate / 2, 0.0f);
  for (size_t i = out.size() / 4; i < out.size(); ++i) {
    out[i] = 0.5f * static_cast<float>(std::sin(2.0 * sonare::constants::kPiD * 1000.0 *
                                                static_cast<double>(i) / kSampleRate));
  }
  return out;
}

DynamicEqBand dynamic_eq_band(float delay_ms) {
  DynamicEqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -30.0f, 4.0f, -12.0f, true};
  band.attack_ms = 1.0f;
  band.release_ms = 20.0f;
  band.detector_delay_ms = delay_ms;
  return band;
}

EqBand equalizer_band(float delay_ms) {
  EqBand band(EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true);
  band.dyn.enabled = true;
  band.dyn.threshold_db = -30.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 1.0f;
  band.dyn.release_ms = 20.0f;
  band.dyn.detector_delay_ms = delay_ms;
  return band;
}

/// Blocks stay within the prepared size, and the onset falls inside a block, so a detector
/// delay changes which block first sees the burst.
void process_blocks(sonare::rt::ProcessorBase& processor, std::vector<float>& samples) {
  constexpr size_t kBlock = 512;
  for (size_t offset = 0; offset < samples.size(); offset += kBlock) {
    float* channel = samples.data() + offset;
    const int count = static_cast<int>(std::min(kBlock, samples.size() - offset));
    processor.process(&channel, 1, count);
  }
}

std::vector<float> run_dynamic_eq(float delay_ms, bool via_set_parameter = false) {
  DynamicEq eq;
  eq.prepare(kSampleRate, 512);
  if (via_set_parameter) {
    eq.set_band(0, dynamic_eq_band(0.0f));
    REQUIRE(eq.set_parameter(DynamicEq::kParamsPerBand * 0 + 10, delay_ms));
  } else {
    eq.set_band(0, dynamic_eq_band(delay_ms));
  }
  auto samples = burst_signal();
  process_blocks(eq, samples);
  return samples;
}

std::vector<float> run_equalizer(float delay_ms) {
  EqualizerProcessor eq;
  eq.prepare(kSampleRate, 512);
  eq.set_band(0, equalizer_band(delay_ms));
  auto samples = burst_signal();
  process_blocks(eq, samples);
  return samples;
}

}  // namespace

TEST_CASE("DynamicEq refuses a non-finite detector delay", "[mastering][eq][boundary]") {
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity()};
  for (float delay_ms : bad) {
    INFO("detector_delay_ms = " << delay_ms);
    DynamicEq eq;
    eq.prepare(kSampleRate, 512);
    try {
      eq.set_band(0, dynamic_eq_band(delay_ms));
      FAIL("set_band accepted a non-finite detector delay");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
    REQUIRE_THROWS_AS(DynamicEq::validate_band(dynamic_eq_band(delay_ms)), SonareException);
    // Automation path: refused rather than stored.
    eq.set_band(0, dynamic_eq_band(0.0f));
    REQUIRE_FALSE(eq.set_parameter(10, delay_ms));
  }
}

TEST_CASE("EqualizerProcessor refuses a non-finite detector delay", "[mastering][eq][boundary]") {
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity()};
  for (float delay_ms : bad) {
    INFO("detector_delay_ms = " << delay_ms);
    EqualizerProcessor eq;
    eq.prepare(kSampleRate, 512);
    try {
      eq.set_band(0, equalizer_band(delay_ms));
      FAIL("set_band accepted a non-finite detector delay");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
    REQUIRE_THROWS_AS(eq.validate_band(equalizer_band(delay_ms)), SonareException);
  }
}

TEST_CASE("DynamicEq detector delay saturates at the maximum", "[mastering][eq][boundary]") {
  const auto at_max = run_dynamic_eq(kMaxDelayMs);
  const auto huge_config = run_dynamic_eq(kHugeDelayMs);
  const auto huge_automated = run_dynamic_eq(kHugeDelayMs, true);
  REQUIRE(max_abs_difference(at_max, huge_config) == 0.0f);
  REQUIRE(max_abs_difference(at_max, huge_automated) == 0.0f);
}

TEST_CASE("EqualizerProcessor detector delay saturates at the maximum",
          "[mastering][eq][boundary]") {
  const auto at_max = run_equalizer(kMaxDelayMs);
  const auto huge = run_equalizer(kHugeDelayMs);
  REQUIRE(max_abs_difference(at_max, huge) == 0.0f);
}

TEST_CASE("Detector delay is audible in the cut timing", "[mastering][eq]") {
  const auto none = run_dynamic_eq(0.0f);
  const auto delayed = run_dynamic_eq(5.0f);
  REQUIRE(max_abs_difference(none, delayed) > 1.0e-4f);

  const auto eq_none = run_equalizer(0.0f);
  const auto eq_delayed = run_equalizer(5.0f);
  REQUIRE(max_abs_difference(eq_none, eq_delayed) > 1.0e-4f);
}
