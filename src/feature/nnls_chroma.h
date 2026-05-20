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
  int midi_min = 21;             ///< A0, used for harmonic template pitch indexing
  int n_pitches = 88;            ///< Piano range A0-C8
  int n_harmonics = 6;           ///< Harmonics used in the spectral template
  bool whiten = true;            ///< Per-frequency running normalization
  int whitening_window = 15;     ///< Frames for running mean/std normalization
  int max_iter = 100;            ///< NNLS iterations per frame
  float tolerance = 1e-4f;       ///< NNLS KKT tolerance
  bool normalize_frames = true;  ///< L-infinity normalize chroma frames
};

/// @brief Builds a log-frequency harmonic template matrix for NNLS.
/// @return Matrix [n_cqt_bins x n_pitches] row-major
std::vector<float> build_nnls_harmonic_template(const std::vector<float>& cqt_frequencies,
                                                const NnlsChromaConfig& config);

/// @brief Computes NNLS chroma from audio.
Chroma nnls_chroma(const Audio& audio, const NnlsChromaConfig& config = NnlsChromaConfig());

}  // namespace sonare
