/// @file true_peak_filter_test.cpp
/// @brief BS.1770-style sanity tests for the true-peak interpolation filter.

#include "rt/true_peak_filter.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using sonare::constants::kTwoPi;
using sonare::rt::TruePeakFilter;

namespace {

// Largest absolute raw sample value in a buffer.
float raw_sample_peak(const std::vector<float>& x) {
  float peak = 0.0f;
  for (float v : x) peak = std::max(peak, std::abs(v));
  return peak;
}

}  // namespace

TEST_CASE("TruePeakFilter resolves inter-sample peaks of a 997 Hz tone", "[rt][truepeak]") {
  constexpr float kSr = 48000.0f;
  constexpr float kFreq = 997.0f;
  constexpr int kN = 4800;  // 100 ms
  // -3 dBFS amplitude so inter-sample peaks can rise above the raw samples
  // without the tone itself clipping.
  const float amplitude = std::pow(10.0f, -3.0f / 20.0f);

  std::vector<float> tone(kN);
  // Phase offset chosen so true sample maxima fall between grid points.
  const float phase = 0.37f;
  for (int n = 0; n < kN; ++n) {
    tone[n] = amplitude * std::sin(kTwoPi * kFreq * static_cast<float>(n) / kSr + phase);
  }

  TruePeakFilter filter(1, 4);
  REQUIRE(filter.factor() == 4);

  const float* mono[] = {tone.data()};
  const float true_peak = filter.process(mono, 1, kN);
  const float sample_peak = raw_sample_peak(tone);

  CAPTURE(sample_peak, true_peak, amplitude);
  // Core BS.1770 property: the reconstructed inter-sample peak must be at least
  // the raw sample peak. It stays within a loose bound of the analog envelope;
  // the windowed-sinc reconstruction adds some overshoot, so allow margin.
  REQUIRE(true_peak >= sample_peak);
  REQUIRE(true_peak <= amplitude * 1.2f);
}

TEST_CASE("TruePeakFilter never under-reports a DC signal", "[rt][truepeak]") {
  constexpr int kN = 256;
  std::vector<float> dc(kN, 0.5f);
  TruePeakFilter filter(1, 4);

  const float* mono[] = {dc.data()};
  const float true_peak = filter.process(mono, 1, kN);
  CAPTURE(true_peak);
  // Interpolation must not lose the DC level; small FIR ripple may add overshoot.
  REQUIRE(true_peak >= 0.5f);
  REQUIRE(true_peak <= 0.6f);
}

TEST_CASE("TruePeakFilter handles empty input and rejects bad factors", "[rt][truepeak]") {
  TruePeakFilter filter(1, 4);
  REQUIRE(filter.process(nullptr, 0, 0) == 0.0f);

  std::vector<float> data(16, 1.0f);
  const float* mono[] = {data.data()};
  REQUIRE(filter.process(mono, 1, 0) == 0.0f);

  REQUIRE_THROWS(TruePeakFilter(1, 3));
}
