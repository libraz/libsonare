#pragma once

/// @file hpss.h
/// @brief Harmonic-Percussive Source Separation (HPSS).

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"

namespace sonare {

/// @brief Configuration for HPSS algorithm.
struct HpssConfig {
  int kernel_size_harmonic = 31;    ///< Horizontal median filter size (harmonic)
  int kernel_size_percussive = 31;  ///< Vertical median filter size (percussive)
  float power = 2.0f;               ///< Power for soft mask computation
  float margin_harmonic = 1.0f;     ///< Margin for harmonic mask
  float margin_percussive = 1.0f;   ///< Margin for percussive mask
  bool use_soft_mask = true;        ///< Use soft masks (false = hard masks)
};

/// @brief Result of HPSS on spectrogram.
struct HpssSpectrogramResult {
  Spectrogram harmonic;    ///< Harmonic component spectrogram
  Spectrogram percussive;  ///< Percussive component spectrogram
};

/// @brief Result of HPSS on audio.
struct HpssAudioResult {
  Audio harmonic;    ///< Harmonic component audio
  Audio percussive;  ///< Percussive component audio
};

/// @brief Result of HPSS with residual on spectrogram.
struct HpssSpectrogramResultWithResidual {
  Spectrogram harmonic;    ///< Harmonic component spectrogram
  Spectrogram percussive;  ///< Percussive component spectrogram
  Spectrogram residual;    ///< Residual component spectrogram
};

/// @brief Result of HPSS with residual on audio.
struct HpssAudioResultWithResidual {
  Audio harmonic;    ///< Harmonic component audio
  Audio percussive;  ///< Percussive component audio
  Audio residual;    ///< Residual component audio
};

/// @brief Applies horizontal median filter to magnitude spectrogram.
/// @param magnitude Magnitude spectrogram [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param kernel_size Filter kernel size (must be odd)
/// @return Filtered magnitude [n_bins x n_frames]
std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size);

/// @brief Applies vertical median filter to magnitude spectrogram.
/// @param magnitude Magnitude spectrogram [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param kernel_size Filter kernel size (must be odd)
/// @return Filtered magnitude [n_bins x n_frames]
std::vector<float> median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                          int kernel_size);

/// @brief Performs HPSS on a spectrogram.
/// @param spec Input spectrogram
/// @param config HPSS configuration
/// @return Harmonic and percussive spectrograms
HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config = HpssConfig());

/// @brief Performs HPSS on audio and returns separated audio signals.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration for analysis/synthesis
/// @return Harmonic and percussive audio signals
HpssAudioResult hpss(const Audio& audio, const HpssConfig& config = HpssConfig(),
                     const StftConfig& stft_config = StftConfig());

/// @brief Extracts only harmonic component from audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Harmonic audio
Audio harmonic(const Audio& audio, const HpssConfig& config = HpssConfig(),
               const StftConfig& stft_config = StftConfig());

/// @brief Extracts only percussive component from audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Percussive audio
Audio percussive(const Audio& audio, const HpssConfig& config = HpssConfig(),
                 const StftConfig& stft_config = StftConfig());

/// @brief Performs HPSS with residual component on spectrogram.
/// @param spec Input spectrogram
/// @param config HPSS configuration
/// @return Harmonic, percussive, and residual spectrograms
/// @details Residual = Original - Harmonic - Percussive
HpssSpectrogramResultWithResidual hpss_with_residual(const Spectrogram& spec,
                                                     const HpssConfig& config = HpssConfig());

/// @brief Performs HPSS with residual component on audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Harmonic, percussive, and residual audio signals
HpssAudioResultWithResidual hpss_with_residual(const Audio& audio,
                                               const HpssConfig& config = HpssConfig(),
                                               const StftConfig& stft_config = StftConfig());

/// @brief Extracts residual component from audio (what remains after H+P removal).
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Residual audio
Audio residual(const Audio& audio, const HpssConfig& config = HpssConfig(),
               const StftConfig& stft_config = StftConfig());

}  // namespace sonare
