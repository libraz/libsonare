/// @file wavelet_test.cpp
/// @brief Unit tests for filters/wavelet (and friends).

#include "filters/wavelet.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "rt/biquad_design.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

template <typename Operation>
void require_invalid_parameter(Operation&& operation) {
  try {
    operation();
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
    return;
  } catch (...) {
    FAIL("operation threw a non-SonareException");
  }
  FAIL("operation did not throw");
}

}  // namespace

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
  // A single frequency cannot infer relative bandwidth, so provide Q explicitly.
  std::vector<float> freqs{44100.0f};  // L = 22050 / 44100 = 0.5 -> Lk == 1
  auto L = wavelet_lengths(freqs, 22050, 1.0f, 1.0f);
  REQUIRE(L[0] < 2.0f);
  auto kernels = wavelet(freqs, 22050, 1.0f, true, false, 1.0f);
  REQUIRE(kernels.size() == 1);
  REQUIRE(std::abs(kernels[0]) > 0.5f);
}

TEST_CASE("wavelet rejects one frequency without an explicit Q", "[util][wavelet]") {
  require_invalid_parameter([&] { wavelet_lengths({440.0f}, 22050, 1.0f); });
}

TEST_CASE("wavelet preserves the empty explicit-Q shape", "[util][wavelet]") {
  int n_fft = -1;
  const auto padded = wavelet({}, 22050, 1.0f, true, true, 1.0f, &n_fft);
  REQUIRE(padded.empty());
  REQUIRE(n_fft == 1);

  n_fft = -1;
  const auto unpadded = wavelet({}, 22050, 1.0f, true, false, 1.0f, &n_fft);
  REQUIRE(unpadded.empty());
  REQUIRE(n_fft == 0);
}

TEST_CASE("wavelet supports non-Hann periodic windows", "[util][wavelet]") {
  const std::vector<float> freqs{200.0f, 400.0f};
  const auto hann = wavelet(freqs, 22050, 1.0f, true);
  const auto hamming =
      detail::wavelet_with_window(freqs, 22050, 1.0f, true, false, 0.0f, WindowType::Hamming);
  const auto blackman =
      detail::wavelet_with_window(freqs, 22050, 1.0f, true, false, 0.0f, WindowType::Blackman);
  const auto rectangular =
      detail::wavelet_with_window(freqs, 22050, 1.0f, true, false, 0.0f, WindowType::Rectangular);

  REQUIRE(hann.size() == hamming.size());
  REQUIRE(hann.size() == blackman.size());
  REQUIRE(hann.size() == rectangular.size());
  for (const auto& selected : {hamming, blackman, rectangular}) {
    double max_diff = 0.0;
    for (size_t i = 0; i < hann.size(); ++i) {
      max_diff = std::max(max_diff, static_cast<double>(std::abs(hann[i] - selected[i])));
    }
    REQUIRE(max_diff > 1e-5);
  }
}

TEST_CASE("wavelet validates non-finite and unrepresentable lengths", "[util][wavelet]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  require_invalid_parameter([&] { wavelet_lengths({100.0f, nan}, 22050, 1.0f); });
  require_invalid_parameter([&] { wavelet_lengths({100.0f, inf}, 22050, 1.0f); });
  require_invalid_parameter([&] { wavelet_lengths({100.0f, 200.0f}, 22050, nan); });
  require_invalid_parameter([&] { wavelet_lengths({100.0f, 200.0f}, 22050, inf); });
  require_invalid_parameter([&] { wavelet_lengths({100.0f, 200.0f}, 22050, 1.0f, nan); });
  require_invalid_parameter([&] { wavelet_lengths({100.0f, 200.0f}, 22050, 1.0f, inf); });

  // The raw lengths remain finite, but the derived floor indices cannot fit in
  // int. wavelet must reject before converting them or allocating the output.
  require_invalid_parameter([&] { wavelet({1e-30f, 2e-30f}, 22050, 1.0f, true); });

  // This finite Q produces an effective length above the generic FFT ceiling,
  // so the ceil-to-int/power-of-two path must reject before allocation.
  require_invalid_parameter([&] { wavelet({1.5e-5f}, 22050, 1.0f, true, true, 1.0f); });
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

TEST_CASE("cq_to_chroma centers merge groups like librosa when bins_per_octave > n_chroma",
          "[util][wavelet]") {
  // 36 bins/octave folded to 12 chroma: n_merge = 3, so librosa rolls the
  // repeated identity left by n_merge // 2 = 1 column to center each chroma bin
  // on its 3 CQ bins. Input bin 2 therefore lands in chroma row 1 (centered),
  // not row 0 as an uncentered floor mapping would give.
  auto M = cq_to_chroma(36, 36, 12, /*fmin=*/0.0f);
  REQUIRE(M.size() == 12 * 36);
  auto chroma_of = [&](int i) {
    for (int c = 0; c < 12; ++c) {
      if (M[c * 36 + i] > 0.5f) return c;
    }
    return -1;
  };
  REQUIRE(chroma_of(0) == 0);
  REQUIRE(chroma_of(1) == 0);
  REQUIRE(chroma_of(2) == 1);  // centered (an uncentered mapping gives 0)
  REQUIRE(chroma_of(3) == 1);
  REQUIRE(chroma_of(5) == 2);
  // Every input column still maps to exactly one chroma row.
  for (int i = 0; i < 36; ++i) {
    REQUIRE(chroma_of(i) >= 0);
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
