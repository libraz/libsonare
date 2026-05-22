#pragma once

/// @file nnls.h
/// @brief Non-negative least squares solver (Lawson-Hanson).

#include <vector>

namespace sonare {

/// @brief Solves A * X = B subject to X >= 0 column-by-column.
/// @details Implements the Lawson-Hanson active-set algorithm (the same
/// underlying routine as `scipy.optimize.nnls`, which is what
/// `librosa.util.nnls` wraps).
/// @param A Coefficient matrix [A_rows x A_cols] row-major.
/// @param A_rows Number of rows in A.
/// @param A_cols Number of columns in A.
/// @param B Right-hand side matrix [A_rows x B_cols] row-major.
/// @param B_cols Number of columns in B.
/// @param max_iter Maximum iterations per column.
/// @param tol Convergence tolerance on the dual variables (KKT residual).
/// @return X [A_cols x B_cols] row-major, with X >= 0.
std::vector<float> nnls(const float* A, int A_rows, int A_cols, const float* B, int B_cols,
                        int max_iter = 100, float tol = 1e-4f);

/// @brief Convenience overload taking std::vector inputs.
std::vector<float> nnls(const std::vector<float>& A, int A_rows, int A_cols,
                        const std::vector<float>& B, int B_cols, int max_iter = 100,
                        float tol = 1e-4f);

}  // namespace sonare
