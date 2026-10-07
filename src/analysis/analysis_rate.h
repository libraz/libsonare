#pragma once

/// @file analysis_rate.h
/// @brief Converts a window length given in samples at 22050 Hz into the input rate.
/// @details High-level analyzers state `n_fft` as a time quantity (samples at
///          @ref kAnalysisSampleRate). The hop stays in caller-buffer samples.

#include <algorithm>
#include <cmath>

#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

inline constexpr int kAnalysisSampleRate = constants::kDefaultSampleRate;

/// @brief Smallest m >= n with m = 2^a * 3^b * 5^c and m % 32 == 0.
inline int fast_fft_length(int n) {
  SONARE_CHECK(n > 0, ErrorCode::InvalidParameter);
  for (long long m = ((static_cast<long long>(n) + 31) / 32) * 32;; m += 32) {
    long long r = m;
    for (int f : {2, 3, 5}) {
      while (r % f == 0) r /= f;
    }
    if (r == 1) return static_cast<int>(m);
  }
}

/// Window length and FFT length at the input rate (`win_length` 0 = same as `n_fft`).
struct RateWindow {
  int win_length;
  int n_fft;
};

/// @brief Converts @p n_fft_at_analysis_rate to the window and FFT length for @p sample_rate.
/// @details Factor 1 returns `{0, n_fft}` unchanged. Throws if the FFT length exceeds
///          @ref kMaxStftNFft.
inline RateWindow window_at_rate(int n_fft_at_analysis_rate, int sample_rate) {
  SONARE_CHECK(n_fft_at_analysis_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  if (sample_rate == kAnalysisSampleRate) return {0, n_fft_at_analysis_rate};
  const double scaled = static_cast<double>(n_fft_at_analysis_rate) *
                        static_cast<double>(sample_rate) / kAnalysisSampleRate;
  SONARE_CHECK(scaled <= static_cast<double>(kMaxStftNFft), ErrorCode::InvalidParameter);
  const int win = std::max(2, static_cast<int>(std::lround(scaled)));
  const int fft = fast_fft_length(win);
  SONARE_CHECK(fft <= kMaxStftNFft, ErrorCode::InvalidParameter);
  return {win, fft};
}

/// @brief StftConfig with the window converted to @p sample_rate; other fields are defaults.
inline StftConfig stft_config_at_rate(int n_fft_at_analysis_rate, int hop_length, int sample_rate) {
  const RateWindow w = window_at_rate(n_fft_at_analysis_rate, sample_rate);
  StftConfig config;
  config.n_fft = w.n_fft;
  config.hop_length = hop_length;
  config.win_length = w.win_length;
  return config;
}

}  // namespace sonare
