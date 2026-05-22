#pragma once

/// @file segment.h
/// @brief Low-level segmentation primitives matching librosa.segment.

#include <string>
#include <vector>

namespace sonare {

/// @brief Cross-similarity matrix between two feature matrices.
/// @details Mirrors `librosa.segment.cross_similarity` with `mode="affinity"`.
/// Computes pairwise distances between columns of `X` and `Y`, then either
/// produces a sparse k-nearest-neighbour adjacency (when `k > 0`) or a dense
/// affinity matrix.
/// @param X Features [X_rows x X_cols] row-major (X_cols samples, X_rows feature dim).
/// @param X_rows Feature dimension.
/// @param X_cols Number of samples in X.
/// @param Y Features [Y_rows x Y_cols] row-major.
/// @param Y_rows Must equal `X_rows`.
/// @param Y_cols Number of samples in Y.
/// @param k When > 0, keep the k nearest neighbours per X-column; otherwise
///          return the dense affinity matrix.
/// @param metric "cosine" or "euclidean".
/// @return Row-major matrix [X_cols x Y_cols] of similarity (cosine) or
///         negative-distance (euclidean) values.
std::vector<float> cross_similarity(const float* X, int X_rows, int X_cols, const float* Y,
                                    int Y_rows, int Y_cols, int k = 0,
                                    const std::string& metric = "cosine");

/// @brief Self-similarity (recurrence) matrix.
/// @details Wraps @ref cross_similarity with `X == Y`. When `sym=true` the
/// returned matrix is symmetrised. `width` excludes the |i - j| < width band
/// from neighbours (matches librosa).
std::vector<float> recurrence_matrix(const float* data, int rows, int cols, int k = 0,
                                     int width = 1, bool sym = false,
                                     const std::string& metric = "euclidean");

/// @brief Converts a recurrence matrix to a lag matrix.
/// @details Row i, lag j corresponds to recurrence[i, i + j] (modulo n).
/// @param rec Recurrence matrix [n x n] row-major.
/// @param n Matrix dimension.
/// @param pad If true, pad with zeros so lag spans [-(n-1), n-1].
std::vector<float> recurrence_to_lag(const float* rec, int n, bool pad = false);

/// @brief Inverse of @ref recurrence_to_lag.
std::vector<float> lag_to_recurrence(const float* lag, int n_rows, int n_lags);

/// @brief Subdivides each segment between consecutive boundary frames into
///        @p n_segments smaller chunks via clustering of column-feature norms.
/// @return Sorted vector of refined boundary indices.
std::vector<int> subsegment(const float* data, int rows, int cols,
                            const std::vector<int>& boundaries, int n_segments = 4);

/// @brief Agglomerative clustering of the columns of @p data into @p k groups.
/// @return A length-`cols` vector of cluster labels in [0, k).
std::vector<int> agglomerative(const float* data, int rows, int cols, int k);

/// @brief Enhances diagonals in a recurrence matrix via Gaussian smoothing
///        along the diagonal direction.
/// @return Row-major [n x n] enhanced matrix.
std::vector<float> path_enhance(const float* rec, int n, int win, int max_ratio = 2,
                                int min_ratio = 0, int n_filters = 7);

}  // namespace sonare
