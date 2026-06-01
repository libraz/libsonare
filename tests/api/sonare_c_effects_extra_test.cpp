/// @file sonare_c_effects_extra_test.cpp
/// @brief Tests for the extended C API effects wrappers (decompose, nn_filter,
///        remix, hpss_with_residual, phase_vocoder).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sonare_c.h"
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

}  // namespace

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
