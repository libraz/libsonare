#pragma once

/// @file stream_frame.h
/// @brief Frame structures for streaming audio analysis.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare {

/// @brief A detected chord change in the progression.
struct ChordChange {
  int root = -1;              ///< Chord root (0-11 for C-B, -1 = unknown)
  int quality = 0;            ///< Chord quality (0=Maj, 1=Min, 2=Dim, etc.)
  float start_time = 0.0f;    ///< Start time in seconds
  float confidence = 0.0f;    ///< Detection confidence (0-1)
};

/// @brief A single frame of analysis results.
/// @details Contains all computed features for one STFT frame.
/// The timestamp represents stream time (input sample position),
/// not necessarily AudioContext.currentTime.
struct StreamFrame {
  /// @brief Timestamp in seconds (stream time).
  /// @note This is computed as (sample_offset + frame_index * hop_length) / sample_rate.
  ///       It may not directly match AudioContext.currentTime due to processing latency.
  float timestamp = 0.0f;

  /// @brief Frame index (0-based, cumulative).
  int frame_index = 0;

  // Frequency domain features (sizes depend on config)
  std::vector<float> magnitude;   ///< Magnitude spectrum [n_bins] or downsampled
  std::vector<float> mel;         ///< Mel spectrogram [n_mels]
  std::vector<float> chroma;      ///< Chromagram [12]

  // Scalar spectral features
  float spectral_centroid = 0.0f;   ///< Spectral centroid in Hz
  float spectral_flatness = 0.0f;   ///< Spectral flatness (0-1)
  float rms_energy = 0.0f;          ///< RMS energy (normalized)

  // Onset detection (1-frame lag)
  float onset_strength = 0.0f;      ///< Onset strength value
  bool onset_valid = false;         ///< False for frame_index == 0 (no previous frame)
};

/// @brief Progressive estimation results for BPM, Key, and Chord.
/// @details Updated periodically based on accumulated data.
/// Confidence increases as more data is processed.
struct ProgressiveEstimate {
  // BPM estimation
  float bpm = 0.0f;                 ///< Estimated BPM (0 if not yet estimated)
  float bpm_confidence = 0.0f;      ///< Confidence (0-1, increases over time)
  int bpm_candidate_count = 0;      ///< Number of BPM candidates considered

  // Key estimation
  int key = -1;                     ///< Estimated key (0-11 for C-B, -1 = unknown)
  bool key_minor = false;           ///< True if minor mode
  float key_confidence = 0.0f;      ///< Confidence (0-1, increases over time)

  // Chord estimation (current chord, updated per frame)
  int chord_root = -1;              ///< Current chord root (0-11 for C-B, -1 = unknown)
  int chord_quality = 0;            ///< Chord quality (0=Maj, 1=Min, 2=Dim, etc.)
  float chord_confidence = 0.0f;    ///< Chord detection confidence (0-1)

  // Chord progression (accumulated over time)
  std::vector<ChordChange> chord_progression;  ///< Detected chord changes

  // Objective statistics (for UI display)
  float accumulated_seconds = 0.0f; ///< Total audio processed
  int used_frames = 0;              ///< Number of frames used for estimation
  bool updated = false;             ///< True if estimate was updated this frame
};

/// @brief Statistics and current state of the analyzer.
struct AnalyzerStats {
  int total_frames = 0;             ///< Total frames processed
  size_t total_samples = 0;         ///< Total samples processed
  float duration_seconds = 0.0f;    ///< Total duration processed
  ProgressiveEstimate estimate;     ///< Current progressive estimate
};

/// @brief Frame buffer in Structure of Arrays format.
/// @details More efficient for postMessage transfer (contiguous arrays).
struct FrameBuffer {
  size_t n_frames = 0;              ///< Number of frames in buffer

  std::vector<float> timestamps;    ///< [n_frames]
  std::vector<float> mel;           ///< [n_frames * n_mels] (row-major)
  std::vector<float> chroma;        ///< [n_frames * 12] (row-major)
  std::vector<float> onset_strength;///< [n_frames]
  std::vector<float> rms_energy;    ///< [n_frames]
  std::vector<float> spectral_centroid; ///< [n_frames]
  std::vector<float> spectral_flatness; ///< [n_frames]

  /// @brief Clears all data.
  void clear() {
    n_frames = 0;
    timestamps.clear();
    mel.clear();
    chroma.clear();
    onset_strength.clear();
    rms_energy.clear();
    spectral_centroid.clear();
    spectral_flatness.clear();
  }

