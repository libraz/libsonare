/// @file sonare_c_effects_extra_test.cpp
/// @brief Tests for the extended C API effects wrappers (decompose, nn_filter,
///        remix, hpss_with_residual, phase_vocoder).

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "util/constants.h"

namespace {

std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i / sample_rate);
  }
  return samples;
}

// A small non-negative spectrogram-like matrix [n_features x n_frames] row-major.
std::vector<float> generate_spectrogram(int n_features, int n_frames) {
  std::vector<float> s(static_cast<size_t>(n_features) * n_frames);
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      s[static_cast<size_t>(f) * n_frames + t] = std::abs(std::sin(0.3f * f + 0.1f * t)) + 0.01f;
    }
  }
  return s;
}

float* non_null_sentinel_float_ptr() {
  return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1));
}

float max_abs_difference(const float* lhs, const float* rhs, size_t length) {
  float result = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    result = std::max(result, std::abs(lhs[i] - rhs[i]));
  }
  return result;
}

}  // namespace

TEST_CASE("configurable C effects preserve legacy defaults", "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  auto samples = generate_sine(440.0f, sample_rate, 0.35f);

  SECTION("time stretch and pitch shift") {
    float* old_stretched = nullptr;
    float* new_stretched = nullptr;
    size_t old_stretched_length = 0;
    size_t new_stretched_length = 0;
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), sample_rate, 0.8f, &old_stretched,
                                &old_stretched_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), sample_rate, 0.8f, 2048, 512,
                                   &new_stretched, &new_stretched_length) == SONARE_OK);
    REQUIRE(old_stretched_length == new_stretched_length);
    REQUIRE(max_abs_difference(old_stretched, new_stretched, old_stretched_length) < 1e-6f);
    sonare_free_floats(old_stretched);
    sonare_free_floats(new_stretched);

    float* old_shifted = nullptr;
    float* new_shifted = nullptr;
    size_t old_shifted_length = 0;
    size_t new_shifted_length = 0;
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), sample_rate, 5.0f, &old_shifted,
                               &old_shifted_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), sample_rate, 5.0f, 2048, 512,
                                  &new_shifted, &new_shifted_length) == SONARE_OK);
    REQUIRE(old_shifted_length == new_shifted_length);
    REQUIRE(max_abs_difference(old_shifted, new_shifted, old_shifted_length) < 1e-6f);
    sonare_free_floats(old_shifted);
    sonare_free_floats(new_shifted);
  }

  SECTION("HPSS and absolute trim") {
    SonareHpssResult old_hpss{};
    SonareHpssResult new_hpss{};
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &old_hpss) ==
            SONARE_OK);
    REQUIRE(sonare_hpss_ex(samples.data(), samples.size(), sample_rate, 31, 31, 2048, 512, 1, 0,
                           &new_hpss, nullptr) == SONARE_OK);
    REQUIRE(old_hpss.length == new_hpss.length);
    REQUIRE(old_hpss.sample_rate == new_hpss.sample_rate);
    REQUIRE(max_abs_difference(old_hpss.harmonic, new_hpss.harmonic, old_hpss.length) < 1e-6f);
    REQUIRE(max_abs_difference(old_hpss.percussive, new_hpss.percussive, old_hpss.length) < 1e-6f);
    sonare_free_hpss_result(&old_hpss);
    sonare_free_hpss_result(&new_hpss);

    std::vector<float> padded(samples.size() + 2048, 0.0f);
    std::copy(samples.begin(), samples.end(), padded.begin() + 1024);
    float* old_trimmed = nullptr;
    float* new_trimmed = nullptr;
    size_t old_trimmed_length = 0;
    size_t new_trimmed_length = 0;
    REQUIRE(sonare_trim(padded.data(), padded.size(), sample_rate, -40.0f, &old_trimmed,
                        &old_trimmed_length) == SONARE_OK);
    REQUIRE(sonare_trim_ex(padded.data(), padded.size(), sample_rate, -40.0f, 2048, 512,
                           &new_trimmed, &new_trimmed_length) == SONARE_OK);
    REQUIRE(old_trimmed_length == new_trimmed_length);
    REQUIRE(max_abs_difference(old_trimmed, new_trimmed, old_trimmed_length) < 1e-6f);
    sonare_free_floats(old_trimmed);
    sonare_free_floats(new_trimmed);
  }
}

