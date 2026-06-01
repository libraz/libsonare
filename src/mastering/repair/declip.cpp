#include "mastering/repair/declip.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/constants.h"
#include "util/lpc.h"

namespace sonare::mastering::repair {
namespace {

using sonare::constants::kEpsilon;

float cubic_hermite(float y0, float y1, float y2, float y3, float t) {
  const float m1 = 0.5f * (y2 - y0);
  const float m2 = 0.5f * (y3 - y1);
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * y1 + (t3 - 2.0f * t2 + t) * m1 +
         (-2.0f * t3 + 3.0f * t2) * y2 + (t3 - t2) * m2;
}

bool has_cubic_context(size_t start, size_t end, size_t size) {
  return start >= 2 && end + 1 < size;
}

bool can_use_lpc(size_t size, int order) {
  return order > 0 && size > static_cast<size_t>(order + 2);
}

float interpolate_fallback(const std::vector<float>& samples, size_t start, size_t end, size_t j) {
  const float left = start > 0 ? samples[start - 1] : (end < samples.size() ? samples[end] : 0.0f);
  const float right = end < samples.size() ? samples[end] : left;
  const size_t length = end - start;
  const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
  return has_cubic_context(start, end, samples.size())
             ? cubic_hermite(samples[start - 2], left, right, samples[end + 1], t)
             : left + (right - left) * t;
}

/// @brief Janssen (1986) constrained AR interpolation for a single clipped segment.
///
/// Reference: A. J. E. M. Janssen, "Adaptive interpolation of discrete-time signals
/// that can be modeled as autoregressive processes," IEEE Trans. ASSP, vol. 34, no. 2,
/// pp. 317–330, Apr. 1986. DOI: 10.1109/TASSP.1986.1164824
///
/// Algorithm outline (cf. Janssen 1986, Section III):
///   1. Initialise unknown samples x_u with cubic / linear interpolation.
///   2. Alternate for `iterations` outer rounds:
///      a. Estimate AR(p) coefficients a from the full signal (known + current x_u)
///         using Burg's method, which gives a minimum-phase, stable model.
///      b. Build the (N-p) x N prediction-error filter matrix A whose rows are
///         AR filter convolutions: (A x)[n] = x[n] + a[1]*x[n-1] + ... + a[p]*x[n-p].
///      c. Partition A into columns for unknowns (A_u) and knowns (A_k).
///      d. Solve the normal equations in x_u:
///           (A_u^T A_u + lambda I) x_u = -A_u^T A_k y_k
///         using Eigen's LDLT decomposition.  The Tikhonov term lambda prevents
///         singularity when the gap is wider than the AR model's effective rank.
///
/// The outer loop re-estimates AR coefficients from progressively better x_u
/// estimates, converging in 2–5 iterations for typical music material.
///
/// Note: the original Janssen formulation imposes a clipping-consistency
/// constraint (|x_u[i]| >= clip_threshold) for the case where the input is
/// known to be a hard-clipped signal. We omit that constraint here so that
/// callers can use declip as a general gap-filling tool: a small spike that
/// happens to exceed the threshold but was not a real clipping event should
/// be smoothly interpolated, not anchored at the threshold.
void reconstruct_region_janssen(std::vector<float>& samples, size_t start, size_t end,
                                const DeclipConfig& config) {
  // Step 1 – cubic / linear initialisation of the unknown region.
  for (size_t j = start; j < end; ++j) {
    samples[j] = interpolate_fallback(samples, start, end, j);
  }

  const size_t n = samples.size();
  const int max_order = std::min(config.lpc_order, static_cast<int>(std::max<size_t>(1, n / 4)));
  // Skip LPC refinement when there isn't enough context or model order for it to
  // outperform the cubic / linear baseline. Short signals (n < 32) or very low
  // AR orders (< 4) tend to push the interpolated values toward the global mean
  // and undo the smooth shape produced by the cubic Hermite step.
  constexpr size_t kMinLpcSignalLength = 32;
  constexpr int kMinLpcOrder = 4;
  if (n < kMinLpcSignalLength || max_order < kMinLpcOrder) return;
  if (!can_use_lpc(n, max_order)) return;

  // Indices of unknown (clipped) samples in the full signal.
  const size_t gap = end - start;
  std::vector<int> unknown_idx;
  unknown_idx.reserve(gap);
  for (size_t j = start; j < end; ++j) unknown_idx.push_back(static_cast<int>(j));

  const int nu = static_cast<int>(unknown_idx.size());
  const int nrows = static_cast<int>(n) - max_order;  // number of filter-output rows
  if (nrows <= 0) return;

  const int iters = std::max(config.iterations, 1);

  for (int outer = 0; outer < iters; ++outer) {
    // Step 2a – AR estimation from the full signal (known + current estimate).
    const auto model = sonare::lpc_burg(samples.data(), n, max_order);
    const int p = max_order;

    // Step 2b – build A_u and A_k vectors for the normal equations.
    // A is (nrows x n) Toeplitz.  Row r (r = 0..nrows-1) acts at sample r+p:
    //   (A x)[r] = x[r+p] + a[1]*x[r+p-1] + ... + a[p]*x[r]
    // We only need A_u (nrows x nu) and the product A_k * y_k.

    // Build set for O(1) membership test.
    std::vector<bool> is_unknown(n, false);
    for (int idx : unknown_idx) is_unknown[static_cast<size_t>(idx)] = true;

    // A_u: each column j corresponds to unknown_idx[j].
    Eigen::MatrixXf Au(nrows, nu);
    Au.setZero();

    // rhs = -A_k y_k, initialised as -A * samples (all columns),
    // then we subtract A_u * x_u to isolate A_k * y_k.
    Eigen::VectorXf rhs(nrows);
    rhs.setZero();

    for (int r = 0; r < nrows; ++r) {
      // Compute (A * samples)[r] = sum_k a[k] * samples[r+p-k] for k=0..p
      double Ax_r = 0.0;
      for (int k = 0; k <= p; ++k) {
        Ax_r += static_cast<double>(model.ar[static_cast<size_t>(k)]) *
                samples[static_cast<size_t>(r + p - k)];
      }
      rhs[r] = -static_cast<float>(Ax_r);  // start with -A*samples

      // Now fill A_u columns for this row and adjust rhs to isolate A_k*y_k.
      for (int j = 0; j < nu; ++j) {
        const int col = unknown_idx[static_cast<size_t>(j)];
        // A[r, col] is the coefficient for position col in row r.
        // Row r acts at time r+p; A[r, col] = a[r+p - col] if 0 <= r+p-col <= p, else 0.
        const int lag = (r + p) - col;
        if (lag >= 0 && lag <= p) {
          const float a_lag = model.ar[static_cast<size_t>(lag)];
          Au(r, j) = a_lag;
          // rhs = -A_k*y_k = -(A*x)[r] + A_u*x_u → we need to add back A_u[r,j]*x_u[j]
          rhs[r] += a_lag * samples[static_cast<size_t>(col)];
        }
      }
    }
    // rhs now holds -A_k * y_k (correctly isolating known-column contributions).

    // Step 2d – solve (A_u^T A_u + lambda I) x_u = A_u^T rhs
    // Tikhonov regularization lambda guards rank-deficient systems (gap > model rank).
    const float lambda = static_cast<float>(nrows) * kEpsilon;
    const Eigen::MatrixXf AtA = Au.transpose() * Au + lambda * Eigen::MatrixXf::Identity(nu, nu);
    const Eigen::VectorXf Atrhs = Au.transpose() * rhs;

    Eigen::LDLT<Eigen::MatrixXf> solver(AtA);
    if (solver.info() != Eigen::Success) break;  // numerical failure: keep current estimate

    const Eigen::VectorXf x_u = solver.solve(Atrhs);
    if (!x_u.allFinite()) break;

    // Step 2e – write the LDLT estimate back into the unknown positions.
    // No clipping constraint is applied: see the function-level note for why
    // we treat declip as a general gap-filler rather than a strict
    // clipping-consistency reconstructor.
    for (int j = 0; j < nu; ++j) {
      samples[static_cast<size_t>(unknown_idx[static_cast<size_t>(j)])] = x_u[j];
    }
  }
}

}  // namespace

Audio declip(const Audio& audio, const DeclipConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.clip_threshold > 0.0f && config.clip_threshold <= 1.0f) || config.lpc_order < 0 ||
      config.iterations < 0) {
    throw std::invalid_argument("invalid declip threshold");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  size_t i = 0;
  while (i < samples.size()) {
    if (std::abs(samples[i]) < config.clip_threshold) {
      ++i;
      continue;
    }

    const size_t start = i;
    while (i < samples.size() && std::abs(samples[i]) >= config.clip_threshold) ++i;
    const size_t end = i;
    reconstruct_region_janssen(samples, start, end, config);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
