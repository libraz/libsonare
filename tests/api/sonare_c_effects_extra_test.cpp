/// @file sonare_c_effects_extra_test.cpp
/// @brief Tests for the extended C API effects wrappers (decompose, nn_filter,
///        remix, hpss_with_residual, phase_vocoder).

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
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

float peak_of(const float* data, size_t n) {
  float peak = 0.0f;
  for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(data[i]));
  return peak;
}

double rms_of(const float* data, size_t n) {
  double sum_sq = 0.0;
  for (size_t i = 0; i < n; ++i) sum_sq += static_cast<double>(data[i]) * data[i];
  return std::sqrt(sum_sq / static_cast<double>(n));
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

  SECTION("rejects a non-finite input element instead of factorising it") {
    float* w = nullptr;
    float* h = nullptr;
    size_t w_len = 0;
    size_t h_len = 0;
    // The same call with the untouched matrix succeeds, so the rejections below
    // cannot come from an unrelated precondition.
    REQUIRE(sonare_decompose_with_init(s.data(), n_features, n_frames, n_components, 20, 2.0f,
                                       "nndsvd", &w, &w_len, &h, &h_len) == SONARE_OK);
    sonare_free_floats(w);
    sonare_free_floats(h);

    for (float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      auto poisoned = s;
      poisoned[poisoned.size() / 2] = bad;
      w = non_null_sentinel_float_ptr();
      h = non_null_sentinel_float_ptr();
      w_len = 99;
      h_len = 99;
      REQUIRE(sonare_decompose_with_init(poisoned.data(), n_features, n_frames, n_components, 20,
                                         2.0f, "nndsvd", &w, &w_len, &h,
                                         &h_len) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(w == nullptr);
      REQUIRE(h == nullptr);
      REQUIRE(w_len == 0);
      REQUIRE(h_len == 0);
      // The plain entry delegates to the _with_init one, so it must reject too.
      REQUIRE(sonare_decompose(poisoned.data(), n_features, n_frames, n_components, 20, 2.0f, &w,
                               &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
    }
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

  SECTION("rejects a non-finite input element instead of smoothing it") {
    float* out = nullptr;
    size_t out_len = 0;
    // Positive control on the untouched matrix: identical arguments, so the
    // rejections below can only come from the non-finite element.
    REQUIRE(sonare_nn_filter(s.data(), n_features, n_frames, "mean", 3, 1, &out, &out_len) ==
            SONARE_OK);
    REQUIRE(out_len == static_cast<size_t>(n_features) * n_frames);
    sonare_free_floats(out);

    for (float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      auto poisoned = s;
      poisoned[0] = bad;
      out = non_null_sentinel_float_ptr();
      out_len = 99;
      // Without the guard this call returns SONARE_OK and an output that is
      // entirely finite: the poisoned element does not propagate, it vanishes
      // in the aggregation. Measured for the NaN case on this input: 0
      // non-finite outputs, but 32 of the 384 values silently shifted by up to
      // 0.13 — the caller has nothing to detect it by.
      REQUIRE(sonare_nn_filter(poisoned.data(), n_features, n_frames, "mean", 3, 1, &out,
                               &out_len) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
      REQUIRE(out_len == 0);
    }
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

  SECTION("null ops with non-zero count, bad mode, bad window, and null out are rejected") {
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

    // Bounds derive from the enum, so a new enumerator widens the accepted range here too.
    const int bad_windows[] = {SONARE_WINDOW_HANN - 1, SONARE_WINDOW_RECTANGULAR + 1, 99};
    for (int bad_window : bad_windows) {
      SonareSpectralEditConfig config{};
      config.window = bad_window;
      out = non_null_sentinel_float_ptr();
      out_len = 99;
      CAPTURE(bad_window);
      REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &config, nullptr, 0, &out,
                                   &out_len) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
      REQUIRE(out_len == 0);
    }
  }

  SECTION("every mapped window is accepted and changes the output") {
    SonareSpectralRegionOp op;
    op.start_sample = 0;
    op.end_sample = static_cast<int64_t>(samples.size());
    op.low_hz = 4000.0f;
    op.high_hz = 6000.0f;
    op.gain_db = -24.0f;
    op.mode = SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;

    const int windows[] = {SONARE_WINDOW_HANN, SONARE_WINDOW_HAMMING, SONARE_WINDOW_BLACKMAN,
                           SONARE_WINDOW_RECTANGULAR};
    constexpr size_t kWindowCount = sizeof(windows) / sizeof(windows[0]);
    REQUIRE(kWindowCount == static_cast<size_t>(SONARE_WINDOW_RECTANGULAR) + 1);

    std::vector<std::vector<float>> rendered(kWindowCount);
    for (size_t i = 0; i < kWindowCount; ++i) {
      SonareSpectralEditConfig config{};
      config.window = windows[i];
      float* out = nullptr;
      size_t out_len = 0;
      CAPTURE(windows[i]);
      REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &config, &op, 1, &out,
                                   &out_len) == SONARE_OK);
      REQUIRE(out != nullptr);
      REQUIRE(out_len == samples.size());
      rendered[i].assign(out, out + out_len);
      sonare_free_floats(out);
    }

    // An implementation that ignored the ordinal and always analysed with Hann would
    // pass every case above; only a pairwise difference says which window was used.
    for (size_t i = 0; i < kWindowCount; ++i) {
      for (size_t j = i + 1; j < kWindowCount; ++j) {
        CAPTURE(windows[i], windows[j]);
        REQUIRE(max_abs_difference(rendered[i].data(), rendered[j].data(), samples.size()) > 1e-4f);
      }
    }
  }

  SECTION("a zero-filled config selects Hann and the documented defaults") {
    SonareSpectralEditConfig zeroed{};
    REQUIRE(SONARE_WINDOW_HANN == 0);
    REQUIRE(zeroed.window == SONARE_WINDOW_HANN);

    SonareSpectralEditConfig spelled_out{};
    spelled_out.n_fft = 2048;
    spelled_out.hop_length = 512;
    spelled_out.window = SONARE_WINDOW_HANN;
    spelled_out.heal_radius_frames = 2;

    SonareSpectralRegionOp op;
    op.start_sample = 0;
    op.end_sample = static_cast<int64_t>(samples.size());
    op.low_hz = 4000.0f;
    op.high_hz = 6000.0f;
    op.gain_db = -24.0f;
    op.mode = SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;

    float* zeroed_out = nullptr;
    size_t zeroed_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &zeroed, &op, 1, &zeroed_out,
                                 &zeroed_len) == SONARE_OK);
    REQUIRE(zeroed_out != nullptr);
    REQUIRE(zeroed_len == samples.size());

    float* spelled_out_audio = nullptr;
    size_t spelled_out_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &spelled_out, &op, 1,
                                 &spelled_out_audio, &spelled_out_len) == SONARE_OK);
    REQUIRE(spelled_out_len == zeroed_len);
    REQUIRE(max_abs_difference(zeroed_out, spelled_out_audio, zeroed_len) == 0.0f);

    sonare_free_floats(zeroed_out);
    sonare_free_floats(spelled_out_audio);
  }
}