TEST_CASE("C effects _ex options reach their native processing paths", "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  auto samples = generate_sine(440.0f, sample_rate, 0.8f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] += 0.25f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1730.0f *
                                   i / sample_rate);
  }

  SECTION("time and pitch FFT settings change the result") {
    float* default_stretched = nullptr;
    float* custom_stretched = nullptr;
    size_t default_stretched_length = 0;
    size_t custom_stretched_length = 0;
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), sample_rate, 0.8f, 2048, 512,
                                   &default_stretched, &default_stretched_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch_ex(samples.data(), samples.size(), sample_rate, 0.8f, 1024, 256,
                                   &custom_stretched, &custom_stretched_length) == SONARE_OK);
    REQUIRE(default_stretched_length == custom_stretched_length);
    REQUIRE(max_abs_difference(default_stretched, custom_stretched, default_stretched_length) >
            1e-5f);
    sonare_free_floats(default_stretched);
    sonare_free_floats(custom_stretched);

    float* default_shifted = nullptr;
    float* custom_shifted = nullptr;
    size_t default_shifted_length = 0;
    size_t custom_shifted_length = 0;
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), sample_rate, 5.0f, 2048, 512,
                                  &default_shifted, &default_shifted_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift_ex(samples.data(), samples.size(), sample_rate, 5.0f, 1024, 256,
                                  &custom_shifted, &custom_shifted_length) == SONARE_OK);
    REQUIRE(default_shifted_length == custom_shifted_length);
    REQUIRE(max_abs_difference(default_shifted, custom_shifted, default_shifted_length) > 1e-5f);
    sonare_free_floats(default_shifted);
    sonare_free_floats(custom_shifted);
  }

  SECTION("HPSS supports hard two-way and three-way routing") {
    SonareHpssResult soft{};
    SonareHpssResult hard{};
    float* ignored_residual = non_null_sentinel_float_ptr();
    REQUIRE(sonare_hpss_ex(samples.data(), samples.size(), sample_rate, 31, 31, 1024, 256, 1, 0,
                           &soft, &ignored_residual) == SONARE_OK);
    REQUIRE(ignored_residual == nullptr);
    REQUIRE(sonare_hpss_ex(samples.data(), samples.size(), sample_rate, 31, 31, 1024, 256, 0, 0,
                           &hard, nullptr) == SONARE_OK);
    REQUIRE(soft.length == hard.length);
    REQUIRE(max_abs_difference(soft.harmonic, hard.harmonic, hard.length) > 1e-5f);
    sonare_free_hpss_result(&soft);
    sonare_free_hpss_result(&hard);

    SonareHpssResult hard_two_way{};
    SonareHpssResult hard_three_way{};
    float* residual = nullptr;
    REQUIRE(sonare_hpss_ex(samples.data(), samples.size(), sample_rate, 31, 31, 1024, 256, 0, 0,
                           &hard_two_way, nullptr) == SONARE_OK);
    REQUIRE(sonare_hpss_ex(samples.data(), samples.size(), sample_rate, 31, 31, 1024, 256, 0, 1,
                           &hard_three_way, &residual) == SONARE_OK);
    REQUIRE(residual != nullptr);
    REQUIRE(hard_two_way.length == hard_three_way.length);
    REQUIRE(hard_three_way.length == samples.size());
    REQUIRE(max_abs_difference(hard_two_way.harmonic, hard_three_way.harmonic,
                               hard_three_way.length) > 1e-5f);
    REQUIRE(max_abs_difference(hard_two_way.percussive, hard_three_way.percussive,
                               hard_three_way.length) > 1e-5f);
    float reconstruction_error = 0.0f;
    float residual_peak = 0.0f;
    for (size_t i = 0; i < hard_three_way.length; ++i) {
      REQUIRE(std::isfinite(hard_two_way.harmonic[i]));
      REQUIRE(std::isfinite(hard_two_way.percussive[i]));
      REQUIRE(std::isfinite(hard_three_way.harmonic[i]));
      REQUIRE(std::isfinite(hard_three_way.percussive[i]));
      REQUIRE(std::isfinite(residual[i]));
      reconstruction_error = std::max(
          reconstruction_error, std::abs(hard_three_way.harmonic[i] + hard_three_way.percussive[i] +
                                         residual[i] - samples[i]));
      residual_peak = std::max(residual_peak, std::abs(residual[i]));
    }
    REQUIRE(residual_peak > 1e-5f);
    REQUIRE(reconstruction_error < 1e-4f);
    sonare_free_hpss_result(&hard_two_way);
    sonare_free_hpss_result(&hard_three_way);
    sonare_free_floats(residual);
  }
}

