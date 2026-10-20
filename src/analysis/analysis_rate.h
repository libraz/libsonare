#pragma once

/// @file analysis_rate.h
/// @brief Converts a window length given in samples at 22050 Hz into the input rate.
/// @details High-level analyzers state `n_fft` as a time quantity (samples at
///          @ref kAnalysisSampleRate). The hop stays in caller-buffer samples.

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

inline constexpr int kAnalysisSampleRate = constants::kDefaultSampleRate;

/// @brief Returns @p audio resampled to @ref kAnalysisSampleRate when non-empty and above it, else
/// unchanged.
inline Audio analysis_rate_audio(const Audio& audio) {
  if (!audio.empty() && audio.sample_rate() > kAnalysisSampleRate) {
    return resample(audio, kAnalysisSampleRate);
  }
  return audio;
}

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

/// @brief Converts a window of @p samples_at_reference_rate samples at @p reference_rate to
///        samples at @p sample_rate, rounded and at least 2.
/// @details For a frame that needs no FFT length of its own (a YIN frame, say); an STFT window
///          goes through @ref window_at_rate. Throws above @ref kMaxStftNFft.
inline int window_length_at_rate(int samples_at_reference_rate, int sample_rate,
                                 int reference_rate = kAnalysisSampleRate) {
  SONARE_CHECK(samples_at_reference_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(reference_rate > 0, ErrorCode::InvalidParameter);
  const double scaled = static_cast<double>(samples_at_reference_rate) *
                        static_cast<double>(sample_rate) / reference_rate;
  SONARE_CHECK(scaled <= static_cast<double>(kMaxStftNFft), ErrorCode::InvalidParameter);
  return std::max(2, static_cast<int>(std::lround(scaled)));
}

/// @brief Converts @p n_fft_at_reference_rate to the window and FFT length for @p sample_rate.
/// @details The window is a time quantity in samples at @p reference_rate. Factor 1 returns
///          `{0, n_fft}` unchanged. Throws if the FFT length exceeds @ref kMaxStftNFft.
inline RateWindow window_at_rate(int n_fft_at_reference_rate, int sample_rate,
                                 int reference_rate = kAnalysisSampleRate) {
  const int win = window_length_at_rate(n_fft_at_reference_rate, sample_rate, reference_rate);
  if (sample_rate == reference_rate) return {0, n_fft_at_reference_rate};
  const int fft = fast_fft_length(win);
  SONARE_CHECK(fft <= kMaxStftNFft, ErrorCode::InvalidParameter);
  return {win, fft};
}

/// @brief StftConfig with the window converted to @p sample_rate; other fields are defaults.
/// @details @p hop_length is used as given (samples of the input buffer).
inline StftConfig stft_config_at_rate(int n_fft_at_reference_rate, int hop_length, int sample_rate,
                                      int reference_rate = kAnalysisSampleRate) {
  const RateWindow w = window_at_rate(n_fft_at_reference_rate, sample_rate, reference_rate);
  StftConfig config;
  config.n_fft = w.n_fft;
  config.hop_length = hop_length;
  config.win_length = w.win_length;
  return config;
}

/// @brief @p at_reference with its window and hop converted from @p reference_rate to
///        @p sample_rate.
/// @details For a geometry whose hop is a time quantity as well as its window. An explicit
///          `win_length` (neither 0 nor `n_fft`) is converted on its own, capped at the FFT
///          length. Other fields are kept. The same rate returns @p at_reference unchanged.
/// @throws SonareException(InvalidParameter) on what @ref window_at_rate refuses, a
///         non-positive hop, or a `win_length` outside [0, n_fft].
inline StftConfig stft_config_scaled_to_rate(const StftConfig& at_reference, int sample_rate,
                                             int reference_rate) {
  if (sample_rate == reference_rate) return at_reference;
  const RateWindow w = window_at_rate(at_reference.n_fft, sample_rate, reference_rate);
  SONARE_CHECK(at_reference.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(at_reference.win_length >= 0 && at_reference.win_length <= at_reference.n_fft,
               ErrorCode::InvalidParameter);
  const auto scale = [&](int samples) {
    return std::lround(static_cast<double>(samples) * sample_rate / reference_rate);
  };
  StftConfig config = at_reference;
  config.n_fft = w.n_fft;
  config.win_length = w.win_length;
  if (at_reference.win_length != 0 && at_reference.win_length != at_reference.n_fft) {
    config.win_length =
        std::min(w.n_fft, std::max(2, static_cast<int>(scale(at_reference.win_length))));
  }
  config.hop_length = std::max(1, static_cast<int>(scale(at_reference.hop_length)));
  return config;
}

/// @brief Detrended onset strength (lag 1) of @p audio over a mel spectrogram of
///        @ref constants::kDefaultNMels bands, with the STFT window given as is.
/// @details The front end shared by the BPM, beat and onset analyzers; @p hop_length is in input
///          samples.
inline std::vector<float> onset_strength_in_window(const Audio& audio, const RateWindow& window,
                                                   int hop_length) {
  MelConfig mel_config;
  mel_config.n_fft = window.n_fft;
  mel_config.win_length = window.win_length;
  mel_config.hop_length = hop_length;
  mel_config.n_mels = constants::kDefaultNMels;

  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;

  return compute_onset_strength(audio, mel_config, onset_config);
}

/// @brief @ref onset_strength_in_window with @p n_fft taken as samples at
///        @ref kAnalysisSampleRate and converted to the input rate.
inline std::vector<float> analysis_onset_strength(const Audio& audio, int n_fft, int hop_length) {
  return onset_strength_in_window(audio, window_at_rate(n_fft, audio.sample_rate()), hop_length);
}

}  // namespace sonare
