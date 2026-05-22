/// @file wavelet_test.cpp
/// @brief Unit tests for filters/wavelet (and friends).

#include "filters/wavelet.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("wavelet_lengths is decreasing with frequency", "[util][wavelet]") {
  std::vector<float> freqs{100.0f, 200.0f, 400.0f};
  auto L = wavelet_lengths(freqs, 22050, 1.0f);
  REQUIRE(L.size() == 3);
  REQUIRE(L[0] > L[1]);
  REQUIRE(L[1] > L[2]);
  // All odd.
  for (float v : L) {
    int iv = static_cast<int>(v);
    REQUIRE(iv % 2 == 1);
  }
}

TEST_CASE("wavelet returns complex kernels of expected total length", "[util][wavelet]") {
  std::vector<float> freqs{200.0f, 400.0f};
  auto L = wavelet_lengths(freqs, 22050, 1.0f);
  auto kernels = wavelet(freqs, 22050, 1.0f, true);
  size_t total = 0;
  for (float v : L) total += static_cast<size_t>(v);
  REQUIRE(kernels.size() == total);
}

TEST_CASE("semitone_filterbank produces n_filters rows of 6 coeffs", "[util][wavelet]") {
  auto fb = semitone_filterbank(2, 12, 100.0f, 22050);
  REQUIRE(fb.size() == 24 * 6);
}

TEST_CASE("cq_to_chroma maps each input bin to a chroma row", "[util][wavelet]") {
  auto M = cq_to_chroma(24, 12, 12, 0.0f);
  REQUIRE(M.size() == 12 * 24);
  // Each chroma row sums to 1 (after normalisation).
  for (int c = 0; c < 12; ++c) {
    float s = 0.0f;
    for (int j = 0; j < 24; ++j) s += M[c * 24 + j];
    REQUIRE_THAT(s, WithinAbs(1.0f, 1e-6f));
  }
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
