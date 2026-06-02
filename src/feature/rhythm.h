#pragma once

/// @file rhythm.h
/// @brief Rhythm-domain features: tempogram and Fourier tempogram.
/// @details librosa.feature.tempogram, librosa.feature.fourier_tempogram,
///          librosa.feature.tempogram_ratio compatible.

#include <vector>

#include "core/audio.h"
#include "core/window.h"

namespace sonare {

enum class TempogramMode {
  kAutocorrelation,
  kCosine,
};

/// @brief Configuration for tempogram / fourier_tempogram.
struct TempogramConfig {
  int hop_length = 512;                  ///< Hop length used for the onset envelope
  int win_length = 384;                  ///< Window length in onset-envelope frames
  WindowType window = WindowType::Hann;  ///< Analysis window
  bool center = true;                    ///< Center-pad the onset envelope
  bool norm = true;                      ///< Max-abs (L-inf) normalize each column (librosa)
  TempogramMode mode = TempogramMode::kAutocorrelation;  ///< Tempogram similarity mode
};

/// @brief Onset autocorrelation tempogram.
/// @details Local autocorrelation of the onset envelope. Mirrors
///          librosa.feature.tempogram by default. Set mode=kCosine to compute
///          window-local cosine similarity between each lagged onset slice.
/// @param onset_envelope Pre-computed onset strength envelope (frames per
///        hop). Length must be > 0.
/// @param sr Sample rate (required to scale tempo axis when consumed by
///        tempogram_ratio; tempogram itself does not depend on sr).
/// @param config Configuration
/// @return Tempogram matrix [win_length x n_frames], row-major.
///         Row i, frame j is the autocorrelation value at lag i for the window
///         centered on frame j of the onset envelope.
std::vector<float> tempogram(const std::vector<float>& onset_envelope, int sr,
                             const TempogramConfig& config = TempogramConfig());

/// @brief Compute the tempogram directly from audio (via onset_strength).
std::vector<float> tempogram(const Audio& audio, const TempogramConfig& config = TempogramConfig());

/// @brief Fourier (FFT-based) tempogram: STFT of the onset envelope.
/// @return Magnitude matrix [n_bins x n_frames] where n_bins = win_length/2 + 1.
std::vector<float> fourier_tempogram(const std::vector<float>& onset_envelope, int sr,
                                     const TempogramConfig& config = TempogramConfig());

std::vector<float> fourier_tempogram(const Audio& audio,
                                     const TempogramConfig& config = TempogramConfig());

/// @brief Cyclic tempogram by octave-folding a Fourier tempogram.
/// @details Tempo bins are folded into one octave [bpm_min, 2*bpm_min), so
///          octave-equivalent tempi such as 60, 120, and 240 BPM contribute
///          to the same cyclic tempo class.
/// @param onset_envelope Pre-computed onset strength envelope
/// @param sr Sample rate
/// @param bpm_min Lower tempo of the cyclic octave
/// @param n_bins Number of cyclic tempo classes
/// @return Matrix [n_bins x n_frames], row-major
std::vector<float> cyclic_tempogram(const std::vector<float>& onset_envelope, int sr,
                                    const TempogramConfig& config = TempogramConfig(),
                                    float bpm_min = 60.0f, int n_bins = 60);

std::vector<float> cyclic_tempogram(const Audio& audio,
                                    const TempogramConfig& config = TempogramConfig(),
                                    float bpm_min = 60.0f, int n_bins = 60);

/// @brief Aggregated tempogram values at integer tempo ratios of a reference tempo.
/// @param tempogram_data Tempogram matrix as returned by tempogram()
/// @param win_length Tempogram win_length used to compute the lag axis
/// @param sr Sample rate
/// @param hop_length Hop length used for the onset envelope
/// @param factors Lag ratios to evaluate (default {0.5, 1, 2, 3, 4})
/// @return Vector of length factors.size() with the mean autocorrelation at
///         each ratio (averaged over frames).
std::vector<float> tempogram_ratio(const std::vector<float>& tempogram_data, int win_length, int sr,
                                   int hop_length,
                                   const std::vector<float>& factors = {0.5f, 1.0f, 2.0f, 3.0f,
                                                                        4.0f});

/// @brief Configuration for Predominant Local Pulse (PLP).
struct PlpConfig {
  int sr = 22050;
  int hop_length = 512;
  float tempo_min = 30.0f;
  float tempo_max = 300.0f;
  int win_length = 384;
};

/// @brief Predominant Local Pulse estimation (librosa.beat.plp).
/// @details Builds a Fourier tempogram, picks the most-likely tempo per frame
/// inside [tempo_min, tempo_max], then performs an inverse STFT of the masked
/// tempogram to produce a pulse curve aligned with the onset envelope.
/// @param onset_envelope Onset strength envelope frames.
/// @param config Configuration parameters.
/// @return Pulse curve of length onset_envelope.size().
std::vector<float> plp(const std::vector<float>& onset_envelope,
                       const PlpConfig& config = PlpConfig());

/// @brief Convenience overload computing PLP directly from audio.
std::vector<float> plp(const Audio& audio, const PlpConfig& config = PlpConfig());

}  // namespace sonare
