/// @file true_peak_filter_test.cpp
/// @brief BS.1770-style sanity tests for the true-peak interpolation filter.

#include "rt/true_peak_filter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "metering/true_peak.h"
#include "rt/polyphase_fir.h"
#include "rt/true_peak_fir.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
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

TEST_CASE("Polyphase interpolation matches centered convolution for every phase",
          "[rt][truepeak]") {
  sonare::rt::PolyphaseFir fir;
  fir.phases = 2;
  fir.taps_per_phase = 3;
  // Phase-major flat layout: phase 0 is {1, 2, 3}, phase 1 is {4, 5, 6}.
  fir.taps = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  const std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};

  REQUIRE_THAT(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), 3, 0, fir),
               WithinAbs(22.0f, 1.0e-6f));
  REQUIRE_THAT(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), 3, 1, fir),
               WithinAbs(58.0f, 1.0e-6f));
}

TEST_CASE("true-peak FIR factory owns every supported design", "[rt][truepeak]") {
  const auto& unity = sonare::rt::true_peak_fir_for(1);
  REQUIRE(unity.phases == 1);
  REQUIRE(unity.taps_per_phase == 1);
  REQUIRE(TruePeakFilter(1, 1).history_samples() == 1);

  for (const int factor : {2, 4, 8, 16}) {
    CAPTURE(factor);
    REQUIRE(sonare::rt::is_supported_polyphase_oversample_factor(factor));
    const auto& fir = sonare::rt::true_peak_fir_for(factor);
    REQUIRE(fir.phases == factor);
    REQUIRE(fir.taps_per_phase == 12);
    REQUIRE(fir.taps.size() == static_cast<size_t>(factor * fir.taps_per_phase));
    REQUIRE(TruePeakFilter(1, factor).history_samples() == fir.taps_per_phase);
  }

  REQUIRE_FALSE(sonare::rt::is_supported_polyphase_oversample_factor(3));
  REQUIRE_THROWS(sonare::rt::true_peak_fir_for(3));
}

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

TEST_CASE("Four-times true peak stays flat across 12 kHz sample phases", "[rt][truepeak]") {
  constexpr float kSr = 48000.0f;
  constexpr float kFreq = 12000.0f;
  constexpr int kN = 4096;
  constexpr int kPhaseSteps = 32;
  constexpr int kFadeSamples = 64;

  for (int phase_step = 0; phase_step < kPhaseSteps; ++phase_step) {
    const float phase = kTwoPi * static_cast<float>(phase_step) / static_cast<float>(kPhaseSteps);
    std::vector<float> tone(kN, 0.0f);
    for (int n = 0; n < kN; ++n) {
      const float fade_in =
          std::min(1.0f, static_cast<float>(n) / static_cast<float>(kFadeSamples));
      const float fade_out =
          std::min(1.0f, static_cast<float>(kN - 1 - n) / static_cast<float>(kFadeSamples));
      tone[static_cast<size_t>(n)] = std::min(fade_in, fade_out) *
                                     std::sin(kTwoPi * kFreq * static_cast<float>(n) / kSr + phase);
    }

    const float true_peak = sonare::metering::true_peak(tone.data(), tone.size(), 4);
    const float true_peak_db = 20.0f * std::log10(true_peak);
    CAPTURE(phase_step, true_peak, true_peak_db);
    REQUIRE_THAT(true_peak_db, WithinAbs(0.0f, 0.2f));
  }
}
