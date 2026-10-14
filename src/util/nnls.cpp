#include "util/nnls.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

namespace {

double lipschitz_upper_bound(const Eigen::MatrixXd& matrix) {
  double bound = 0.0;
  for (Eigen::Index row = 0; row < matrix.rows(); ++row) {
    double row_sum = 0.0;
    for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
      row_sum += std::abs(matrix(row, column));
    }
    bound = std::max(bound, row_sum);
  }
  return bound;
}

}  // namespace

std::vector<float> nnls(const float* A, int A_rows, int A_cols, const float* B, int B_cols,
                        int max_iter, float tol) {
  if (A == nullptr || B == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "nnls: A and B must be non-null");
  }
  if (A_rows <= 0 || A_cols <= 0 || B_cols <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "nnls: dimensions must be positive");
  }
  if (tol < 0.0f)
    throw SonareException(ErrorCode::InvalidParameter, "nnls: tol must be non-negative");

  // Build A once and solve all B columns together with projected FISTA. This
  // reuses AtA/AtB and turns the former per-column active-set factorisations
  // into cache-friendly matrix multiplies.
  // Flatten indices are promoted to size_t so a large dictionary (A_rows *
  // A_cols, A_rows * B_cols, A_cols * B_cols) cannot overflow int (UB) when the
  // row-major offset is formed.
  Eigen::MatrixXd Ad(A_rows, A_cols);
  for (int i = 0; i < A_rows; ++i) {
    for (int j = 0; j < A_cols; ++j) {
      Ad(i, j) = static_cast<double>(A[static_cast<size_t>(i) * A_cols + j]);
    }
  }
  const Eigen::MatrixXd AtA = Ad.transpose() * Ad;
  Eigen::MatrixXd Bd(A_rows, B_cols);
  for (int row = 0; row < A_rows; ++row) {
    for (int column = 0; column < B_cols; ++column) {
      Bd(row, column) = static_cast<double>(B[static_cast<size_t>(row) * B_cols + column]);
    }
  }
  const Eigen::MatrixXd AtB = Ad.transpose() * Bd;
  Eigen::MatrixXd x = Eigen::MatrixXd::Zero(A_cols, B_cols);
  Eigen::MatrixXd y = x;
  double momentum = 1.0;
  const double lipschitz = lipschitz_upper_bound(AtA);
  if (lipschitz > 0.0) {
    const double step = 1.0 / lipschitz;
    for (int iteration = 0; iteration < max_iter; ++iteration) {
      const Eigen::MatrixXd next = (y - step * (AtA * y - AtB)).cwiseMax(0.0);
      const double denominator = std::max(1.0, x.norm());
      // Do not accept the naturally small first FISTA updates as convergence:
      // use a stricter iterate tolerance and require a short warm-up. This
      // preserves the public solver's historical 1e-4 solution accuracy while
      // retaining the batched matrix path.
      if (iteration >= 15 && (next - x).norm() <= static_cast<double>(tol) * 0.1 * denominator) {
        x = next;
        break;
      }
      const double next_momentum = 0.5 * (1.0 + std::sqrt(1.0 + 4.0 * momentum * momentum));
      y = next + ((momentum - 1.0) / next_momentum) * (next - x);
      x = next;
      momentum = next_momentum;
    }
  }
  std::vector<float> X(static_cast<size_t>(A_cols) * B_cols, 0.0f);
  for (int row = 0; row < A_cols; ++row) {
    for (int column = 0; column < B_cols; ++column) {
      X[static_cast<size_t>(row) * B_cols + column] = static_cast<float>(x(row, column));
    }
  }
  return X;
}

std::vector<float> nnls(const std::vector<float>& A, int A_rows, int A_cols,
                        const std::vector<float>& B, int B_cols, int max_iter, float tol) {
  if (A_rows > 0 && A_cols > 0 && B_cols > 0) {
    std::size_t expected_a = 0;
    std::size_t expected_b = 0;
    if (!numeric::checked_matrix_element_count(A_rows, A_cols, &expected_a) ||
        !numeric::checked_matrix_element_count(A_rows, B_cols, &expected_b) ||
        A.size() != expected_a || B.size() != expected_b) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "nnls: A and B sizes must match their matrix dimensions");
    }
  }
  return nnls(A.data(), A_rows, A_cols, B.data(), B_cols, max_iter, tol);
}

}  // namespace sonare
