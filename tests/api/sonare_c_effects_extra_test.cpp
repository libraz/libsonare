/// @file sonare_c_effects_extra_test.cpp
/// @brief Tests for the extended C API effects wrappers (decompose, nn_filter,
///        remix, hpss residual output, phase_vocoder).

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#if defined(SONARE_WITH_MIXING)
#include "support/alloc_guard.h"
#endif
#include "sonare_c_test_helpers.h"
#include "util/constants.h"

namespace {

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

float max_abs_difference(const float* lhs, const float* rhs, size_t length) {
  float result = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    result = std::max(result, std::abs(lhs[i] - rhs[i]));
  }
  return result;
}

}  // namespace

namespace {

SonareHpssOptions hpss_options(int n_fft, size_t hop, int soft) {
  SonareHpssOptions options;
  sonare_hpss_options_init(&options);
  options.n_fft = n_fft;
  options.hop_length = hop;
  options.use_soft_mask = soft;
  return options;
}

SonareTimeStretchOptions stretch_options(int n_fft, size_t hop) {
  SonareTimeStretchOptions options;
  sonare_time_stretch_options_init(&options);
  options.n_fft = n_fft;
  options.hop_length = hop;
  return options;
}

SonarePitchShiftOptions shift_options(int n_fft, size_t hop) {
  SonarePitchShiftOptions options;
  sonare_pitch_shift_options_init(&options);
  options.n_fft = n_fft;
  options.hop_length = hop;
  return options;
}

SonareTrimOptions trim_options(size_t frame, size_t hop) {
  SonareTrimOptions options;
  sonare_trim_options_init(&options);
  options.frame_length = frame;
  options.hop_length = hop;
  return options;
}

}  // namespace

TEST_CASE("C effect options initialise to the defaults and NULL options mean the defaults",
          "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  auto samples = generate_sine(440.0f, sample_rate, 0.35f);

  SECTION("init values") {
    SonareHpssOptions hpss;
    sonare_hpss_options_init(&hpss);
    CHECK(hpss.struct_size == sizeof(SonareHpssOptions));
    CHECK(hpss.n_fft == 2048);
    CHECK(hpss.hop_length == 0);
    CHECK(hpss.use_soft_mask == 1);
  }

  SECTION("time stretch and pitch shift") {
    float* null_stretched = nullptr;
    float* init_stretched = nullptr;
    size_t null_stretched_length = 0;
    size_t init_stretched_length = 0;
    const SonareTimeStretchOptions stretch = stretch_options(2048, 512);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), sample_rate, 0.8f, nullptr,
                                &null_stretched, &null_stretched_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), sample_rate, 0.8f, &stretch,
                                &init_stretched, &init_stretched_length) == SONARE_OK);
    REQUIRE(null_stretched_length == init_stretched_length);
    REQUIRE(max_abs_difference(null_stretched, init_stretched, null_stretched_length) < 1e-6f);
    sonare_free_floats(null_stretched);
    sonare_free_floats(init_stretched);

    float* null_shifted = nullptr;
    float* init_shifted = nullptr;
    size_t null_shifted_length = 0;
    size_t init_shifted_length = 0;
    const SonarePitchShiftOptions shift = shift_options(2048, 512);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), sample_rate, 5.0f, nullptr,
                               &null_shifted, &null_shifted_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), sample_rate, 5.0f, &shift,
                               &init_shifted, &init_shifted_length) == SONARE_OK);
    REQUIRE(null_shifted_length == init_shifted_length);
    REQUIRE(max_abs_difference(null_shifted, init_shifted, null_shifted_length) < 1e-6f);
    sonare_free_floats(null_shifted);
    sonare_free_floats(init_shifted);
  }

  SECTION("HPSS and absolute trim") {
    SonareHpssResult null_hpss{};
    SonareHpssResult init_hpss{};
    const SonareHpssOptions hpss = hpss_options(2048, 512, 1);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, nullptr, &null_hpss,
                        nullptr) == SONARE_OK);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &hpss, &init_hpss,
                        nullptr) == SONARE_OK);
    REQUIRE(null_hpss.length == init_hpss.length);
    REQUIRE(null_hpss.sample_rate == init_hpss.sample_rate);
    REQUIRE(max_abs_difference(null_hpss.harmonic, init_hpss.harmonic, null_hpss.length) < 1e-6f);
    REQUIRE(max_abs_difference(null_hpss.percussive, init_hpss.percussive, null_hpss.length) <
            1e-6f);
    sonare_free_hpss_result(&null_hpss);
    sonare_free_hpss_result(&init_hpss);

    std::vector<float> padded(samples.size() + 2048, 0.0f);
    std::copy(samples.begin(), samples.end(), padded.begin() + 1024);
    float* null_trimmed = nullptr;
    float* init_trimmed = nullptr;
    size_t null_trimmed_length = 0;
    size_t init_trimmed_length = 0;
    const SonareTrimOptions trim = trim_options(2048, 512);
    REQUIRE(sonare_trim(padded.data(), padded.size(), sample_rate, -40.0f, nullptr, &null_trimmed,
                        &null_trimmed_length) == SONARE_OK);
    REQUIRE(sonare_trim(padded.data(), padded.size(), sample_rate, -40.0f, &trim, &init_trimmed,
                        &init_trimmed_length) == SONARE_OK);
    REQUIRE(null_trimmed_length == init_trimmed_length);
    REQUIRE(max_abs_difference(null_trimmed, init_trimmed, null_trimmed_length) < 1e-6f);
    sonare_free_floats(null_trimmed);
    sonare_free_floats(init_trimmed);
  }
}

