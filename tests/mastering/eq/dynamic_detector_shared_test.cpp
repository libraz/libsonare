/// @file dynamic_detector_shared_test.cpp
/// @brief DynamicEq and the EqualizerProcessor dynamic band read one detector.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "util/constants.h"

using namespace sonare::mastering::eq;

namespace {

constexpr int kSampleRate = 48000;
constexpr size_t kBlock = 480;
// Both sites run the same double-precision detector, so the readings (float dB values) agree to
// well under a thousandth of the ~1e-5 dB a float-designed band-pass moves them.
constexpr float kToleranceDb = 1.0e-6f;

/// A low, narrow side-chain band (high sensitivity to coefficient rounding) over a swept tone.
std::vector<float> sweep_signal() {
  std::vector<float> out(kSampleRate / 2);
  double phase = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    phase += sonare::constants::kTwoPiD * (80.0 + 400.0 * t) / kSampleRate;
    out[i] = 0.4f * static_cast<float>(std::sin(phase)) * (i < out.size() / 2 ? 1.0f : 0.25f);
  }
  return out;
}

DynamicEqBand dynamic_eq_band() {
  DynamicEqBand band{EqBandType::Peak, 120.0f, 0.0f, 2.0f, -60.0f, 4.0f, -12.0f, true};
  band.sidechain_q = 6.0f;
  band.sidechain_freq_hz = 150.0f;
  band.attack_ms = 2.0f;
  band.release_ms = 40.0f;
  band.detector_delay_ms = 3.0f;
  return band;
}

EqBand equalizer_band() {
  EqBand band(EqBandType::Peak, 120.0f, 0.0f, 2.0f, true);
  band.dyn.enabled = true;
  band.dyn.threshold_db = -60.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.sidechain_q = 6.0f;
  band.dyn.sidechain_freq_hz = 150.0f;
  band.dyn.attack_ms = 2.0f;
  band.dyn.release_ms = 40.0f;
  band.dyn.detector_delay_ms = 3.0f;
  return band;
}

}  // namespace

TEST_CASE("DynamicEq and EqualizerProcessor share one band detector",
          "[mastering][eq][dynamic][regression]") {
  DynamicEq dynamic_eq;
  dynamic_eq.prepare(kSampleRate, static_cast<int>(kBlock));
  dynamic_eq.set_band(0, dynamic_eq_band());
  EqualizerProcessor equalizer;
  equalizer.prepare(kSampleRate, static_cast<int>(kBlock));
  equalizer.set_band(0, equalizer_band());

  auto for_dynamic = sweep_signal();
  auto for_equalizer = for_dynamic;
  float worst_db = 0.0f;
  for (size_t offset = 0; offset < for_dynamic.size(); offset += kBlock) {
    const int count = static_cast<int>(std::min(kBlock, for_dynamic.size() - offset));
    float* dynamic_channel = for_dynamic.data() + offset;
    float* equalizer_channel = for_equalizer.data() + offset;
    dynamic_eq.process(&dynamic_channel, 1, count);
    equalizer.process(&equalizer_channel, 1, count);
    worst_db = std::max(worst_db, std::abs(dynamic_eq.last_band_detector_db(0) -
                                           equalizer.last_band_detector_db(0)));
  }
  REQUIRE(worst_db <= kToleranceDb);
}
