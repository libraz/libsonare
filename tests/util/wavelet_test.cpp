/// @file wavelet_test.cpp
/// @brief Unit tests for filters/wavelet (and friends).

#include "filters/wavelet.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "rt/biquad_design.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("wavelet_lengths is decreasing with frequency", "[util][wavelet]") {
  std::vector<float> freqs{100.0f, 200.0f, 400.0f};
  auto L = wavelet_lengths(freqs, 22050, 1.0f);
  REQUIRE(L.size() == 3);
  REQUIRE(L[0] > L[1]);
  REQUIRE(L[1] > L[2]);
  for (float v : L) REQUIRE(v > 0.0f);
}

TEST_CASE("wavelet returns complex kernels of expected total length", "[util][wavelet]") {
  std::vector<float> freqs{200.0f, 400.0f};
  auto L = wavelet_lengths(freqs, 22050, 1.0f);
  auto kernels = wavelet(freqs, 22050, 1.0f, true);
  // Effective integer length L_k = floor(ilen/2) - floor(-ilen/2) which equals
  // floor(ilen) for fractional ilen and exactly ilen for integer.
  auto eff_len = [](float ilen) {
    int s = static_cast<int>(std::floor(-ilen / 2.0f));
    int e = static_cast<int>(std::floor(ilen / 2.0f));
    return e - s;
  };
  size_t total = 0;
  for (float v : L) total += static_cast<size_t>(eff_len(v));
  REQUIRE(kernels.size() == total);
}

TEST_CASE("wavelet length-1 kernel is non-zero (scipy get_window Nx==1)", "[util][wavelet]") {
  // A frequency at/above Nyquist yields a single-sample kernel (L < 2). The
  // periodic-Hann window value at n==0 would be 0, zeroing the kernel; scipy's
  // get_window returns 1.0 for a length-1 window, so the kernel must survive.
  std::vector<float> freqs{44100.0f};  // L = 22050 / 44100 = 0.5 -> Lk == 1
  auto L = wavelet_lengths(freqs, 22050, 1.0f);
  REQUIRE(L[0] < 2.0f);
  auto kernels = wavelet(freqs, 22050, 1.0f, true);
  REQUIRE(kernels.size() == 1);
  REQUIRE(std::abs(kernels[0]) > 0.5f);
}

TEST_CASE("semitone_filterbank produces n_filters rows of 6 coeffs", "[util][wavelet]") {
  auto fb = semitone_filterbank(2, 12, 100.0f, 22050);
  REQUIRE(fb.size() == 24 * 6);
}

TEST_CASE("semitone_filterbank rows match shared RBJ bandpass design", "[util][wavelet]") {
  constexpr float fmin = 100.0f;
  constexpr int sr = 22050;
  auto fb = semitone_filterbank(1, 12, fmin, sr);
  REQUIRE(fb.size() == 12 * 6);

  const auto coeffs = sonare::rt::rbj_bandpass_d(fmin, sr, 25.0);
  const double a0 = fb[3];
  REQUIRE_THAT(static_cast<double>(fb[0]) / a0, WithinAbs(coeffs.b0, 1e-7));
  REQUIRE_THAT(static_cast<double>(fb[1]) / a0, WithinAbs(coeffs.b1, 1e-7));
  REQUIRE_THAT(static_cast<double>(fb[2]) / a0, WithinAbs(coeffs.b2, 1e-7));
  REQUIRE_THAT(static_cast<double>(fb[4]) / a0, WithinAbs(coeffs.a1, 1e-7));
  REQUIRE_THAT(static_cast<double>(fb[5]) / a0, WithinAbs(coeffs.a2, 1e-7));
}

TEST_CASE("cq_to_chroma maps each input bin to a chroma row", "[util][wavelet]") {
  auto M = cq_to_chroma(24, 12, 12, 0.0f);
  REQUIRE(M.size() == 12 * 24);
  // Each CQT input column sums to 1 after folding, preserving per-bin energy.
  for (int j = 0; j < 24; ++j) {
    float s = 0.0f;
    for (int c = 0; c < 12; ++c) s += M[c * 24 + j];
    REQUIRE_THAT(s, WithinAbs(1.0f, 1e-6f));
  }
}

TEST_CASE("cq_to_chroma applies fmin and tuning pitch-class offset", "[util][wavelet]") {
  auto tuned = cq_to_chroma(12, 12, 12, 440.0f, 1.0f);
  auto untuned = cq_to_chroma(12, 12, 12, 440.0f, 0.0f);

  // A4 is pitch class 9; adding +1 semitone tuning moves the first bin to A#.
  REQUIRE_THAT(untuned[9 * 12], WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(tuned[10 * 12], WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(tuned[9 * 12], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("diagonal_filter has its peak on the chosen diagonal", "[util][wavelet]") {
  auto D = diagonal_filter(5, /*direction=*/1, 1.0f);
  REQUIRE(D.size() == 25);
  // Diagonal entries == 1, off-diagonal smaller.
  for (int i = 0; i < 5; ++i) {
    REQUIRE_THAT(D[i * 5 + i], WithinAbs(1.0f, 1e-6f));
  }
}

TEST_CASE("window_bandwidth of a rectangular window is 1", "[util][wavelet]") {
  std::vector<float> rect(64, 1.0f);
  float bw = window_bandwidth(rect);
  REQUIRE_THAT(bw, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("window_sumsquare length is consistent with OLA", "[util][wavelet]") {
  std::vector<float> window(8, 1.0f);
  auto ss = window_sumsquare(window, /*n_frames=*/4, /*hop_length=*/2, /*n_fft=*/8);
  REQUIRE(ss.size() == 8 + 2 * 3);
  for (float v : ss) REQUIRE(v >= 0.0f);
}
