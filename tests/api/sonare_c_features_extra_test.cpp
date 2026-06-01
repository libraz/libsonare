/// @file sonare_c_features_extra_test.cpp
/// @brief Tests for the spectral-contrast / poly-features / zero-crossings /
///        tuning C API functions added on top of the core feature wrappers.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sonare_c.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 22050;

// Generates a mono sine wave.
std::vector<float> make_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] = std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq *
                          static_cast<float>(i) / static_cast<float>(sample_rate));
  }
  return samples;
}

// Returns a non-null sentinel so we can assert the wrapper clears outputs.
float* non_null_floats() { return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1)); }
int* non_null_ints() { return reinterpret_cast<int*>(static_cast<std::uintptr_t>(0x1)); }

}  // namespace

TEST_CASE("sonare_spectral_contrast", "[c_api][features]") {
  auto samples = make_sine(440.0f, kSampleRate, 1.0f);

  SECTION("returns [(n_bands + 1) x n_frames] matrix") {
    const int n_bands = 6;
    float* out = nullptr;
    int rows = 0;
    int cols = 0;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512,
                                     n_bands, 200.0f, 0.02f, &out, &rows, &cols) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(rows == n_bands + 1);
    REQUIRE(cols > 0);
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (size_t i = 0; i < total; ++i) {
      REQUIRE(std::isfinite(out[i]));
    }
    sonare_free_floats(out);
  }

  SECTION("rejects null outputs without writing") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 6,
                                     200.0f, 0.02f, nullptr, &rows,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 6,
                                     200.0f, 0.02f, &out, nullptr,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    // Null samples is an invalid parameter; outputs must be cleared.
    REQUIRE(sonare_spectral_contrast(nullptr, samples.size(), kSampleRate, 2048, 512, 6, 200.0f,
                                     0.02f, &out, &rows, &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);

    // n_bands <= 0 propagates as invalid parameter from the C++ layer.
    out = non_null_floats();
    rows = 7;
    cols = 7;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 0,
                                     200.0f, 0.02f, &out, &rows,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);
  }
}

TEST_CASE("sonare_poly_features", "[c_api][features]") {
  auto samples = make_sine(440.0f, kSampleRate, 1.0f);

  SECTION("returns [(order + 1) x n_frames] matrix") {
    const int order = 2;
    float* out = nullptr;
    int rows = 0;
    int cols = 0;
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, order,
                                 &out, &rows, &cols) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(rows == order + 1);
    REQUIRE(cols > 0);
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (size_t i = 0; i < total; ++i) {
      REQUIRE(std::isfinite(out[i]));
    }
    sonare_free_floats(out);
  }

  SECTION("rejects null outputs without writing") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, 1, nullptr,
                                 &rows, &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, 1, &out,
                                 &rows, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_poly_features(nullptr, samples.size(), kSampleRate, 2048, 512, 1, &out, &rows,
                                 &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);
  }
}

TEST_CASE("sonare_zero_crossings", "[c_api][features]") {
  // Alternating-sign signal: a sign change occurs on every sample.
  std::vector<float> signal(16);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  }

  SECTION("returns sorted crossing indices") {
    int* out = nullptr;
    size_t count = 0;
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, &out, &count) == SONARE_OK);
    REQUIRE(count > 0);
    REQUIRE(out != nullptr);
    // pad=1 reports index 0; an alternating signal crosses on every later index.
    REQUIRE(out[0] == 0);
    for (size_t i = 1; i < count; ++i) {
      REQUIRE(out[i] > out[i - 1]);
    }
    sonare_free_ints(out);
  }

  SECTION("rejects null outputs without writing") {
    int* out = non_null_ints();
    size_t count = 7;
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, &out, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    int* out = non_null_ints();
    size_t count = 7;
    // Negative threshold is invalid.
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), -1.0f, 0, 1, 1, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);

    // Null samples with non-zero length is invalid.
    out = non_null_ints();
    count = 7;
    REQUIRE(sonare_zero_crossings(nullptr, signal.size(), sonare::constants::kEpsilon, 0, 1, 1,
                                  &out, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
  }
}

TEST_CASE("sonare_pitch_tuning", "[c_api][features]") {
  SECTION("returns a tuning offset in (-0.5, 0.5]") {
    // Frequencies exactly on equal-tempered pitches -> tuning near 0.
    std::vector<float> freqs = {440.0f, 880.0f, 220.0f};
    float tuning = -1.0f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 12, &tuning) == SONARE_OK);
    REQUIRE(std::isfinite(tuning));
    REQUIRE(tuning > -0.5f);
    REQUIRE(tuning <= 0.5f);
    REQUIRE(tuning == Catch::Approx(0.0f).margin(0.02f));
  }

  SECTION("rejects null out and invalid params") {
    std::vector<float> freqs = {440.0f};
    float tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 12, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);

    tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.0f, 12, &tuning) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);

    tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 0, &tuning) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);
  }
}