TEST_CASE("sonare_normalize_rms clips and sonare_trim_ex uses custom framing", "[c_api][effects]") {
  SECTION("RMS normalization clips overshoots") {
    const std::vector<float> samples = {0.8f, -0.8f, 0.8f, -0.8f};
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_normalize_rms(samples.data(), samples.size(), 22050, 0.0f, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(out_length == samples.size());
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == -1.0f);
    for (size_t i = 0; i < out_length; ++i) REQUIRE(std::abs(out[i]) <= 1.0f);
    sonare_free_floats(out);
  }

  SECTION("trim_ex accepts frame and hop overrides") {
    constexpr int sample_rate = 8000;
    std::vector<float> samples(1024, 0.0f);
    for (size_t i = 128; i < 896; ++i) {
      samples[i] = 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * i /
                                   sample_rate);
    }
    float* legacy = nullptr;
    size_t legacy_length = 0;
    REQUIRE(sonare_trim(samples.data(), samples.size(), sample_rate, -40.0f, &legacy,
                        &legacy_length) == SONARE_OK);

    float* custom = nullptr;
    size_t custom_length = 0;
    REQUIRE(sonare_trim_ex(samples.data(), samples.size(), sample_rate, -40.0f, 64, 16, &custom,
                           &custom_length) == SONARE_OK);
    REQUIRE(custom != nullptr);
    REQUIRE(custom_length < samples.size());
    REQUIRE((custom_length != legacy_length ||
             max_abs_difference(custom, legacy, custom_length) > 1e-5f));
    sonare_free_floats(legacy);
    sonare_free_floats(custom);
  }
}

