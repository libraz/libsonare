#pragma once

/// @file nnls_chroma.h
/// @brief NNLS chroma feature extraction for chord recognition.

#include <vector>

#include "core/audio.h"
#include "feature/chroma.h"
#include "feature/cqt.h"

namespace sonare {

/// @brief Configuration for NNLS chroma extraction.
struct NnlsChromaConfig {
  CqtConfig cqt = [] {
    CqtConfig config;
    config.bins_per_octave = 36;  // Three bins per semitone.
    config.n_bins = 252;          // Seven octaves.
    return config;
  }();
  int midi_min = 21;    ///< A0, used for harmonic template pitch indexing
  int n_pitches = 88;   ///< Piano range A0-C8
  int n_harmonics = 6;  ///< Harmonics used in the spectral template
  /// Per frame, z-score each CQT bin against the bins within half an octave of it, so the
  /// harmonic peaks stand above the local spectral envelope; a held note keeps its level.
  bool whiten = true;
  int max_iter = 100;            ///< NNLS iterations per frame
  float tolerance = 1e-4f;       ///< NNLS KKT tolerance
  bool normalize_frames = true;  ///< L-infinity normalize chroma frames
  /// Recording tuning offset in fractions of a semitone (estimate_tuning() unit); shifts the
  /// CQT grid, the harmonic templates and the STFT blend together. 0 is concert A440.
  float tuning = 0.0f;

  /// @brief Blend a plain STFT chroma into the NNLS result for robustness on
  /// sparse/pure-tone content where CQT salience can be ambiguous.
  bool enable_stft_blend = true;    ///< Disable to use NNLS/CQT salience only.
  float stft_blend_weight = 0.55f;  ///< Weight applied to the STFT chroma in the blend.
  int stft_blend_n_fft = 4096;      ///< FFT size used for the STFT chroma fallback.

  /// @brief The reduced analysis profile shared by every public NNLS chroma entry point: one
  /// semitone per CQT bin over seven octaves, four harmonics, 25 iterations.
  static NnlsChromaConfig fast(int hop_length) {
    NnlsChromaConfig config;
    config.cqt.bins_per_octave = 12;
    config.cqt.n_bins = 84;
    config.cqt.hop_length = hop_length;
    config.midi_min = 24;
    config.n_pitches = 60;
    config.n_harmonics = 4;
    config.max_iter = 25;
    config.tolerance = 1.0e-3f;
    return config;
  }
};

/// @brief Builds a log-frequency harmonic template matrix for NNLS.
/// @return Matrix [n_cqt_bins x n_pitches] row-major
std::vector<float> build_nnls_harmonic_template(const std::vector<float>& cqt_frequencies,
                                                const NnlsChromaConfig& config);

/// @brief Computes NNLS chroma from audio.
Chroma nnls_chroma(const Audio& audio, const NnlsChromaConfig& config = NnlsChromaConfig());

}  // namespace sonare
