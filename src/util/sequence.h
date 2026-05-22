#pragma once

/// @file sequence.h
/// @brief Sequence-domain helpers (DTW, Viterbi, RQA) mirroring librosa.sequence.

#include <string>
#include <utility>
#include <vector>

namespace sonare {

/// @brief DTW result.
struct DtwResult {
  float distance = 0.0f;                  ///< Final alignment cost.
  std::vector<std::pair<int, int>> path;  ///< Warping path (X-index, Y-index) order.
  std::vector<float> accumulated_cost;    ///< Accumulated cost matrix [X_cols x Y_cols].
};

/// @brief Dynamic Time Warping between two feature sequences.
/// @param X Features [X_rows x X_cols] row-major.
/// @param Y Features [Y_rows x Y_cols] row-major (Y_rows must equal X_rows).
/// @param metric "euclidean", "cosine", "manhattan", or "chebyshev".
/// @param subseq If true, allow the warping path to start anywhere along Y
///        (subsequence DTW).
/// @param step_sizes_sigma Allowed warping steps as (di, dj) pairs. Each step
///        means the path may move from (i - di, j - dj) to (i, j). Defaults to
///        the symmetric P0 pattern {(1,1), (1,0), (0,1)}, matching librosa.
/// @param weights_add Per-step additive cost weights. Defaults to all 1.0.
DtwResult dtw(const float* X, int X_rows, int X_cols, const float* Y, int Y_rows, int Y_cols,
              const std::string& metric = "euclidean", bool subseq = false,
              const std::vector<std::pair<int, int>>& step_sizes_sigma = {},
              const std::vector<float>& weights_add = {});

/// @brief Recurrence quantification analysis.
struct RqaResult {
  float recurrence_rate = 0.0f;  ///< Fraction of recurring points.
  float determinism = 0.0f;      ///< Fraction in diagonal lines >= 2.
  float average_diagonal_length = 0.0f;
  int max_diagonal_length = 0;
};

/// @brief Computes RQA statistics from a binary recurrence matrix.
RqaResult rqa(const float* rec, int n);

/// @brief Viterbi decoding for a fully-observed HMM.
/// @param log_prob Emission log-probabilities [n_states x n_steps] row-major.
/// @param n_states Number of hidden states.
/// @param n_steps Number of observations.
/// @param transition Transition matrix [n_states x n_states] (probabilities).
/// @param p_init Initial state distribution (length n_states). Uniform if null.
std::vector<int> viterbi(const float* log_prob, int n_states, int n_steps, const float* transition,
                         const float* p_init = nullptr);

/// @brief Discriminative Viterbi (posteriors instead of likelihoods).
/// @details Converts posteriors to scaled emission likelihoods using the
/// supplied state prior `p_state`.
std::vector<int> viterbi_discriminative(const float* posteriors, int n_states, int n_steps,
                                        const float* transition, const float* p_state,
                                        const float* p_init = nullptr);

}  // namespace sonare
