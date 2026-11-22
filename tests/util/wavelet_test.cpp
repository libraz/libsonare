/// @file wavelet_test.cpp
/// @brief Unit tests for filters/wavelet.

#include "filters/wavelet.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "util/exception.h"

using namespace sonare;

namespace {

std::vector<std::complex<float>> hann_wavelet(const std::vector<float>& freqs, int sr,
                                              float window_param, bool is_cqt, bool pad_fft = false,
                                              float Q = 0.0f, int* n_fft_out = nullptr) {
  return detail::wavelet_with_window(freqs, sr, window_param, is_cqt, pad_fft, Q, WindowType::Hann,
                                     n_fft_out);
}

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
  auto kernels = hann_wavelet(freqs, 22050, 1.0f, true);
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
  auto kernels = hann_wavelet(freqs, 22050, 1.0f, true, false, 1.0f);
  REQUIRE(kernels.size() == 1);
  REQUIRE(std::abs(kernels[0]) > 0.5f);
}

TEST_CASE("wavelet rejects one frequency without an explicit Q", "[util][wavelet]") {
  require_invalid_parameter([&] { wavelet_lengths({440.0f}, 22050, 1.0f); });
}

TEST_CASE("wavelet preserves the empty explicit-Q shape", "[util][wavelet]") {
  int n_fft = -1;
  const auto padded = hann_wavelet({}, 22050, 1.0f, true, true, 1.0f, &n_fft);
  REQUIRE(padded.empty());
  REQUIRE(n_fft == 1);

  n_fft = -1;
  const auto unpadded = hann_wavelet({}, 22050, 1.0f, true, false, 1.0f, &n_fft);
  REQUIRE(unpadded.empty());
  REQUIRE(n_fft == 0);
}

TEST_CASE("wavelet supports non-Hann periodic windows", "[util][wavelet]") {
  const std::vector<float> freqs{200.0f, 400.0f};
  const auto hann = hann_wavelet(freqs, 22050, 1.0f, true);
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
  require_invalid_parameter([&] { hann_wavelet({1e-30f, 2e-30f}, 22050, 1.0f, true); });

  // This finite Q produces an effective length above the generic FFT ceiling,
  // so the ceil-to-int/power-of-two path must reject before allocation.
  require_invalid_parameter([&] { hann_wavelet({1.5e-5f}, 22050, 1.0f, true, true, 1.0f); });
}
