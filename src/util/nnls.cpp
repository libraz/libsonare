#include "util/nnls.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>

#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Lawson-Hanson NNLS using pre-computed normal-equation matrices.
/// @details Solves `min_x ||A x - b||^2 s.t. x >= 0` using only `AtA` and `Atb`.
/// Each inner subproblem reduces to a small LDLT solve on the active set
/// (`AtA(P, P) s = Atb(P)`), which avoids the per-iteration SVD of the
/// original implementation and is independent of the number of rows of A.
Eigen::VectorXd nnls_with_normal(const Eigen::MatrixXd& AtA, const Eigen::VectorXd& Atb,
                                 int max_iter, double tol) {
  const int n = static_cast<int>(AtA.cols());
  Eigen::VectorXd x = Eigen::VectorXd::Zero(n);
  std::vector<int> P;
  std::vector<char> in_P(static_cast<size_t>(n), 0);

  // Workspace reused per inner iteration.
  Eigen::MatrixXd AtA_P;
  Eigen::VectorXd Atb_P;
  Eigen::VectorXd s;

  for (int outer = 0; outer < max_iter; ++outer) {
    // Gradient (dual variable): w = Atb - AtA * x.
    Eigen::VectorXd w = Atb - AtA * x;
    int j_best = -1;
    double w_best = tol;
    for (int j = 0; j < n; ++j) {
      if (in_P[j]) continue;
      if (w[j] > w_best) {
        w_best = w[j];
        j_best = j;
      }
    }
    if (j_best < 0) break;
    P.push_back(j_best);
    in_P[j_best] = 1;

    for (int inner = 0; inner < max_iter; ++inner) {
      const int k = static_cast<int>(P.size());
      AtA_P.resize(k, k);
      Atb_P.resize(k);
      for (int a = 0; a < k; ++a) {
        Atb_P[a] = Atb[P[a]];
        for (int b = 0; b < k; ++b) {
          AtA_P(a, b) = AtA(P[a], P[b]);
        }
      }
      // LDLT handles semi-definite cases; for singular sub-systems we still get
      // a least-squares-ish step that the active-set logic will clean up.
      s = AtA_P.ldlt().solve(Atb_P);

      double min_s = std::numeric_limits<double>::infinity();
      for (int a = 0; a < k; ++a) min_s = std::min(min_s, s[a]);
      if (min_s > 0.0) {
        for (int a = 0; a < k; ++a) x[P[a]] = s[a];
        break;
      }

      // Step toward `s`, stopping at the first variable that would go negative.
      double alpha = std::numeric_limits<double>::infinity();
      for (int a = 0; a < k; ++a) {
        const int idx = P[a];
        const double sa = s[a];
        if (sa <= 0.0) {
          const double diff = x[idx] - sa;
          if (diff > 1e-15) alpha = std::min(alpha, x[idx] / diff);
        }
      }
      if (!std::isfinite(alpha) || alpha <= 0.0) {
        // Numerical breakdown; drop the most recently added index and bail.
        in_P[P.back()] = 0;
        P.pop_back();
        break;
      }
      for (int a = 0; a < k; ++a) {
        const int idx = P[a];
        x[idx] += alpha * (s[a] - x[idx]);
      }
      // Remove indices that fell to (near) zero.
      std::vector<int> new_P;
      new_P.reserve(P.size());
      for (int idx : P) {
        if (x[idx] > 1e-12) {
          new_P.push_back(idx);
        } else {
          x[idx] = 0.0;
          in_P[idx] = 0;
        }
      }
      P.swap(new_P);
    }
  }
  return x;
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

  // Build A once and pre-compute the normal-equation matrices. AtA is shared
  // across all columns of B.
  Eigen::MatrixXd Ad(A_rows, A_cols);
  for (int i = 0; i < A_rows; ++i) {
    for (int j = 0; j < A_cols; ++j) {
      Ad(i, j) = static_cast<double>(A[i * A_cols + j]);
    }
  }
  const Eigen::MatrixXd AtA = Ad.transpose() * Ad;

  std::vector<float> X(static_cast<size_t>(A_cols) * B_cols, 0.0f);
  Eigen::VectorXd b(A_rows);
  for (int c = 0; c < B_cols; ++c) {
    for (int i = 0; i < A_rows; ++i) {
      b[i] = static_cast<double>(B[i * B_cols + c]);
    }
    Eigen::VectorXd Atb = Ad.transpose() * b;
    Eigen::VectorXd x = nnls_with_normal(AtA, Atb, max_iter, static_cast<double>(tol));
    for (int j = 0; j < A_cols; ++j) {
      X[j * B_cols + c] = static_cast<float>(x[j]);
    }
  }
  return X;
}

std::vector<float> nnls(const std::vector<float>& A, int A_rows, int A_cols,
                        const std::vector<float>& B, int B_cols, int max_iter, float tol) {
  return nnls(A.data(), A_rows, A_cols, B.data(), B_cols, max_iter, tol);
}

}  // namespace sonare