TEST_CASE("C effects options reach their native processing paths", "[c_api][effects]") {
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
    const SonareTimeStretchOptions default_stretch = stretch_options(2048, 512);
    const SonareTimeStretchOptions custom_stretch = stretch_options(1024, 256);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), sample_rate, 0.8f, &default_stretch,
                                &default_stretched, &default_stretched_length) == SONARE_OK);
    REQUIRE(sonare_time_stretch(samples.data(), samples.size(), sample_rate, 0.8f, &custom_stretch,
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
    const SonarePitchShiftOptions default_shift = shift_options(2048, 512);
    const SonarePitchShiftOptions custom_shift = shift_options(1024, 256);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), sample_rate, 5.0f, &default_shift,
                               &default_shifted, &default_shifted_length) == SONARE_OK);
    REQUIRE(sonare_pitch_shift(samples.data(), samples.size(), sample_rate, 5.0f, &custom_shift,
                               &custom_shifted, &custom_shifted_length) == SONARE_OK);
    REQUIRE(default_shifted_length == custom_shifted_length);
    REQUIRE(max_abs_difference(default_shifted, custom_shifted, default_shifted_length) > 1e-5f);
    sonare_free_floats(default_shifted);
    sonare_free_floats(custom_shifted);
  }

  SECTION("HPSS supports hard two-way and three-way routing") {
    SonareHpssResult soft{};
    SonareHpssResult hard{};
    const SonareHpssOptions soft_options = hpss_options(1024, 256, 1);
    const SonareHpssOptions hard_options = hpss_options(1024, 256, 0);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &soft_options, &soft,
                        nullptr) == SONARE_OK);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &hard_options, &hard,
                        nullptr) == SONARE_OK);
    REQUIRE(soft.length == hard.length);
    REQUIRE(max_abs_difference(soft.harmonic, hard.harmonic, hard.length) > 1e-5f);
    sonare_free_hpss_result(&soft);
    sonare_free_hpss_result(&hard);

    SonareHpssResult hard_two_way{};
    SonareHpssResult hard_three_way{};
    float* residual = nullptr;
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &hard_options,
                        &hard_two_way, nullptr) == SONARE_OK);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &hard_options,
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

  SECTION("HPSS reads a zero hop as a quarter of n_fft and refuses one sparser than half overlap") {
    SonareHpssResult absent{};
    SonareHpssResult quarter{};
    const SonareHpssOptions auto_options = hpss_options(1024, 0, 1);
    const SonareHpssOptions quarter_options = hpss_options(1024, 256, 1);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &auto_options, &absent,
                        nullptr) == SONARE_OK);
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &quarter_options,
                        &quarter, nullptr) == SONARE_OK);
    REQUIRE(absent.length == quarter.length);
    CHECK(std::equal(absent.harmonic, absent.harmonic + absent.length, quarter.harmonic));
    CHECK(std::equal(absent.percussive, absent.percussive + absent.length, quarter.percussive));
    sonare_free_hpss_result(&absent);
    sonare_free_hpss_result(&quarter);

    SonareHpssResult refused{};
    const SonareHpssOptions sparse_options = hpss_options(1024, 513, 1);
    CHECK(sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &sparse_options,
                      &refused, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
  }
}

TEST_CASE("sonare_hpss writes no residual when out_residual is NULL", "[c_api][effects]") {
  auto samples = generate_sine(440.0f, 22050, 0.3f);
  SonareHpssResult result{};
  REQUIRE(sonare_hpss(samples.data(), samples.size(), 22050, 31, 31, nullptr, &result, nullptr) ==
          SONARE_OK);
  REQUIRE(result.length == samples.size());
  sonare_free_hpss_result(&result);
}