TEST_CASE("effect _ex outputs are cleared before validation", "[c_api][effects]") {
  const float sample = 0.25f;

  float* out = non_null_sentinel_float_ptr();
  size_t out_length = 99;
  REQUIRE(sonare_time_stretch_ex(nullptr, 0, 22050, 0.8f, 2048, 512, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  out = non_null_sentinel_float_ptr();
  out_length = 99;
  REQUIRE(sonare_pitch_shift_ex(&sample, 1, 22050, 0.0f, 2048, 512, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  out = non_null_sentinel_float_ptr();
  out_length = 99;
  REQUIRE(sonare_normalize_rms(nullptr, 0, 22050, -10.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  out = non_null_sentinel_float_ptr();
  out_length = 99;
  REQUIRE(sonare_trim_ex(&sample, 1, 22050, -40.0f, 64, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  SonareHpssResult result{non_null_sentinel_float_ptr(), non_null_sentinel_float_ptr(), 99, 99};
  float* residual = non_null_sentinel_float_ptr();
  REQUIRE(sonare_hpss_ex(nullptr, 0, 22050, 31, 31, 2048, 512, 1, 0, &result, &residual) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.harmonic == nullptr);
  REQUIRE(result.percussive == nullptr);
  REQUIRE(result.length == 0);
  REQUIRE(result.sample_rate == 0);
  REQUIRE(residual == nullptr);

  result = {non_null_sentinel_float_ptr(), non_null_sentinel_float_ptr(), 99, 99};
  REQUIRE(sonare_hpss_ex(&sample, 1, 22050, 31, 31, 2048, 512, 1, 1, &result, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.harmonic == nullptr);
  REQUIRE(result.percussive == nullptr);
  REQUIRE(result.length == 0);
  REQUIRE(result.sample_rate == 0);
}

TEST_CASE("sonare_decompose", "[c_api][effects]") {
  const int n_features = 16;
  const int n_frames = 24;
  const int n_components = 4;
  auto s = generate_spectrogram(n_features, n_frames);

  SECTION("returns W and H matrices of expected size") {
    float* w = nullptr;
    float* h = nullptr;
    size_t w_len = 0;
    size_t h_len = 0;
    REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &w, &w_len, &h,
                             &h_len) == SONARE_OK);
    REQUIRE(w != nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(w_len == static_cast<size_t>(n_features) * n_components);
    REQUIRE(h_len == static_cast<size_t>(n_components) * n_frames);
    for (size_t i = 0; i < w_len; ++i) REQUIRE(std::isfinite(w[i]));
    for (size_t i = 0; i < h_len; ++i) REQUIRE(std::isfinite(h[i]));
    sonare_free_floats(w);
    sonare_free_floats(h);
  }

  SECTION("rejects null outputs and bad dimensions, clearing outputs") {
    REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, nullptr,
                             nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    float* w = non_null_sentinel_float_ptr();
    float* h = non_null_sentinel_float_ptr();
    size_t w_len = 99;
    size_t h_len = 99;
    REQUIRE(sonare_decompose(nullptr, n_features, n_frames, n_components, 20, 2.0f, &w, &w_len, &h,
                             &h_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(w == nullptr);
    REQUIRE(h == nullptr);
    REQUIRE(w_len == 0);
    REQUIRE(h_len == 0);

    w = non_null_sentinel_float_ptr();
    h = non_null_sentinel_float_ptr();
    REQUIRE(sonare_decompose(s.data(), 0, n_frames, n_components, 20, 2.0f, &w, &w_len, &h,
                             &h_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(w == nullptr);
    REQUIRE(h == nullptr);
  }
}

TEST_CASE("sonare_nn_filter", "[c_api][effects]") {
  const int n_features = 16;
  const int n_frames = 24;
  auto s = generate_spectrogram(n_features, n_frames);

  SECTION("returns smoothed spectrogram of identical shape") {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_nn_filter(s.data(), n_features, n_frames, "mean", 3, 1, &out, &out_len) ==
            SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len == static_cast<size_t>(n_features) * n_frames);
    for (size_t i = 0; i < out_len; ++i) REQUIRE(std::isfinite(out[i]));
    sonare_free_floats(out);
  }

  SECTION("NULL aggregate defaults to mean") {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_nn_filter(s.data(), n_features, n_frames, nullptr, 3, 1, &out, &out_len) ==
            SONARE_OK);
    REQUIRE(out_len == static_cast<size_t>(n_features) * n_frames);
    sonare_free_floats(out);
  }

  SECTION("rejects null out and bad dimensions") {
    REQUIRE(sonare_nn_filter(s.data(), n_features, n_frames, "mean", 3, 1, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    float* out = non_null_sentinel_float_ptr();
    size_t out_len = 99;
    REQUIRE(sonare_nn_filter(nullptr, n_features, n_frames, "mean", 3, 1, &out, &out_len) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_len == 0);
  }
}

TEST_CASE("sonare_remix", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("concatenates interval slices") {
    // Two slices: [0, 1000) and [5000, 5500) -> total 1500 samples.
    std::vector<int> intervals = {0, 1000, 5000, 5500};
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_remix(samples.data(), samples.size(), sr, intervals.data(), 2, 0, &out,
                         &out_len) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len == 1500);
    sonare_free_floats(out);
  }

  SECTION("rejects null out and null intervals with count > 0") {
    REQUIRE(sonare_remix(samples.data(), samples.size(), sr, nullptr, 0, 0, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    float* out = non_null_sentinel_float_ptr();
    size_t out_len = 99;
    REQUIRE(sonare_remix(samples.data(), samples.size(), sr, nullptr, 2, 0, &out, &out_len) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_len == 0);
  }
}

TEST_CASE("sonare_remix_aligned_intervals", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 0.5f);

  SECTION("resolves one clamped pair per interval") {
    std::vector<int> intervals = {0, 1000, 5000, 5500};
    int* out = nullptr;
    size_t out_count = 0;
    REQUIRE(sonare_remix_aligned_intervals(samples.data(), samples.size(), sr, intervals.data(), 2,
                                           1, &out, &out_count) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_count == 2);
    for (size_t i = 0; i < out_count; ++i) {
      REQUIRE(out[2 * i] >= 0);
      REQUIRE(out[2 * i + 1] <= static_cast<int>(samples.size()));
      REQUIRE(out[2 * i + 1] > out[2 * i]);
    }
    sonare_free_ints(out);
  }

  SECTION("a signal with no sign change is left unsnapped") {
    std::vector<float> flat(4096, 0.25f);
    std::vector<int> intervals = {100, 200};
    int* out = nullptr;
    size_t out_count = 0;
    REQUIRE(sonare_remix_aligned_intervals(flat.data(), flat.size(), sr, intervals.data(), 1, 1,
                                           &out, &out_count) == SONARE_OK);
    REQUIRE(out_count == 1);
    REQUIRE(out[0] == 100);
    REQUIRE(out[1] == 200);
    sonare_free_ints(out);
  }

  SECTION("rejects null out") {
    std::vector<int> intervals = {0, 100};
    REQUIRE(sonare_remix_aligned_intervals(samples.data(), samples.size(), sr, intervals.data(), 1,
                                           1, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_decompose_stems", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 0.4f);

  SECTION("emits one flat component buffer plus the factorisation") {
    SonareDecomposeStemsConfig config{};
    config.struct_version = 1;
    config.n_components = 2;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.n_iter = 30;
    float* out = nullptr;
    size_t count = 0;
    size_t length = 0;
    float* w = nullptr;
    size_t w_length = 0;
    float* h = nullptr;
    size_t h_length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &count,
                                   &length, &w, &w_length, &h, &h_length) == SONARE_OK);
    REQUIRE(count == 2);
    REQUIRE(length == samples.size());
    REQUIRE(w_length == static_cast<size_t>(config.n_fft / 2 + 1) * count);
    REQUIRE(h_length % count == 0);
    // The masks partition the spectrogram, so the components sum back to the
    // input over the interior where the window overlap is complete.
    double err = 0.0;
    double ref = 0.0;
    for (size_t i = static_cast<size_t>(config.n_fft);
         i + static_cast<size_t>(config.n_fft) < length; ++i) {
      const double sum = out[i] + out[length + i];
      err += (sum - samples[i]) * (sum - samples[i]);
      ref += static_cast<double>(samples[i]) * samples[i];
    }
    REQUIRE(ref > 0.0);
    REQUIRE(std::sqrt(err / ref) < 0.05);
    sonare_free_floats(out);
    sonare_free_floats(w);
    sonare_free_floats(h);
  }

  SECTION("NULL config selects the defaults and W/H are optional") {
    float* out = nullptr;
    size_t count = 0;
    size_t length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, nullptr, &out, &count,
                                   &length, nullptr, nullptr, nullptr, nullptr) == SONARE_OK);
    REQUIRE(count == 4);
    REQUIRE(length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("rejects an unknown struct version and an out-of-range mask power") {
    SonareDecomposeStemsConfig config{};
    config.struct_version = 99;
    float* out = non_null_sentinel_float_ptr();
    size_t count = 99;
    size_t length = 99;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &count,
                                   &length, nullptr, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
    REQUIRE(length == 0);

    config = SonareDecomposeStemsConfig{};
    config.mask_power = 0.5f;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &count,
                                   &length, nullptr, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects a half-supplied W or H out-parameter pair") {
    float* out = nullptr;
    size_t count = 0;
    size_t length = 0;
    float* w = nullptr;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, nullptr, &out, &count,
                                   &length, &w, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_hpss_with_residual", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("returns three same-length signals") {
    float* h = nullptr;
    float* p = nullptr;
    float* r = nullptr;
    size_t len = 0;
    int out_sr = 0;
    REQUIRE(sonare_hpss_with_residual(samples.data(), samples.size(), sr, 31, 31, &h, &p, &r, &len,
                                      &out_sr) == SONARE_OK);
    REQUIRE(h != nullptr);
    REQUIRE(p != nullptr);
    REQUIRE(r != nullptr);
    REQUIRE(len > 0);
    REQUIRE(out_sr == sr);
    for (size_t i = 0; i < len; ++i) {
      REQUIRE(std::isfinite(h[i]));
      REQUIRE(std::isfinite(p[i]));
      REQUIRE(std::isfinite(r[i]));
    }
    sonare_free_floats(h);
    sonare_free_floats(p);
    sonare_free_floats(r);
  }

  SECTION("rejects null outputs and clears them on bad input") {
    REQUIRE(sonare_hpss_with_residual(samples.data(), samples.size(), sr, 31, 31, nullptr, nullptr,
                                      nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    float* h = non_null_sentinel_float_ptr();
    float* p = non_null_sentinel_float_ptr();
    float* r = non_null_sentinel_float_ptr();
    size_t len = 99;
    int out_sr = 99;
    REQUIRE(sonare_hpss_with_residual(nullptr, 0, sr, 31, 31, &h, &p, &r, &len, &out_sr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(h == nullptr);
    REQUIRE(p == nullptr);
    REQUIRE(r == nullptr);
    REQUIRE(len == 0);
    REQUIRE(out_sr == 0);
  }
}

TEST_CASE("sonare_phase_vocoder", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("stretches audio (rate < 1 produces a longer signal)") {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), sr, 0.5f, 2048, 512, &out,
                                 &out_len) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len > samples.size());
    for (size_t i = 0; i < out_len; ++i) REQUIRE(std::isfinite(out[i]));
    sonare_free_floats(out);
  }

  SECTION("rate <= 0 and null out are rejected") {
    float* out = non_null_sentinel_float_ptr();
    size_t out_len = 99;
    REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), sr, 0.0f, 2048, 512, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_len == 0);

    REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), sr, 0.5f, 2048, 512, nullptr,
                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_spectral_edit", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(1000.0f, sr, 0.5f);
  // add a 5 kHz tone so a band attenuation is measurable.
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] += std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 5000.0f * i / sr);
  }

  SECTION("null config + zero ops is an identity transform") {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, nullptr, 0, &out,
                                 &out_len) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len == samples.size());
    for (size_t i = 0; i < out_len; ++i) REQUIRE(std::isfinite(out[i]));
    sonare_free_floats(out);
  }

  SECTION("attenuating a band runs and returns same-length finite audio") {
    SonareSpectralEditConfig config;
    config.n_fft = 2048;
    config.hop_length = 512;
    config.window = SONARE_WINDOW_HANN;
    config.heal_radius_frames = 2;

    SonareSpectralRegionOp op;
    op.start_sample = 0;
    op.end_sample = static_cast<int64_t>(samples.size());
    op.low_hz = 4000.0f;
    op.high_hz = 6000.0f;
    op.gain_db = -24.0f;
    op.mode = SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;

    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &config, &op, 1, &out,
                                 &out_len) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len == samples.size());
    for (size_t i = 0; i < out_len; ++i) REQUIRE(std::isfinite(out[i]));
    sonare_free_floats(out);
  }

  SECTION("null ops with non-zero count, bad mode, and null out are rejected") {
    float* out = non_null_sentinel_float_ptr();
    size_t out_len = 99;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, nullptr, 3, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_len == 0);

    SonareSpectralRegionOp bad{0, static_cast<int64_t>(samples.size()), 0.0f, 0.0f, 0.0f, 99};
    out = non_null_sentinel_float_ptr();
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, &bad, 1, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);

    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, nullptr, 0, nullptr,
                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}
