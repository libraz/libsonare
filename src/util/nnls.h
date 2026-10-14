#pragma once

/// @file nnls.h
/// @brief Batched non-negative least squares solver (projected FISTA).

#include <vector>

namespace sonare {

/// @brief Solves A * X = B subject to X >= 0 column-by-column.
/// @details Solves all right-hand-side columns together, reusing AtA/AtB.
/// @param A Coefficient matrix [A_rows x A_cols] row-major.
/// @param A_rows Number of rows in A.
/// @param A_cols Number of columns in A.
/// @param B Right-hand side matrix [A_rows x B_cols] row-major.
/// @param B_cols Number of columns in B.
/// @param max_iter Maximum iterations per column.
/// @param tol Convergence tolerance for the L2 norm of the iterate change.
///        After the warm-up, iteration stops when that change is at most
///        0.1 * tol * max(1, ||x||); the threshold is absolute below unit
///        norm and scales with the current solution norm above it.
/// @return X [A_cols x B_cols] row-major, with X >= 0.
std::vector<float> nnls(const float* A, int A_rows, int A_cols, const float* B, int B_cols,
                        int max_iter = 100, float tol = 1e-4f);

/// @brief Convenience overload taking std::vector inputs.
std::vector<float> nnls(const std::vector<float>& A, int A_rows, int A_cols,
                        const std::vector<float>& B, int B_cols, int max_iter = 100,
                        float tol = 1e-4f);

}  // namespace sonare
