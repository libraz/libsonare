/// @file pitch_utilities_test.cpp
/// @brief Unit tests for piptrack / pitch_tuning / estimate_tuning.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/pitch.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

Audio make_tone(float freq, int sr, float duration) {
  const size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  const double tp = static_cast<double>(constants::kTwoPi);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(std::sin(tp * static_cast<double>(freq) * t));
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace

TEST_CASE("piptrack returns expected shape", "[pitch_utilities][unit]") {
  Audio audio = make_tone(440.0f, 22050, 0.5f);
  auto r = piptrack(audio, 2048, 512, 150.0f, 4000.0f, 0.1f);
  REQUIRE(r.n_bins > 0);
  REQUIRE(r.n_frames > 0);
  REQUIRE(r.pitches.size() == static_cast<size_t>(r.n_bins * r.n_frames));
  REQUIRE(r.magnitudes.size() == r.pitches.size());
}

TEST_CASE("piptrack finds a peak near 440Hz", "[pitch_utilities][unit]") {
  Audio audio = make_tone(440.0f, 22050, 0.5f);
  auto r = piptrack(audio, 2048, 512, 100.0f, 2000.0f, 0.1f);

  // Find the maximum-magnitude pitch across the matrix.
  float best_mag = 0.0f;
  float best_freq = 0.0f;
  for (size_t i = 0; i < r.pitches.size(); ++i) {
    if (r.magnitudes[i] > best_mag) {
      best_mag = r.magnitudes[i];
      best_freq = r.pitches[i];
    }
  }
  REQUIRE(best_freq > 0.0f);
  // Allow generous tolerance for FFT bin resolution.
  REQUIRE(std::abs(best_freq - 440.0f) < 30.0f);
}

TEST_CASE("pitch_tuning returns near zero for A4-aligned frequencies", "[pitch_utilities][unit]") {
  // Exact 12-TET frequencies (relative to A4=440).
  std::vector<float> freqs = {440.0f, 880.0f, 220.0f, 261.626f, 392.0f};
  float pt = pitch_tuning(freqs, 0.01f, 12);
  REQUIRE(std::abs(pt) <= 0.05f);
}

TEST_CASE("pitch_tuning detects a deliberate shift", "[pitch_utilities][unit]") {
  // Shift everything by ~+0.25 of a bin (25 cents).
  // For bins_per_octave=12 and resolution 0.01, a peak near 0.25 is expected.
  const float shift_cents = 25.0f;
  const float factor = std::pow(2.0f, shift_cents / 1200.0f);
  std::vector<float> freqs;
  for (float f : {440.0f, 880.0f, 220.0f, 261.626f}) freqs.push_back(f * factor);
  float pt = pitch_tuning(freqs, 0.01f, 12);
  REQUIRE(pt > 0.0f);
  REQUIRE(pt < 0.5f);
}

TEST_CASE("pitch_tuning returns 0 for empty input", "[pitch_utilities][unit]") {
  std::vector<float> empty;
  float pt = pitch_tuning(empty, 0.01f, 12);
  REQUIRE_THAT(pt, WithinAbs(0.0f, 1e-9f));
}

TEST_CASE("estimate_tuning returns a finite small value for A4 tone", "[pitch_utilities][unit]") {
  Audio audio = make_tone(440.0f, 22050, 1.0f);
  float et = estimate_tuning(audio, 2048, 512);
  REQUIRE(std::isfinite(et));
  REQUIRE(std::abs(et) <= 0.5f);
}
