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
/// NMF). Supports the full beta-divergence family used by `sklearn.NMF`
/// (Frobenius, KL, IS). Input and outputs are non-negative.
/// @param S Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows).
/// @param n_frames Number of time frames.
/// @param n_components Target number of components (k).
/// @param n_iter Number of multiplicative update iterations. @warning Passing
///        n_iter==0 is accepted by the core but returns the raw init matrices
///        (a degenerate, meaningless factorisation); the C ABI rejects it.
/// @param solver Algorithm name (only "mu" is supported currently).
/// @param beta Beta value for the divergence: 2 = Frobenius (default), 1 =
///        Kullback-Leibler, 0 = Itakura-Saito. Fractional values are accepted
///        and use the generalized multiplicative-update formula
///        (Fevotte-Idier 2011).
/// @param init Initialisation: "random" (default, deterministic seed) or
///        "nndsvd" (SVD-based, deterministic). "nndsvd" tends to converge in
///        fewer iterations but costs an SVD up-front.
DecomposeResult decompose(const float* S, int n_features, int n_frames, int n_components,
                          int n_iter = 100, const std::string& solver = "mu", float beta = 2.0f,
                          const std::string& init = "random");

/// @brief Nearest-neighbour filter for spectrogram denoising.
/// @details Mirrors `librosa.decompose.nn_filter`. For each frame, the k
/// nearest neighbour frames (by cosine similarity, with frames within `width`
/// time excluded) are aggregated. Supported aggregators: "mean", "median",
/// "min", "max". @p width must be >= 0 (negative values are rejected, mirroring
/// librosa, rather than silently disabling the time-exclusion band).
/// @return Smoothed spectrogram [n_features x n_frames] row-major.
std::vector<float> nn_filter(const float* S, int n_features, int n_frames,
                             const std::string& aggregate = "mean", int k = 5, int width = 1);

}  // namespace sonare
