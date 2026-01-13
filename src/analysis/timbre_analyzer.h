#pragma once

/// @file timbre_analyzer.h
/// @brief Timbre analysis for extracting perceptual sound characteristics.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"

namespace sonare {

/// @brief Timbral characteristics of audio.
struct Timbre {
  float brightness;  ///< Spectral brightness [0, 1] (high = bright/harsh)
  float warmth;      ///< Spectral warmth [0, 1] (high = warm/full)
  float density;     ///< Spectral density [0, 1] (high = rich/complex)
  float roughness;   ///< Spectral roughness [0, 1] (high = rough/harsh)
  float complexity;  ///< Harmonic complexity [0, 1] (high = complex)
};

/// @brief Configuration for timbre analysis.
struct TimbreConfig {
  int n_fft = 2048;         ///< FFT size
  int hop_length = 512;     ///< Hop length
  int n_mels = 128;         ///< Number of mel bands
  int n_mfcc = 13;          ///< Number of MFCC coefficients
  float window_sec = 0.5f;  ///< Window size for time-varying analysis
};

/// @brief Timbre analyzer for extracting sound characteristics.
/// @details Analyzes spectral features to extract perceptual timbral qualities
/// such as brightness, warmth, density, and roughness.
class TimbreAnalyzer {
 public:
  /// @brief Constructs timbre analyzer from audio.
  /// @param audio Input audio
  /// @param config Timbre analysis configuration
  explicit TimbreAnalyzer(const Audio& audio, const TimbreConfig& config = TimbreConfig());

  /// @brief Constructs timbre analyzer from pre-computed features.
  /// @param spec Spectrogram
  /// @param mel_spec Mel spectrogram
  /// @param config Timbre analysis configuration
  TimbreAnalyzer(const Spectrogram& spec, const MelSpectrogram& mel_spec,
                 const TimbreConfig& config = TimbreConfig());

  /// @brief Returns overall timbre characteristics.
  const Timbre& timbre() const { return timbre_; }

  /// @brief Returns timbre over time.
  /// @return Vector of Timbre for each time window
  const std::vector<Timbre>& timbre_over_time() const { return timbre_over_time_; }

  /// @brief Returns brightness level [0, 1].
  float brightness() const { return timbre_.brightness; }

  /// @brief Returns warmth level [0, 1].
  float warmth() const { return timbre_.warmth; }

  /// @brief Returns density level [0, 1].
  float density() const { return timbre_.density; }

  /// @brief Returns roughness level [0, 1].
  float roughness() const { return timbre_.roughness; }

  /// @brief Returns complexity level [0, 1].
  float complexity() const { return timbre_.complexity; }

  /// @brief Returns spectral centroid values.
  const std::vector<float>& spectral_centroid() const { return spectral_centroid_; }

  /// @brief Returns spectral flatness values.
  const std::vector<float>& spectral_flatness() const { return spectral_flatness_; }

  /// @brief Returns spectral rolloff values.
  const std::vector<float>& spectral_rolloff() const { return spectral_rolloff_; }

 private:
  void init_from_features(const Spectrogram& spec, const MelSpectrogram& mel_spec);
  void analyze();
  void compute_overall_timbre();
  Timbre compute_window_timbre(int start_frame, int end_frame) const;

  Timbre timbre_;
  std::vector<Timbre> timbre_over_time_;
  std::vector<float> spectral_centroid_;
  std::vector<float> spectral_flatness_;
  std::vector<float> spectral_rolloff_;
  std::vector<float> spectral_flux_;
  std::vector<float> mfcc_variance_;
  int n_frames_;
  int sr_;
  TimbreConfig config_;
};

}  // namespace sonare