TEST_CASE("each C effect options struct refuses a wrong struct_size by name", "[c_api][effects]") {
  const float sample = 0.25f;
  float* out = nullptr;
  size_t out_length = 0;
  SonareHpssResult result{};

  SonareHpssOptions hpss;
  sonare_hpss_options_init(&hpss);
  hpss.struct_size = 0;
  REQUIRE(sonare_hpss(&sample, 1, 22050, 31, 31, &hpss, &result, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("options") != std::string::npos);

  SonareTimeStretchOptions stretch;
  sonare_time_stretch_options_init(&stretch);
  stretch.struct_size = 0;
  REQUIRE(sonare_time_stretch(&sample, 1, 22050, 0.8f, &stretch, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("options") != std::string::npos);

  SonarePitchShiftOptions shift;
  sonare_pitch_shift_options_init(&shift);
  shift.struct_size = 0;
  REQUIRE(sonare_pitch_shift(&sample, 1, 22050, 2.0f, &shift, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("options") != std::string::npos);

  SonareTrimOptions trim;
  sonare_trim_options_init(&trim);
  trim.struct_size = 0;
  REQUIRE(sonare_trim(&sample, 1, 22050, -40.0f, &trim, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("options") != std::string::npos);
}

TEST_CASE("sonare_normalize_rms reaches its target unclipped and sonare_trim uses custom framing",
          "[c_api][effects]") {
  SECTION("RMS normalization lands on target and leaves the crest above full scale") {
    const std::vector<float> samples = {0.8f, -0.8f, 0.2f, -0.2f};
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_normalize_rms(samples.data(), samples.size(), 22050, 0.0f, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(out_length == samples.size());
    double energy = 0.0;
    for (size_t i = 0; i < out_length; ++i) energy += static_cast<double>(out[i]) * out[i];
    CHECK(std::abs(std::sqrt(energy / static_cast<double>(out_length)) - 1.0) < 1e-5);
    CHECK(out[0] > 1.0f);
    CHECK(out[1] == -out[0]);
    sonare_free_floats(out);
  }

  SECTION("trim accepts frame and hop overrides") {
    constexpr int sample_rate = 8000;
    std::vector<float> samples(1024, 0.0f);
    for (size_t i = 128; i < 896; ++i) {
      samples[i] = 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * i /
                                   sample_rate);
    }
    float* by_default = nullptr;
    size_t default_length = 0;
    REQUIRE(sonare_trim(samples.data(), samples.size(), sample_rate, -40.0f, nullptr, &by_default,
                        &default_length) == SONARE_OK);

    float* custom = nullptr;
    size_t custom_length = 0;
    const SonareTrimOptions custom_options = trim_options(64, 16);
    REQUIRE(sonare_trim(samples.data(), samples.size(), sample_rate, -40.0f, &custom_options,
                        &custom, &custom_length) == SONARE_OK);
    REQUIRE(custom != nullptr);
    REQUIRE(custom_length < samples.size());
    REQUIRE((custom_length != default_length ||
             max_abs_difference(custom, by_default, custom_length) > 1e-5f));
    sonare_free_floats(by_default);
    sonare_free_floats(custom);
  }
}

TEST_CASE("effects accept input shorter than one analysis hop", "[c_api][effects]") {
  const float sample = 0.25f;
  float* out = nullptr;
  size_t out_length = 0;
  const SonarePitchShiftOptions shift = shift_options(2048, 512);
  REQUIRE(sonare_pitch_shift(&sample, 1, 22050, 0.0f, &shift, &out, &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == 1);
  REQUIRE(std::isfinite(out[0]));
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  const SonareTimeStretchOptions stretch = stretch_options(2048, 512);
  REQUIRE(sonare_time_stretch(&sample, 1, 22050, 0.5f, &stretch, &out, &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == 2);
  sonare_free_floats(out);
}

TEST_CASE("effect outputs are cleared before validation", "[c_api][effects]") {
  const float sample = 0.25f;
  const SonareTimeStretchOptions stretch = stretch_options(2048, 512);
  const SonarePitchShiftOptions shift = shift_options(2048, 512);
  const SonareTrimOptions bad_trim = trim_options(64, 0);
  const SonareHpssOptions hpss = hpss_options(2048, 512, 1);

  float* out = non_null_sentinel_float_ptr();
  size_t out_length = 99;
  REQUIRE(sonare_time_stretch(nullptr, 0, 22050, 0.8f, &stretch, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  out = non_null_sentinel_float_ptr();
  out_length = 99;
  REQUIRE(sonare_pitch_shift(&sample, 1, 22050, std::numeric_limits<float>::quiet_NaN(), &shift,
                             &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
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
  REQUIRE(sonare_trim(&sample, 1, 22050, -40.0f, &bad_trim, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  SonareHpssResult result{non_null_sentinel_float_ptr(), non_null_sentinel_float_ptr(), 99, 99};
  float* residual = non_null_sentinel_float_ptr();
  REQUIRE(sonare_hpss(nullptr, 0, 22050, 31, 31, &hpss, &result, &residual) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.harmonic == nullptr);
  REQUIRE(result.percussive == nullptr);
  REQUIRE(result.length == 0);
  REQUIRE(result.sample_rate == 0);
  REQUIRE(residual == nullptr);
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
    REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, nullptr, &w,
                             &w_len, &h, &h_len) == SONARE_OK);
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
                             nullptr, nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    float* w = non_null_sentinel_float_ptr();
    float* h = non_null_sentinel_float_ptr();
    size_t w_len = 99;
    size_t h_len = 99;
    REQUIRE(sonare_decompose(nullptr, n_features, n_frames, n_components, 20, 2.0f, nullptr, &w,
                             &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(w == nullptr);
    REQUIRE(h == nullptr);
    REQUIRE(w_len == 0);
    REQUIRE(h_len == 0);

    w = non_null_sentinel_float_ptr();
    h = non_null_sentinel_float_ptr();
    REQUIRE(sonare_decompose(s.data(), 0, n_frames, n_components, 20, 2.0f, nullptr, &w, &w_len, &h,
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
    SonareDecomposeOptions nndsvd;
    sonare_decompose_options_init(&nndsvd);
    nndsvd.init = "nndsvd";
    REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &nndsvd, &w,
                             &w_len, &h, &h_len) == SONARE_OK);
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
      REQUIRE(sonare_decompose(poisoned.data(), n_features, n_frames, n_components, 20, 2.0f,
                               &nndsvd, &w, &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(w == nullptr);
      REQUIRE(h == nullptr);
      REQUIRE(w_len == 0);
      REQUIRE(h_len == 0);
      // The default initialisation rejects it as well.
      REQUIRE(sonare_decompose(poisoned.data(), n_features, n_frames, n_components, 20, 2.0f,
                               nullptr, &w, &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
    }
  }
}

TEST_CASE("sonare_decompose names an unknown init and a wrong struct_size", "[c_api][effects]") {
  const int n_features = 16;
  const int n_frames = 24;
  const int n_components = 4;
  const auto s = generate_spectrogram(n_features, n_frames);
  float* w = nullptr;
  float* h = nullptr;
  size_t w_len = 0;
  size_t h_len = 0;

  SonareDecomposeOptions options;
  sonare_decompose_options_init(&options);
  options.init = "spectral";
  REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &options, &w,
                           &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.init must be \"random\" or \"nndsvd\"");

  sonare_decompose_options_init(&options);
  options.struct_size = 0;
  REQUIRE(sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &options, &w,
                           &w_len, &h, &h_len) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("options") != std::string::npos);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_decompose reads n_iter 0 as the library default", "[c_api][effects]") {
  const int n_features = 16;
  const int n_frames = 24;
  const int n_components = 4;
  auto s = generate_spectrogram(n_features, n_frames);
  auto run = [&](int n_iter, std::vector<float>* w_out) {
    float* w = nullptr;
    float* h = nullptr;
    size_t w_len = 0;
    size_t h_len = 0;
    const SonareError err = sonare_decompose(s.data(), n_features, n_frames, n_components, n_iter,
                                             2.0f, nullptr, &w, &w_len, &h, &h_len);
    if (err == SONARE_OK) w_out->assign(w, w + w_len);
    sonare_free_floats(w);
    sonare_free_floats(h);
    return err;
  };
  std::vector<float> defaulted;
  std::vector<float> explicit_iter;
  std::vector<float> other_iter;
  REQUIRE(run(0, &defaulted) == SONARE_OK);
  REQUIRE(run(50, &explicit_iter) == SONARE_OK);
  REQUIRE(run(20, &other_iter) == SONARE_OK);
  CHECK(defaulted == explicit_iter);
  CHECK(defaulted != other_iter);

  std::vector<float> unused;
  CHECK(run(-1, &unused) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_decompose stages both factors before publication on allocation failure",
          "[c_api][effects][allocation]") {
  constexpr int n_features = 16;
  constexpr int n_frames = 24;
  constexpr int n_components = 4;
  const auto s = generate_spectrogram(n_features, n_frames);
  SonareDecomposeOptions nndsvd;
  sonare_decompose_options_init(&nndsvd);
  nndsvd.init = "nndsvd";

  const auto measure_success = [&]() {
    float* w = nullptr;
    float* h = nullptr;
    size_t w_length = 0;
    size_t h_length = 0;
    SonareError error = SONARE_ERROR_UNKNOWN;
    size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      error = sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &nndsvd, &w,
                               &w_length, &h, &h_length);
      allocations = guard.count();
    }
    REQUIRE(error == SONARE_OK);
    REQUIRE(w != nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(w_length > 0);
    REQUIRE(h_length > 0);
    sonare_free_floats(w);
    sonare_free_floats(h);
    return allocations;
  };

  // A warm run removes one-time setup from the measurement. Two subsequent
  // deterministic NNDSVD calls must have the same allocation sequence, so the
  // final allocation is specifically the H copy targeted below.
  (void)measure_success();
  const size_t allocations = measure_success();
  const size_t repeated_allocations = measure_success();
  REQUIRE(allocations > 0);
  REQUIRE(allocations == repeated_allocations);

  float* w = non_null_sentinel_float_ptr();
  float* h = non_null_sentinel_float_ptr();
  size_t w_length = 99;
  size_t h_length = 99;
  SonareError error = SONARE_OK;
  {
    sonare::test::AllocationFailureAtGuard fail_last_h(allocations);
    error = sonare_decompose(s.data(), n_features, n_frames, n_components, 20, 2.0f, &nndsvd, &w,
                             &w_length, &h, &h_length);
  }
  REQUIRE(error == SONARE_ERROR_OUT_OF_MEMORY);
  REQUIRE(w == nullptr);
  REQUIRE(h == nullptr);
  REQUIRE(w_length == 0);
  REQUIRE(h_length == 0);
  // The free entry points are intentionally null-safe, including after a
  // partially completed multi-output call.
  sonare_free_floats(w);
  sonare_free_floats(h);
}
#endif

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

TEST_CASE("sonare_nn_filter reads -1 as the library default and refuses other non-quantities",
          "[c_api][effects]") {
  const int n_features = 16;
  const int n_frames = 24;
  auto s = generate_spectrogram(n_features, n_frames);
  auto run = [&](int k, int width, std::vector<float>* result) {
    float* out = nullptr;
    size_t out_len = 0;
    const SonareError err =
        sonare_nn_filter(s.data(), n_features, n_frames, "mean", k, width, &out, &out_len);
    if (err == SONARE_OK) result->assign(out, out + out_len);
    sonare_free_floats(out);
    return err;
  };
  std::vector<float> defaulted;
  std::vector<float> explicit_values;
  std::vector<float> other_k;
  REQUIRE(run(-1, -1, &defaulted) == SONARE_OK);
  REQUIRE(run(7, 1, &explicit_values) == SONARE_OK);
  REQUIRE(run(3, 1, &other_k) == SONARE_OK);
  CHECK(defaulted == explicit_values);
  CHECK(defaulted != other_k);

  std::vector<float> unused;
  CHECK(run(0, 1, &unused) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(run(-2, 1, &unused) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(run(7, -2, &unused) == SONARE_ERROR_INVALID_PARAMETER);
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
    SonareDecomposeStemsConfig config;
    sonare_decompose_stems_config_init(&config);
    config.n_components = 2;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.n_iter = 30;
    float* out = nullptr;
    int count = 0;
    size_t length = 0;
    float* w = nullptr;
    size_t w_length = 0;
    float* h = nullptr;
    size_t h_length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &length,
                                   &count, &w, &w_length, &h, &h_length) == SONARE_OK);
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
    int count = 0;
    size_t length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, nullptr, &out, &length,
                                   &count, nullptr, nullptr, nullptr, nullptr) == SONARE_OK);
    REQUIRE(count == 4);
    REQUIRE(length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("W and H can be requested independently") {
    SonareDecomposeStemsConfig config;
    sonare_decompose_stems_config_init(&config);
    config.n_components = 2;
    config.n_fft = 256;
    config.hop_length = 64;
    config.n_iter = 4;

    float* w_only_out = nullptr;
    int w_only_count = 0;
    size_t w_only_length = 0;
    float* w_only = nullptr;
    size_t w_only_factor_length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &w_only_out,
                                   &w_only_length, &w_only_count, &w_only, &w_only_factor_length,
                                   nullptr, nullptr) == SONARE_OK);
    REQUIRE(w_only_out != nullptr);
    REQUIRE(w_only_count == config.n_components);
    REQUIRE(w_only_length == samples.size());
    REQUIRE(w_only != nullptr);
    REQUIRE(w_only_factor_length ==
            static_cast<size_t>(config.n_fft / 2 + 1) * config.n_components);
    sonare_free_floats(w_only_out);
    sonare_free_floats(w_only);

    float* h_only_out = nullptr;
    int h_only_count = 0;
    size_t h_only_length = 0;
    float* h_only = nullptr;
    size_t h_only_factor_length = 0;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &h_only_out,
                                   &h_only_length, &h_only_count, nullptr, nullptr, &h_only,
                                   &h_only_factor_length) == SONARE_OK);
    REQUIRE(h_only_out != nullptr);
    REQUIRE(h_only_count == config.n_components);
    REQUIRE(h_only_length == samples.size());
    REQUIRE(h_only != nullptr);
    REQUIRE(h_only_factor_length > 0);
    REQUIRE(h_only_factor_length % static_cast<size_t>(config.n_components) == 0);
    sonare_free_floats(h_only_out);
    sonare_free_floats(h_only);
  }

  SECTION("rejects an uninitialised config, a zero in a nonzero field and a low mask power") {
    SonareDecomposeStemsConfig config{};
    float* out = non_null_sentinel_float_ptr();
    int count = 99;
    size_t length = 99;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &length,
                                   &count, nullptr, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
    REQUIRE(length == 0);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.struct_size must be set by sonare_decompose_stems_config_init");

    for (const auto zero : std::vector<void (*)(SonareDecomposeStemsConfig&)>{
             [](SonareDecomposeStemsConfig& c) { c.n_components = 0; },
             [](SonareDecomposeStemsConfig& c) { c.n_fft = 0; },
             [](SonareDecomposeStemsConfig& c) {
               c.hop_length = static_cast<size_t>(std::numeric_limits<int>::max()) + 1;
             },
             [](SonareDecomposeStemsConfig& c) { c.n_iter = 0; }}) {
      sonare_decompose_stems_config_init(&config);
      zero(config);
      REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &length,
                                     &count, nullptr, nullptr, nullptr,
                                     nullptr) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(std::string(sonare_last_error_message()).find("config.") == 0);
    }

    sonare_decompose_stems_config_init(&config);
    config.mask_power = 0.5f;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, &config, &out, &length,
                                   &count, nullptr, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects a half-supplied W or H out-parameter pair") {
    float* out = nullptr;
    int count = 0;
    size_t length = 0;
    float* w = nullptr;
    REQUIRE(sonare_decompose_stems(samples.data(), samples.size(), sr, nullptr, &out, &length,
                                   &count, &w, nullptr, nullptr,
                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_decompose_stems_linked", "[c_api][effects]") {
  const int sr = 22050;
  auto left = generate_sine(440.0f, sr, 0.4f);
  auto right = generate_sine(660.0f, sr, 0.4f);
  const float* channels[2] = {left.data(), right.data()};

  SECTION("emits [component][channel][sample] plus the factorisation") {
    SonareDecomposeStemsConfig config;
    sonare_decompose_stems_config_init(&config);
    config.n_components = 2;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.n_iter = 30;
    float* out = nullptr;
    int component_count = 0;
    const size_t channel_count = 2;
    size_t component_length = 0;
    float* w = nullptr;
    size_t w_length = 0;
    float* h = nullptr;
    size_t h_length = 0;
    REQUIRE(sonare_decompose_stems_linked(channels, 2, left.size(), sr, &config, &out,
                                          &component_length, &component_count, &w, &w_length, &h,
                                          &h_length) == SONARE_OK);
    REQUIRE(component_count == 2);
    REQUIRE(component_length == left.size());
    REQUIRE(w_length == static_cast<size_t>(config.n_fft / 2 + 1) * component_count);
    REQUIRE(h_length % component_count == 0);

    // The masks partition each channel's own complex spectrogram, so summing
    // the components reconstructs each channel over the interior, where the
    // analysis window overlap is complete.
    const size_t per_component = channel_count * component_length;
    for (size_t c = 0; c < channel_count; ++c) {
      const float* source = c == 0 ? left.data() : right.data();
      double err = 0.0;
      double ref = 0.0;
      for (size_t i = static_cast<size_t>(config.n_fft);
           i + static_cast<size_t>(config.n_fft) < component_length; ++i) {
        double sum = 0.0;
        for (size_t comp = 0; comp < static_cast<size_t>(component_count); ++comp) {
          sum += out[comp * per_component + c * component_length + i];
        }
        err += (sum - source[i]) * (sum - source[i]);
        ref += static_cast<double>(source[i]) * source[i];
      }
      REQUIRE(ref > 0.0);
      REQUIRE(std::sqrt(err / ref) < 0.05);
    }
    sonare_free_floats(out);
    sonare_free_floats(w);
    sonare_free_floats(h);
  }

  SECTION("one channel matches sonare_decompose_stems bit for bit") {
    SonareDecomposeStemsConfig config;
    sonare_decompose_stems_config_init(&config);
    config.n_components = 2;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.n_iter = 20;

    float* mono_out = nullptr;
    int mono_count = 0;
    size_t mono_length = 0;
    float* mono_w = nullptr;
    size_t mono_w_length = 0;
    float* mono_h = nullptr;
    size_t mono_h_length = 0;
    REQUIRE(sonare_decompose_stems(left.data(), left.size(), sr, &config, &mono_out, &mono_length,
                                   &mono_count, &mono_w, &mono_w_length, &mono_h,
                                   &mono_h_length) == SONARE_OK);

    const float* one_channel[1] = {left.data()};
    float* linked_out = nullptr;
    int linked_component_count = 0;
    size_t linked_component_length = 0;
    float* linked_w = nullptr;
    size_t linked_w_length = 0;
    float* linked_h = nullptr;
    size_t linked_h_length = 0;
    REQUIRE(sonare_decompose_stems_linked(one_channel, 1, left.size(), sr, &config, &linked_out,
                                          &linked_component_length, &linked_component_count,
                                          &linked_w, &linked_w_length, &linked_h,
                                          &linked_h_length) == SONARE_OK);

    REQUIRE(linked_component_count == mono_count);
    REQUIRE(linked_component_length == mono_length);
    REQUIRE(linked_w_length == mono_w_length);
    REQUIRE(linked_h_length == mono_h_length);
    REQUIRE(max_abs_difference(linked_out, mono_out, linked_component_count * mono_length) == 0.0f);
    REQUIRE(max_abs_difference(linked_w, mono_w, mono_w_length) == 0.0f);
    REQUIRE(max_abs_difference(linked_h, mono_h, mono_h_length) == 0.0f);

    sonare_free_floats(mono_out);
    sonare_free_floats(mono_w);
    sonare_free_floats(mono_h);
    sonare_free_floats(linked_out);
    sonare_free_floats(linked_w);
    sonare_free_floats(linked_h);
  }

  SECTION("NULL config selects the defaults and W/H are optional") {
    float* out = nullptr;
    int component_count = 0;
    size_t component_length = 0;
    REQUIRE(sonare_decompose_stems_linked(channels, 2, left.size(), sr, nullptr, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_OK);
    REQUIRE(component_count == 4);
    REQUIRE(component_length == left.size());
    sonare_free_floats(out);
  }

  SECTION("rejects an uninitialised config, a zero in a nonzero field and a low mask power") {
    SonareDecomposeStemsConfig config{};
    float* out = non_null_sentinel_float_ptr();
    int component_count = 99;
    size_t component_length = 99;
    REQUIRE(sonare_decompose_stems_linked(channels, 2, left.size(), sr, &config, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(component_count == 0);
    REQUIRE(component_length == 0);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.struct_size must be set by sonare_decompose_stems_config_init");

    sonare_decompose_stems_config_init(&config);
    config.mask_power = 0.5f;
    REQUIRE(sonare_decompose_stems_linked(channels, 2, left.size(), sr, &config, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects a half-supplied W or H out-parameter pair") {
    float* out = nullptr;
    int component_count = 0;
    size_t component_length = 0;
    float* w = nullptr;
    REQUIRE(sonare_decompose_stems_linked(channels, 2, left.size(), sr, nullptr, &out,
                                          &component_length, &component_count, &w, nullptr, nullptr,
                                          nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION(
      "rejects a null channel set, zero channels, a null channel buffer and an "
      "over-limit channel count") {
    float* out = non_null_sentinel_float_ptr();
    int component_count = 99;
    size_t component_length = 99;
    REQUIRE(sonare_decompose_stems_linked(nullptr, 2, left.size(), sr, nullptr, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(component_count == 0);
    REQUIRE(component_length == 0);

    REQUIRE(sonare_decompose_stems_linked(channels, 0, left.size(), sr, nullptr, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    const float* with_null[2] = {left.data(), nullptr};
    REQUIRE(sonare_decompose_stems_linked(with_null, 2, left.size(), sr, nullptr, &out,
                                          &component_length, &component_count, nullptr, nullptr,
                                          nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    std::vector<const float*> too_many(65, left.data());
    REQUIRE(sonare_decompose_stems_linked(too_many.data(), too_many.size(), left.size(), sr,
                                          nullptr, &out, &component_length, &component_count,
                                          nullptr, nullptr, nullptr,
                                          nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("linked stems rejects an overflowing channel count before allocation",
          "[c_api][effects][regression]") {
  const float sample = 0.0f;
  const float* channels[] = {&sample};
  float* out = non_null_sentinel_float_ptr();
  float* w = non_null_sentinel_float_ptr();
  float* h = non_null_sentinel_float_ptr();
  int component_count = 99;
  size_t component_length = 99;
  size_t w_length = 99;
  size_t h_length = 99;
  SonareError error = SONARE_OK;

  CHECK_NOTHROW(error = sonare_decompose_stems_linked(
                    channels, std::numeric_limits<size_t>::max(), 1, 48000, nullptr, &out,
                    &component_length, &component_count, &w, &w_length, &h, &h_length));
  CHECK(error == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  CHECK(w == nullptr);
  CHECK(h == nullptr);
  CHECK(component_count == 0);
  CHECK(component_length == 0);
  CHECK(w_length == 0);
  CHECK(h_length == 0);
}

TEST_CASE("sonare_hpss with a residual output", "[c_api][effects]") {
  const int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("returns three same-length signals") {
    SonareHpssResult result{};
    float* r = nullptr;
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sr, 31, 31, nullptr, &result, &r) ==
            SONARE_OK);
    REQUIRE(result.harmonic != nullptr);
    REQUIRE(result.percussive != nullptr);
    REQUIRE(r != nullptr);
    REQUIRE(result.length > 0);
    REQUIRE(result.sample_rate == sr);
    for (size_t i = 0; i < result.length; ++i) {
      REQUIRE(std::isfinite(result.harmonic[i]));
      REQUIRE(std::isfinite(result.percussive[i]));
      REQUIRE(std::isfinite(r[i]));
    }
    sonare_free_hpss_result(&result);
    sonare_free_floats(r);
  }

  SECTION("rejects a NULL result and clears outputs on bad input") {
    float* r = nullptr;
    REQUIRE(sonare_hpss(samples.data(), samples.size(), sr, 31, 31, nullptr, nullptr, &r) ==
            SONARE_ERROR_INVALID_PARAMETER);
    SonareHpssResult result{non_null_sentinel_float_ptr(), non_null_sentinel_float_ptr(), 99, 99};
    r = non_null_sentinel_float_ptr();
    REQUIRE(sonare_hpss(nullptr, 0, sr, 31, 31, nullptr, &result, &r) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.harmonic == nullptr);
    REQUIRE(result.percussive == nullptr);
    REQUIRE(r == nullptr);
    REQUIRE(result.length == 0);
    REQUIRE(result.sample_rate == 0);
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
    sonare_spectral_edit_config_init(&config);
    config.n_fft = 2048;
    config.hop_length = 512;

    SonareSpectralRegionOp op;
    sonare_spectral_region_op_init(&op);
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

    SonareSpectralRegionOp bad;
    sonare_spectral_region_op_init(&bad);
    bad.end_sample = static_cast<int64_t>(samples.size());
    bad.mode = 99;
    out = non_null_sentinel_float_ptr();
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, &bad, 1, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);

    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, nullptr, 0, nullptr,
                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    // Bounds derive from the enum, so a new enumerator widens the accepted range here too.
    const int bad_windows[] = {SONARE_WINDOW_HANN - 1, SONARE_WINDOW_RECTANGULAR + 1, 99};
    for (int bad_window : bad_windows) {
      SonareSpectralEditConfig config;
      sonare_spectral_edit_config_init(&config);
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
    sonare_spectral_region_op_init(&op);
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
      SonareSpectralEditConfig config;
      sonare_spectral_edit_config_init(&config);
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

  SECTION("init selects Hann and the documented defaults") {
    SonareSpectralEditConfig initialised;
    sonare_spectral_edit_config_init(&initialised);
    CHECK(initialised.struct_size == sizeof(SonareSpectralEditConfig));
    CHECK(initialised.n_fft == 2048);
    CHECK(initialised.hop_length == 0);
    CHECK(initialised.window == SONARE_WINDOW_HANN);
    CHECK(initialised.heal_radius_frames == 2);

    SonareSpectralEditConfig spelled_out = initialised;
    spelled_out.hop_length = 512;

    SonareSpectralRegionOp op;
    sonare_spectral_region_op_init(&op);
    CHECK(op.struct_size == sizeof(SonareSpectralRegionOp));
    CHECK(std::isnan(op.high_hz));
    op.start_sample = 0;
    op.end_sample = static_cast<int64_t>(samples.size());
    op.low_hz = 4000.0f;
    op.high_hz = 6000.0f;
    op.gain_db = -24.0f;
    op.mode = SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;

    float* init_out = nullptr;
    size_t init_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &initialised, &op, 1,
                                 &init_out, &init_len) == SONARE_OK);
    REQUIRE(init_out != nullptr);
    REQUIRE(init_len == samples.size());

    float* spelled_out_audio = nullptr;
    size_t spelled_out_len = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &spelled_out, &op, 1,
                                 &spelled_out_audio, &spelled_out_len) == SONARE_OK);
    REQUIRE(spelled_out_len == init_len);
    REQUIRE(max_abs_difference(init_out, spelled_out_audio, init_len) == 0.0f);

    sonare_free_floats(init_out);
    sonare_free_floats(spelled_out_audio);
  }

  SECTION("an uninitialised config or op, a zero field and a non-positive high edge are refused") {
    float* out = nullptr;
    size_t out_len = 0;
    SonareSpectralEditConfig zeroed{};
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &zeroed, nullptr, 0, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.struct_size must be set by sonare_spectral_edit_config_init");

    SonareSpectralEditConfig config;
    sonare_spectral_edit_config_init(&config);
    config.n_fft = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &config, nullptr, 0, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find("config.n_fft") == 0);
    sonare_spectral_edit_config_init(&config);
    config.heal_radius_frames = 0;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, &config, nullptr, 0, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find("config.heal_radius_frames") == 0);

    SonareSpectralRegionOp op{};
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, &op, 1, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "ops[0].struct_size must be set by sonare_spectral_region_op_init");
    sonare_spectral_region_op_init(&op);
    op.high_hz = 0.0f;
    REQUIRE(sonare_spectral_edit(samples.data(), samples.size(), sr, nullptr, &op, 1, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find("ops[0].high_hz") == 0);
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
TEST_CASE("sonare_eq_create rejects an out-of-range sample rate", "[c_api][validation]") {
  const int bad_rates[] = {std::numeric_limits<int>::min(), -1, 0, 7999, 384001, 1000000000};
  for (int bad_rate : bad_rates) {
    CAPTURE(bad_rate);
    SonareEq* refused_eq = nullptr;
    REQUIRE(sonare_eq_create(bad_rate, 512, &refused_eq) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_eq == nullptr);
    // The refusal also leaves a diagnostic message.
    const char* message = sonare_last_error_message();
    REQUIRE(message != nullptr);
    REQUIRE(message[0] != '\0');
  }

  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000, 512, &eq) == SONARE_OK);
  REQUIRE(eq != nullptr);
  sonare_eq_destroy(eq);
}
#endif

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_streaming_retune_prepare rejects an out-of-range sample rate",
          "[c_api][validation]") {
  SonareStreamingRetune* retune = nullptr;
  REQUIRE(sonare_streaming_retune_create(5.0f, 1.0f, 0, &retune) == SONARE_OK);
  REQUIRE(retune != nullptr);

  const int bad_rates[] = {-1, 0, 7999, 384001, 1000000000};
  for (int bad_rate : bad_rates) {
    CAPTURE(bad_rate);
    REQUIRE(sonare_streaming_retune_prepare(retune, bad_rate, 512) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  REQUIRE(sonare_streaming_retune_prepare(retune, 48000, 512) == SONARE_OK);
  sonare_streaming_retune_destroy(retune);
}
#endif

#ifndef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare streaming retune exports deterministic feature-off stubs",
          "[c_api][voice_changer][stub]") {
  // The shared library still exports this API when the optional DSP is absent.
  // Every call must report the capability boundary rather than accidentally
  // validating arguments or reaching an unlinked StreamingRetune symbol.
  SonareStreamingRetune* retune = nullptr;
  REQUIRE(sonare_streaming_retune_create(5.0f, 1.0f, 0, &retune) == SONARE_OK);
  REQUIRE(retune == nullptr);
  const char* detail = sonare_last_error_message();
  REQUIRE(detail != nullptr);
  REQUIRE(detail[0] != '\0');

  sonare_streaming_retune_destroy(nullptr);
  REQUIRE(sonare_streaming_retune_prepare(nullptr, 48000, 128) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_streaming_retune_reset(nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_streaming_retune_set_config(nullptr, 0.0f, 1.0f, 0) == SONARE_ERROR_NOT_SUPPORTED);

  float semitones = 11.0f;
  float mix = 0.75f;
  int grain = 123;
  REQUIRE(sonare_streaming_retune_config(nullptr, &semitones, &mix, &grain) ==
          SONARE_ERROR_NOT_SUPPORTED);
  CHECK(semitones == 0.0f);
  CHECK(mix == 0.0f);
  CHECK(grain == 0);

  float sample = 0.25f;
  REQUIRE(sonare_streaming_retune_process_mono(nullptr, &sample, 1) == SONARE_ERROR_NOT_SUPPORTED);

  grain = 123;
  REQUIRE(sonare_streaming_retune_grain_size_samples(nullptr, &grain) ==
          SONARE_ERROR_NOT_SUPPORTED);
  CHECK(grain == 0);
  int latency = 456;
  REQUIRE(sonare_streaming_retune_latency_samples(nullptr, &latency) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(latency == 0);
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

  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_normalize_stereo(left.data(), right.data(), left.size(), sample_rate, -1.0f,
                                  &out_left, &out_right, &out_length) == SONARE_OK);
  REQUIRE(out_length == left.size());

  const float loud_peak = max_abs(out_left, out_length);
  const float quiet_peak = max_abs(out_right, out_length);
  REQUIRE(std::abs(20.0f * std::log10(loud_peak) - (-1.0f)) < 0.05f);
  REQUIRE(std::abs(20.0f * std::log10(loud_peak / quiet_peak) - 12.0f) < 0.05f);

  // Control: the mono entry on the same quiet channel lands it at the target,
  // 12 dB from where the shared gain leaves it, so the assertions above are
  // about the linkage rather than about normalization having happened.
  float* alone = nullptr;
  size_t alone_length = 0;
  REQUIRE(sonare_normalize(right.data(), right.size(), sample_rate, -1.0f, &alone, &alone_length) ==
          SONARE_OK);
  REQUIRE(std::abs(20.0f * std::log10(max_abs(alone, alone_length)) - (-1.0f)) < 0.05f);
  REQUIRE(quiet_peak < max_abs(alone, alone_length) * 0.5f);
  sonare_free_floats(alone);

  sonare_free_floats(out_left);
  sonare_free_floats(out_right);
}

TEST_CASE("sonare_normalize_rms_stereo measures the pair together", "[c_api][effects]") {
  constexpr int sample_rate = 22050;
  std::vector<float> left = generate_sine(440.0f, sample_rate, 0.25f);
  std::vector<float> right = left;
  for (float& value : left) value *= 0.5f;
  for (float& value : right) value *= 0.125f;

  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_normalize_rms_stereo(left.data(), right.data(), left.size(), sample_rate, -20.0f,
                                      &out_left, &out_right, &out_length) == SONARE_OK);

  const double l = rms_of(out_left, out_length);
  const double r = rms_of(out_right, out_length);
  const double joint_db = 20.0 * std::log10(std::sqrt((l * l + r * r) / 2.0));
  REQUIRE(std::abs(joint_db - (-20.0)) < 0.05);

  // Control: neither channel sits on the target by itself, so the figure driven
  // there is the joint one.
  REQUIRE(std::abs(20.0 * std::log10(l) - (-20.0)) > 1.0);
  REQUIRE(std::abs(20.0 * std::log10(r) - (-20.0)) > 1.0);

  sonare_free_floats(out_left);
  sonare_free_floats(out_right);
}

TEST_CASE("the C normalize entries reach a target above full scale unclipped", "[c_api][effects]") {
  // A +/-0.5 alternation: its peak and RMS are both 0.5, so every entry lands on +3 dB.
  const std::vector<float> samples = {0.5f, -0.5f, 0.5f, -0.5f};
  const double expected = std::pow(10.0, 3.0 / 20.0);
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_normalize(samples.data(), samples.size(), 22050, 3.0f, &out, &out_length) ==
          SONARE_OK);
  CHECK(std::abs(out[0] - expected) < 1e-5);
  sonare_free_floats(out);

  using StereoEntry =
      SonareError (*)(const float*, const float*, size_t, int, float, float**, float**, size_t*);
  for (StereoEntry entry : {&sonare_normalize_stereo, &sonare_normalize_rms_stereo}) {
    float* out_left = nullptr;
    float* out_right = nullptr;
    REQUIRE(entry(samples.data(), samples.data(), samples.size(), 22050, 3.0f, &out_left,
                  &out_right, &out_length) == SONARE_OK);
    CHECK(std::abs(out_left[0] - expected) < 1e-5);
    CHECK(std::abs(out_right[1] + expected) < 1e-5);
    sonare_free_floats(out_left);
    sonare_free_floats(out_right);
  }
}

TEST_CASE("the stereo normalize outputs are cleared before validation", "[c_api][effects]") {
  const std::vector<float> samples = {0.5f, -0.5f, 0.5f, -0.5f};

  // The C entry carries one length and one sample rate for both channels, so
  // the pair's length and rate mismatches are not expressible here; they are
  // covered where they are reachable, on the C++ entry.
  float* out_left = non_null_sentinel_float_ptr();
  float* out_right = non_null_sentinel_float_ptr();
  size_t out_length = 99;

  REQUIRE(sonare_normalize_stereo(nullptr, samples.data(), samples.size(), 22050, -1.0f, &out_left,
                                  &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out_left == nullptr);
  REQUIRE(out_right == nullptr);
  REQUIRE(out_length == 0);

  REQUIRE(sonare_normalize_stereo(samples.data(), nullptr, samples.size(), 22050, -1.0f, &out_left,
                                  &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), 0, 22050, -1.0f, &out_left,
                                  &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 0, -1.0f,
                                  &out_left, &out_right,
                                  &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050, -1.0f,
                                  nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // A non-finite target is the core's refusal, reached through the boundary rather than
  // short-circuited by it.
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050,
                                  std::numeric_limits<float>::infinity(), &out_left, &out_right,
                                  &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // Control: the same call with none of those faults succeeds, so the refusals
  // are about the arguments rather than about the entry point.
  REQUIRE(sonare_normalize_stereo(samples.data(), samples.data(), samples.size(), 22050, -1.0f,
                                  &out_left, &out_right, &out_length) == SONARE_OK);
  REQUIRE(out_left != nullptr);
  REQUIRE(out_right != nullptr);
  sonare_free_floats(out_left);
  sonare_free_floats(out_right);
}
