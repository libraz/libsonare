#pragma once

/// @file decompose.h
/// @brief NMF decomposition and neighbour-based denoising (librosa.decompose).

#include <cstddef>
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
/// @throw sonare::SonareException (InvalidParameter) on a null @p S, a
///        non-positive dimension, an unsupported solver, a non-finite @p beta,
///        a negative @p n_iter, or any non-finite element of @p S. The
///        finiteness precondition lives here rather than in each binding so
///        every surface reports it identically.
DecomposeResult decompose(const float* S, int n_features, int n_frames, int n_components,
                          int n_iter = 100, const std::string& solver = "mu", float beta = 2.0f,
                          const std::string& init = "random");

/// @brief Options for @ref decompose_stems.
struct DecomposeStemsConfig {
  int n_components = 4;         ///< Number of NMF components (k).
  int n_fft = 2048;             ///< STFT size.
  int hop_length = 512;         ///< STFT hop.
  int n_iter = 100;             ///< NMF multiplicative-update iterations.
  float beta = 2.0f;            ///< Beta divergence: 2 = Frobenius, 1 = KL, 0 = IS.
  std::string init = "random";  ///< NMF initialisation ("random" or "nndsvd").
  /// Soft-mask exponent. 1 keeps the magnitude ratio; 2 is the Wiener-style
  /// power ratio, which separates more aggressively at the cost of more
  /// artefacts on overlapping partials. Values below 1 are rejected.
  float mask_power = 1.0f;
};

/// @brief Validation rules for @ref DecomposeStemsConfig.
/// @details Found by argument-dependent lookup from @ref Validated, which
///          @ref decompose_stems applies before any spectrogram work. Bindings
///          that build the config directly inherit the rules from that single
///          entry point.
/// @throws SonareException(InvalidParameter) for a non-positive size or count,
///         a non-finite beta, or a mask power below 1.
void validate_config(const DecomposeStemsConfig& config);

/// @brief Output of @ref decompose_stems.
struct DecomposeStemsResult {
  /// One time-domain signal per component, each the length of the input.
  std::vector<std::vector<float>> components;
  std::vector<float> W;  ///< Component matrix [n_bins x n_components] row-major.
  std::vector<float> H;  ///< Activation matrix [n_components x n_frames] row-major.
};

/// @brief NMF separation that CARRIES the original phase, so each component is
///        directly listenable.
/// @details @ref decompose returns the W / H factors of a magnitude
///          spectrogram, which have no phase; reconstructing from them needs a
///          phase estimator (Griffin-Lim), and an estimated phase does not hold
///          up as a stem. This instead builds a soft mask per component from
///          the factorisation and applies it to the ORIGINAL complex
///          spectrogram, so every component keeps the source's phase:
///
///              mask_k = (W[:,k] H[k,:])^p / sum_j (W[:,j] H[j,:])^p
///
///          The masks sum to one wherever the model has any energy, and the
///          inverse STFT is linear, so the component signals sum back to the
///          input (up to STFT edge effects). Bins where the model has no energy
///          at all are dropped from every component rather than duplicated.
/// @param samples Input signal (mono).
/// @param n Number of samples.
/// @param sample_rate Sample rate in Hz.
/// @param config Component count, STFT geometry, NMF and mask settings.
/// @return One signal per component plus the factorisation that produced them.
/// @throw sonare::SonareException on an empty input or an invalid configuration.
DecomposeStemsResult decompose_stems(const float* samples, std::size_t n, int sample_rate,
                                     const DecomposeStemsConfig& config = DecomposeStemsConfig());

/// @brief Nearest-neighbour filter for spectrogram denoising.
/// @details Mirrors `librosa.decompose.nn_filter`. For each frame, the k
/// nearest neighbour frames (by cosine similarity, with frames within `width`
/// time excluded) are aggregated. Supported aggregators: "mean", "median",
/// "min", "max". @p width must be >= 0 (negative values are rejected, mirroring
/// librosa, rather than silently disabling the time-exclusion band).
/// @return Smoothed spectrogram [n_features x n_frames] row-major.
/// @throw sonare::SonareException (InvalidParameter) on a null @p S, an
///        unsupported aggregator, a negative @p width, or any non-finite
///        element of @p S. Non-finite input is rejected rather than filtered
///        because a NaN vanishes in the aggregation and leaves an entirely
///        finite but silently altered result.
std::vector<float> nn_filter(const float* S, int n_features, int n_frames,
                             const std::string& aggregate = "mean", int k = 5, int width = 1);

}  // namespace sonare