TEST_CASE("sonare_estimate_tuning", "[c_api][features]") {
  auto samples = make_sine(440.0f, kSampleRate, 1.0f);

  SECTION("returns a finite tuning offset") {
    float tuning = -1.0f;
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.01f,
                                   12, &tuning) == SONARE_OK);
    REQUIRE(std::isfinite(tuning));
    REQUIRE(tuning > -0.5f);
    REQUIRE(tuning <= 0.5f);
  }

  SECTION("rejects null out and invalid params") {
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.01f,
                                   12, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    float tuning = 0.25f;
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.0f, 12,
                                   &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);

    tuning = 0.25f;
    REQUIRE(sonare_estimate_tuning(nullptr, samples.size(), kSampleRate, 2048, 512, 0.01f, 12,
                                   &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);
  }
}

// Silence yields all-unvoiced frames, which is exactly where the fill_na flag
// changes the f0 output (NaN when off, 0 when on).
TEST_CASE("sonare_pitch_yin fill_na", "[c_api][features]") {
  std::vector<float> silence(static_cast<size_t>(kSampleRate), 0.0f);

  SECTION("fill_na = 0 leaves unvoiced frames as NaN") {
    SonarePitchResult result{};
    REQUIRE(sonare_pitch_yin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                             0.3f, /*fill_na=*/0, &result) == SONARE_OK);
    REQUIRE(result.n_frames > 0);
    bool any_nan = false;
    for (int i = 0; i < result.n_frames; ++i) {
      if (std::isnan(result.f0[i])) any_nan = true;
    }
    REQUIRE(any_nan);
    sonare_free_pitch_result(&result);
  }

  SECTION("fill_na = 1 replaces unvoiced frames with finite 0") {
    SonarePitchResult result{};
    REQUIRE(sonare_pitch_yin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                             0.3f, /*fill_na=*/1, &result) == SONARE_OK);
    REQUIRE(result.n_frames > 0);
    for (int i = 0; i < result.n_frames; ++i) {
      REQUIRE(std::isfinite(result.f0[i]));
    }
    sonare_free_pitch_result(&result);
  }
}

TEST_CASE("sonare_pitch_pyin fill_na", "[c_api][features]") {
  std::vector<float> silence(static_cast<size_t>(kSampleRate), 0.0f);

  SonarePitchResult nan_result{};
  REQUIRE(sonare_pitch_pyin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                            0.3f, /*fill_na=*/0, &nan_result) == SONARE_OK);
  REQUIRE(nan_result.n_frames > 0);
  bool any_nan = false;
  for (int i = 0; i < nan_result.n_frames; ++i) {
    if (std::isnan(nan_result.f0[i])) any_nan = true;
  }
  REQUIRE(any_nan);
  sonare_free_pitch_result(&nan_result);

  SonarePitchResult filled{};
  REQUIRE(sonare_pitch_pyin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                            0.3f, /*fill_na=*/1, &filled) == SONARE_OK);
  for (int i = 0; i < filled.n_frames; ++i) {
    REQUIRE(std::isfinite(filled.f0[i]));
  }
  sonare_free_pitch_result(&filled);
}

TEST_CASE("sonare_analyze_timbre exposes timbre_over_time", "[c_api][features]") {
  auto samples = make_sine(440.0f, kSampleRate, 2.0f);

  SonareTimbreResult result{};
  REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), kSampleRate, 2048, 512, 128, 13,
                                0.5f, &result) == SONARE_OK);

  // A 2s signal with a 0.5s window must yield at least one per-window snapshot,
  // and the pointer/count pair must be consistent.
  REQUIRE(result.timbre_over_time_count > 0);
  REQUIRE(result.timbre_over_time != nullptr);
  for (size_t i = 0; i < result.timbre_over_time_count; ++i) {
    const SonareTimbreFrame& frame = result.timbre_over_time[i];
    REQUIRE(std::isfinite(frame.brightness));
    REQUIRE(std::isfinite(frame.warmth));
    REQUIRE(std::isfinite(frame.density));
    REQUIRE(std::isfinite(frame.roughness));
    REQUIRE(std::isfinite(frame.complexity));
  }

  sonare_free_timbre_result(&result);

  // Double-free / free-after-clear must be safe (pointers nulled on free).
  sonare_free_timbre_result(&result);
  REQUIRE(result.timbre_over_time == nullptr);
  REQUIRE(result.timbre_over_time_count == 0);
}
