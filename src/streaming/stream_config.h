#pragma once

/// @file stream_config.h
/// @brief Configuration for streaming audio analysis.

#include "util/constants.h"
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
  /// @brief Sample rate in Hz.
  /// @details Defaults to 44100 Hz, *intentionally different* from the batch
  ///          MusicAnalyzer default of 22050 Hz (constants::kDefaultSampleRate,
  ///          chosen for librosa parity). Real-time audio reaches the analyzer
  ///          straight from the playback/capture graph (AudioWorklet, device
  ///          callbacks), which almost always runs at 44100/48000 Hz. Matching
  ///          the native graph rate avoids an extra resample on the hot path and
  ///          keeps timestamps aligned with the audio clock. The analyzer
  ///          resamples internally only when the input exceeds 44100 Hz.
  int sample_rate = 44100;
  int n_fft = 2048;                      ///< FFT size
  int hop_length = 512;                  ///< Hop length between frames
  WindowType window = WindowType::Hann;  ///< Window function type

  // Feature computation flags
  /// @brief Populate StreamFrame::magnitude (raw per-frame magnitude spectrum).
  /// @details Defaults to false: no SOA read path surfaces the per-frame
  /// magnitude, so enabling it only costs a per-frame allocation+copy on the
  /// realtime path with no readable result (centroid/flatness use a separate
  /// internal buffer). The flat C ABI rejects a non-zero value for the same
  /// reason, so leaving it off keeps the default config portable across surfaces.
  bool compute_magnitude = false;  ///< Compute magnitude spectrum (not surfaced; see above)
  bool compute_mel = true;         ///< Compute mel spectrogram
  bool compute_chroma = true;      ///< Compute chromagram
  /// @brief Compute onset strength (spectral flux of the log-mel spectrum).
  /// @details Onset strength — and the progressive BPM estimate built on top of
  ///          it — is derived from the mel path. When @c compute_onset is true
  ///          the StreamAnalyzer constructor force-enables @c compute_mel (the
  ///          coercion is visible via StreamAnalyzer::config()); otherwise onset
  ///          would be identically 0 and BPM would never converge.
  bool compute_onset = true;
  bool compute_spectral = true;  ///< Compute spectral features

  // Mel configuration
  int n_mels = 128;   ///< Number of mel bands
  float fmin = 0.0f;  ///< Minimum frequency for mel
  float fmax = 0.0f;  ///< Maximum frequency (0 = sr/2)

  // Tuning configuration
  float tuning_ref_hz = constants::kA4Hz;  ///< Reference frequency for A4

  // Output configuration
  OutputFormat output_format = OutputFormat::Float32;
  int emit_every_n_frames = 1;   ///< Emit every N frames (for throttling)
  int magnitude_downsample = 1;  ///< Downsample factor for magnitude

  // Progressive estimation configuration
  float key_update_interval_sec = 5.0f;   ///< Interval for key re-estimation
  float bpm_update_interval_sec = 10.0f;  ///< Interval for BPM re-estimation

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
