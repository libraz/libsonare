#pragma once

/// @file decompose.h
/// @brief NMF decomposition and neighbour-based denoising (librosa.decompose).

#include <string>
#include <vector>

namespace sonare {

/// @brief Output of @ref decompose: NMF components.
struct DecomposeResult {
  std::vector<float> W;  ///< Component matrix [n_features x n_components] row-major.
  std::vector<float> H;  ///< Activation matrix [n_components x n_frames] row-major.
};

/// @brief Non-negative matrix factorisation of a non-negative spectrogram.
/// @details Implements `librosa.decompose.decompose` (multiplicative-update
/// NMF, Lee-Seung). The input matrix and outputs are non-negative.
/// @param S Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows).
/// @param n_frames Number of time frames.
/// @param n_components Target number of components (k).
/// @param n_iter Number of multiplicative update iterations.
/// @param solver Algorithm name (currently "mu" is supported).
DecomposeResult decompose(const float* S, int n_features, int n_frames, int n_components,
                          int n_iter = 100, const std::string& solver = "mu");

/// @brief Nearest-neighbour filter for spectrogram denoising.
/// @details Mirrors `librosa.decompose.nn_filter` with `aggregate="mean"` /
/// `aggregate="median"`. For each frame, the k nearest neighbour frames (by
/// cosine similarity, with frames within `width` time excluded) are aggregated.
/// @return Smoothed spectrogram [n_features x n_frames] row-major.
std::vector<float> nn_filter(const float* S, int n_features, int n_frames,
                             const std::string& aggregate = "mean", int k = 5, int width = 1);

}  // namespace sonare