  /// @brief Reserves capacity for n frames.
  void reserve(size_t n, int n_mels) {
    timestamps.reserve(n);
    mel.reserve(n * n_mels);
    chroma.reserve(n * 12);
    onset_strength.reserve(n);
    rms_energy.reserve(n);
    spectral_centroid.reserve(n);
    spectral_flatness.reserve(n);
  }
};

/// @brief Quantization configuration for bandwidth reduction.
struct QuantizeConfig {
  /// @brief dB range for mel spectrogram quantization.
  /// @details Values below mel_db_min are clipped to 0.
  float mel_db_min = -80.0f;

  /// @brief Maximum dB value for mel spectrogram.
  /// @details Values above mel_db_max are clipped to max.
  float mel_db_max = 0.0f;

  /// @brief Maximum expected onset strength value.
  /// @details Used to normalize onset to 0-1 range before quantization.
  float onset_max = 50.0f;

  /// @brief Maximum expected RMS energy value.
  float rms_max = 1.0f;

  /// @brief Maximum expected spectral centroid value (Hz).
  float centroid_max = 11025.0f;
};

/// @brief Frame buffer with 8-bit quantized data.
/// @details Reduces bandwidth for postMessage transfer.
/// Mel values are quantized from dB scale, chroma from 0-1.
struct QuantizedFrameBufferU8 {
  size_t n_frames = 0;              ///< Number of frames in buffer
  int n_mels = 0;                   ///< Number of mel bands per frame

  std::vector<float> timestamps;    ///< [n_frames] (kept as float for precision)
  std::vector<uint8_t> mel;         ///< [n_frames * n_mels] quantized mel (dB scaled)
  std::vector<uint8_t> chroma;      ///< [n_frames * 12] quantized chroma (0-255)
  std::vector<uint8_t> onset_strength; ///< [n_frames] quantized onset
  std::vector<uint8_t> rms_energy;  ///< [n_frames] quantized RMS
  std::vector<uint8_t> spectral_centroid; ///< [n_frames] quantized centroid
  std::vector<uint8_t> spectral_flatness; ///< [n_frames] quantized flatness

  /// @brief Clears all data.
  void clear() {
    n_frames = 0;
    n_mels = 0;
    timestamps.clear();
    mel.clear();
    chroma.clear();
    onset_strength.clear();
    rms_energy.clear();
    spectral_centroid.clear();
    spectral_flatness.clear();
  }

  /// @brief Reserves capacity for n frames.
  void reserve(size_t n, int mels) {
    n_mels = mels;
    timestamps.reserve(n);
    mel.reserve(n * mels);
    chroma.reserve(n * 12);
    onset_strength.reserve(n);
    rms_energy.reserve(n);
    spectral_centroid.reserve(n);
    spectral_flatness.reserve(n);
  }
};

/// @brief Frame buffer with 16-bit quantized data.
/// @details Higher precision than U8, still reduces bandwidth vs Float32.
struct QuantizedFrameBufferI16 {
  size_t n_frames = 0;              ///< Number of frames in buffer
  int n_mels = 0;                   ///< Number of mel bands per frame

  std::vector<float> timestamps;    ///< [n_frames] (kept as float for precision)
  std::vector<int16_t> mel;         ///< [n_frames * n_mels] quantized mel
  std::vector<int16_t> chroma;      ///< [n_frames * 12] quantized chroma
  std::vector<int16_t> onset_strength; ///< [n_frames] quantized onset
  std::vector<int16_t> rms_energy;  ///< [n_frames] quantized RMS
  std::vector<int16_t> spectral_centroid; ///< [n_frames] quantized centroid
  std::vector<int16_t> spectral_flatness; ///< [n_frames] quantized flatness

  /// @brief Clears all data.
  void clear() {
    n_frames = 0;
    n_mels = 0;
    timestamps.clear();
    mel.clear();
    chroma.clear();
    onset_strength.clear();
    rms_energy.clear();
    spectral_centroid.clear();
    spectral_flatness.clear();
  }

  /// @brief Reserves capacity for n frames.
  void reserve(size_t n, int mels) {
    n_mels = mels;
    timestamps.reserve(n);
    mel.reserve(n * mels);
    chroma.reserve(n * 12);
    onset_strength.reserve(n);
    rms_energy.reserve(n);
    spectral_centroid.reserve(n);
    spectral_flatness.reserve(n);
  }
};

}  // namespace sonare