TEST_CASE("C scalar guards reject every non-finite argument", "[c_api][validation]") {
  constexpr int sr = 22050;
  const auto samples = generate_sine(440.0f, sr, 0.25f);

  // Driven separately rather than as one representative value: a guard spelled
  // !(x > 0) already refuses a NaN, so only an infinity distinguishes it.
  const float bad_floats[] = {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()};

  SECTION("sonare_phase_vocoder rate") {
    for (float bad_rate : bad_floats) {
      float* out = non_null_sentinel_float_ptr();
      size_t out_len = 99;
      CAPTURE(bad_rate);
      REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), sr, bad_rate, 2048, 512, &out,
                                   &out_len) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
      REQUIRE(out_len == 0);
    }

    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_phase_vocoder(samples.data(), samples.size(), sr, 1.25f, 2048, 512, &out,
                                 &out_len) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_len > 0);
    sonare_free_floats(out);
  }

  SECTION("sonare_zero_crossings threshold") {
    for (float bad_threshold : bad_floats) {
      int* out = nullptr;
      size_t count = 99;
      CAPTURE(bad_threshold);
      REQUIRE(sonare_zero_crossings(samples.data(), samples.size(), bad_threshold, 0, 1, 1, &out,
                                    &count) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
      REQUIRE(count == 0);
    }

    int* out = nullptr;
    size_t count = 0;
    REQUIRE(sonare_zero_crossings(samples.data(), samples.size(), 0.0f, 0, 1, 1, &out, &count) ==
            SONARE_OK);
    REQUIRE(count > 0);
    sonare_free_ints(out);
  }

  SECTION("sonare_pitch_tuning resolution") {
    const std::vector<float> frequencies = {440.0f, 441.5f, 660.0f, 880.0f};
    for (float bad_resolution : bad_floats) {
      float tuning = 99.0f;
      CAPTURE(bad_resolution);
      REQUIRE(sonare_pitch_tuning(frequencies.data(), frequencies.size(), bad_resolution, 12,
                                  &tuning) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(tuning == 0.0f);
    }

    float tuning = 99.0f;
    REQUIRE(sonare_pitch_tuning(frequencies.data(), frequencies.size(), 0.01f, 12, &tuning) ==
            SONARE_OK);
    REQUIRE(std::isfinite(tuning));
  }

  SECTION("sonare_estimate_tuning resolution") {
    for (float bad_resolution : bad_floats) {
      float tuning = 99.0f;
      CAPTURE(bad_resolution);
      REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), sr, 2048, 512, bad_resolution,
                                     12, &tuning) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(tuning == 0.0f);
    }

    float tuning = 99.0f;
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), sr, 2048, 512, 0.01f, 12,
                                   &tuning) == SONARE_OK);
    REQUIRE(std::isfinite(tuning));
  }
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("sonare_eq_create rejects a non-finite sample rate", "[c_api][validation]") {
  const double bad_rates[] = {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()};
  for (double bad_rate : bad_rates) {
    CAPTURE(bad_rate);
    REQUIRE(sonare_eq_create(bad_rate, 512) == nullptr);
    // A handle constructor reports through the thread-local message rather than
    // a SonareError, so the message is the only channel the code can be read on.
    const char* message = sonare_last_error_message();
    REQUIRE(message != nullptr);
    REQUIRE(message[0] != '\0');
  }

  SonareEq* eq = sonare_eq_create(48000.0, 512);
  REQUIRE(eq != nullptr);
  sonare_eq_destroy(eq);
}
#endif

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_streaming_retune_prepare rejects a non-finite sample rate",
          "[c_api][validation]") {
  SonareStreamingRetune* retune = sonare_streaming_retune_create(5.0f, 1.0f, 0);
  REQUIRE(retune != nullptr);

  const double bad_rates[] = {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()};
  for (double bad_rate : bad_rates) {
    CAPTURE(bad_rate);
    REQUIRE(sonare_streaming_retune_prepare(retune, bad_rate, 512) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  REQUIRE(sonare_streaming_retune_prepare(retune, 48000.0, 512) == SONARE_OK);
  sonare_streaming_retune_destroy(retune);
}
#endif

TEST_CASE("sonare_normalize_stereo shares one gain between the channels", "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  std::vector<float> left = generate_sine(440.0f, sample_rate, 0.25f);
  std::vector<float> right = left;
  // 12 dB apart, so a shared gain and a per-channel gain leave the quiet side in
  // two places that no tolerance can confuse.
  for (float& value : left) value *= 0.5f;
  for (float& value : right) value *= 0.125f;

  SonareNormalizeStereoResult result{};
  REQUIRE(sonare_normalize_stereo(left.data(), right.data(), left.size(), sample_rate, -1.0f,
                                  &result) == SONARE_OK);
  REQUIRE(result.length == left.size());

  const float loud_peak = peak_of(result.left, result.length);
  const float quiet_peak = peak_of(result.right, result.length);
  REQUIRE(std::abs(20.0f * std::log10(loud_peak) - (-1.0f)) < 0.05f);
  REQUIRE(std::abs(20.0f * std::log10(loud_peak / quiet_peak) - 12.0f) < 0.05f);
  REQUIRE(std::abs(result.applied_gain_db - (-1.0f - 20.0f * std::log10(0.5f))) < 0.05f);

  // Control: the mono entry on the same quiet channel lands it at the target,
  // 12 dB from where the shared gain leaves it, so the assertions above are
  // about the linkage rather than about normalization having happened.
  float* alone = nullptr;
  size_t alone_length = 0;
  REQUIRE(sonare_normalize(right.data(), right.size(), sample_rate, -1.0f, &alone, &alone_length) ==
          SONARE_OK);
  REQUIRE(std::abs(20.0f * std::log10(peak_of(alone, alone_length)) - (-1.0f)) < 0.05f);
  REQUIRE(quiet_peak < peak_of(alone, alone_length) * 0.5f);
  sonare_free_floats(alone);

  sonare_free_floats(result.left);
  sonare_free_floats(result.right);
}

TEST_CASE("sonare_normalize_rms_stereo measures the pair together", "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  std::vector<float> left = generate_sine(440.0f, sample_rate, 0.25f);
  std::vector<float> right = left;
  for (float& value : left) value *= 0.5f;
  for (float& value : right) value *= 0.125f;

  SonareNormalizeStereoResult result{};
  REQUIRE(sonare_normalize_rms_stereo(left.data(), right.data(), left.size(), sample_rate, -20.0f,
                                      &result) == SONARE_OK);

  const double l = rms_of(result.left, result.length);
  const double r = rms_of(result.right, result.length);
  const double joint_db = 20.0 * std::log10(std::sqrt((l * l + r * r) / 2.0));
  REQUIRE(std::abs(joint_db - (-20.0)) < 0.05);

  // Control: neither channel sits on the target by itself, so the figure driven
  // there is the joint one.
  REQUIRE(std::abs(20.0 * std::log10(l) - (-20.0)) > 1.0);
  REQUIRE(std::abs(20.0 * std::log10(r) - (-20.0)) > 1.0);

  sonare_free_floats(result.left);
  sonare_free_floats(result.right);
}

TEST_CASE("the stereo normalize outputs are cleared before validation", "[c_api][effects]") {
  const std::vector<float> samples = {0.5f, -0.5f, 0.5f, -0.5f};

  // The C entry carries one length and one sample rate for both channels, so
  // the pair's length and rate mismatches are not expressible here; they are
  // covered where they are reachable, on the C++ entry.
  SonareNormalizeStereoResult result;
  result.left = non_null_sentinel_float_ptr();
  result.right = non_null_sentinel_float_ptr();
  result.length = 99;
  result.applied_gain_db = 99.0f;

  REQUIRE(sonare_normalize_stereo(nullptr, samples.data(), samples.size(), 22050, -1.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.left == nullptr);
  REQUIRE(result.right == nullptr);
  REQUIRE(result.length == 0);
  REQUIRE(result.applied_gain_db == 0.0f);

  REQUIRE(sonare_normalize_stereo(samples.data(), nullptr, samples.size(), 22050, -1.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), 0, 22050, -1.0f, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 0, -1.0f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050, -1.0f,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // A positive target with the entry's implicit clipping is the core's refusal,
  // reached through the boundary rather than short-circuited by it.
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050, 1.0f,
                                  &result) != SONARE_OK);

  // Control: the same call with none of those faults succeeds, so the refusals
  // are about the arguments rather than about the entry point.
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050, -1.0f,
                                  &result) == SONARE_OK);
  REQUIRE(result.left != nullptr);
  sonare_free_floats(result.left);
  sonare_free_floats(result.right);
}
