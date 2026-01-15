#pragma once

/// @file stream_config.h
/// @brief Configuration for streaming audio analysis.

#include "util/types.h"

namespace sonare {

/// @brief Output format for streaming data.
enum class OutputFormat {
  Float32,  ///< Full precision float
  Int16,    ///< 16-bit signed integer (for bandwidth reduction)
  Uint8,    ///< 8-bit unsigned integer (for visualization)
};

/// @brief Configuration for StreamAnalyzer.
struct StreamConfig {
  // Basic parameters
  int sample_rate = 44100;                  ///< Sample rate in Hz
  int n_fft = 2048;                         ///< FFT size
  int hop_length = 512;                     ///< Hop length between frames
  WindowType window = WindowType::Hann;     ///< Window function type

  // Feature computation flags
  bool compute_magnitude = true;            ///< Compute magnitude spectrum
  bool compute_mel = true;                  ///< Compute mel spectrogram
  bool compute_chroma = true;               ///< Compute chromagram
  bool compute_onset = true;                ///< Compute onset strength
  bool compute_spectral = true;             ///< Compute spectral features

  // Mel configuration
  int n_mels = 128;                         ///< Number of mel bands
  float fmin = 0.0f;                        ///< Minimum frequency for mel
  float fmax = 0.0f;                        ///< Maximum frequency (0 = sr/2)

  // Tuning configuration
  float tuning_ref_hz = 440.0f;             ///< Reference frequency for A4

  // Output configuration
  OutputFormat output_format = OutputFormat::Float32;
  int emit_every_n_frames = 1;              ///< Emit every N frames (for throttling)
  int magnitude_downsample = 1;             ///< Downsample factor for magnitude

  // Progressive estimation configuration
  float key_update_interval_sec = 5.0f;     ///< Interval for key re-estimation
  float bpm_update_interval_sec = 10.0f;    ///< Interval for BPM re-estimation

  // Helper methods

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_fft / 2 + 1; }

  /// @brief Returns overlap size in samples.
  int overlap() const { return n_fft - hop_length; }

  /// @brief Returns frame duration in seconds.
  float frame_duration() const {
    return static_cast<float>(hop_length) / static_cast<float>(sample_rate);
  }

  /// @brief Returns maximum frequency for mel.
  float effective_fmax() const {
    return fmax > 0.0f ? fmax : static_cast<float>(sample_rate) / 2.0f;
  }
};

}  // namespace sonare
