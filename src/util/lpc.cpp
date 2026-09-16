#include "util/lpc.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {
namespace {

using sonare::constants::kEpsilon;

void validate_lpc_args(const float* x, size_t n, int order) {
  if (x == nullptr && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "input must not be null");
  }
  if (order < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "order must be non-negative");
  }
  if (order >= static_cast<int>(n) && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "order must be smaller than input length");
  }
}

void validate_gap_args(const float* samples, size_t n, size_t start, size_t end) {
  if (samples == nullptr && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "samples must not be null");
  }
  if (start > end || end > n) {
    throw SonareException(ErrorCode::InvalidParameter, "gap must lie inside the buffer");
  }
}

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

float interpolate_gap_sample(const float* samples, size_t n, size_t start, size_t end, size_t j) {
  const float left = start > 0 ? samples[start - 1] : (end < n ? samples[end] : 0.0f);
  const float right = end < n ? samples[end] : left;
  const size_t length = end - start;
  const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
  return has_cubic_context(start, end, n)
             ? cubic_hermite(samples[start - 2], left, right, samples[end + 1], t)
             : left + (right - left) * t;
}

}  // namespace

LpcResult lpc_burg(const float* x, size_t n, int order) {
  validate_lpc_args(x, n, order);
  LpcResult result;
  result.ar.assign(static_cast<size_t>(order + 1), 0.0f);
  result.ar[0] = 1.0f;
  if (n == 0 || order == 0) {
    double energy = 0.0;
    for (size_t i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
    result.variance = n == 0 ? 0.0f : static_cast<float>(energy / static_cast<double>(n));
    return result;
  }

  std::vector<double> y(x, x + n);
  std::vector<double> ar(static_cast<size_t>(order + 1), 0.0);
  std::vector<double> ar_prev(static_cast<size_t>(order + 1), 0.0);
  ar[0] = 1.0;
  ar_prev[0] = 1.0;

  std::vector<double> fwd(y.begin() + 1, y.end());
  std::vector<double> bwd(y.begin(), y.end() - 1);
  size_t fwd_start = 0;
  size_t active_len = fwd.size();

  double den = 0.0;
  for (size_t i = 0; i < active_len; ++i) {
    const double f = fwd[fwd_start + i];
    const double b = bwd[i];
    den += f * f + b * b;
  }

  constexpr double kTiny = 1.0e-300;
  for (int i = 0; i < order; ++i) {
    double dot = 0.0;
    for (size_t k = 0; k < active_len; ++k) {
      dot += bwd[k] * fwd[fwd_start + k];
    }
    const double reflection = -2.0 * dot / (den + kTiny);

    std::swap(ar, ar_prev);
    ar[0] = ar_prev[0];
    for (int j = 1; j <= i + 1; ++j) {
      ar[static_cast<size_t>(j)] =
          ar_prev[static_cast<size_t>(j)] + reflection * ar_prev[static_cast<size_t>(i - j + 1)];
    }

    for (size_t k = 0; k < active_len; ++k) {
      const double f = fwd[fwd_start + k];
      const double b = bwd[k];
      fwd[fwd_start + k] = f + reflection * b;
      bwd[k] = b + reflection * f;
    }

    if (active_len > 0) {
      const double q = 1.0 - reflection * reflection;
      const double bwd_back = bwd[active_len - 1];
      const double fwd_front = fwd[fwd_start];
      den = q * den - bwd_back * bwd_back - fwd_front * fwd_front;
    }

    if (active_len > 0) {
      ++fwd_start;
      --active_len;
    }
  }

  for (int i = 0; i <= order; ++i) {
    result.ar[static_cast<size_t>(i)] = static_cast<float>(ar[static_cast<size_t>(i)]);
  }
  result.variance = static_cast<float>(std::max(den, 0.0) / static_cast<double>(n));
  return result;
}

void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out) {
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  validate_lpc_args(x, n, order);
  LpcResult& result = *out;
  result.variance = 0.0f;
  result.ar.assign(static_cast<size_t>(order + 1), 0.0f);
  result.ar[0] = 1.0f;
  if (n == 0 || order == 0) {
    double energy = 0.0;
    for (size_t i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
    result.variance = n == 0 ? 0.0f : static_cast<float>(energy / static_cast<double>(n));
    return;
  }

  const std::vector<float> raw = unnormalized_autocorrelation(x, n, static_cast<size_t>(order + 1));
  std::vector<double> r(static_cast<size_t>(order + 1), 0.0);
  for (int lag = 0; lag <= order; ++lag) {
    r[static_cast<size_t>(lag)] =
        static_cast<double>(raw[static_cast<size_t>(lag)]) / static_cast<double>(n);
  }

  std::vector<double> a(static_cast<size_t>(order + 1), 0.0);
  a[0] = 1.0;
  double error = r[0];
  if (error <= 1.0e-20) {
    result.variance = 0.0f;
    return;
  }

  for (int i = 1; i <= order; ++i) {
    double acc = r[static_cast<size_t>(i)];
    for (int j = 1; j < i; ++j) {
      acc += a[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];
    }
    const double reflection = -acc / error;
    std::vector<double> next_a = a;
    for (int j = 1; j < i; ++j) {
      next_a[static_cast<size_t>(j)] =
          a[static_cast<size_t>(j)] + reflection * a[static_cast<size_t>(i - j)];
    }
    next_a[static_cast<size_t>(i)] = reflection;
    a = next_a;
    error *= 1.0 - reflection * reflection;
    if (error <= 1.0e-20) {
      error = 0.0;
      break;
    }
  }

  for (int i = 0; i <= order; ++i) {
    result.ar[static_cast<size_t>(i)] = static_cast<float>(a[static_cast<size_t>(i)]);
  }
  result.variance = static_cast<float>(error);
}

LpcResult lpc_autocorrelation(const float* x, size_t n, int order) {
  LpcResult result;
  lpc_autocorrelation(x, n, order, &result);
  return result;
}

std::vector<float> lpc_residual(const float* x, size_t n, const LpcResult& model) {
  if (x == nullptr && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "input must not be null");
  }
  if (model.ar.empty() || model.ar[0] == 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid LPC model");
  }
  const size_t order = model.ar.size() - 1;
  std::vector<float> residual(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    double e = x[i];
    const size_t max_k = std::min(order, i);
    for (size_t k = 1; k <= max_k; ++k) {
      e += static_cast<double>(model.ar[k]) * x[i - k];
    }
    residual[i] = static_cast<float>(e);
  }
  return residual;
}

void interpolate_gap(float* samples, size_t n, size_t start, size_t end) {
  validate_gap_args(samples, n, start, end);
  for (size_t j = start; j < end; ++j) {
    samples[j] = interpolate_gap_sample(samples, n, start, end, j);
  }
}

bool ar_interpolate_region(float* samples, size_t n, size_t start, size_t end,
                           const ArInterpolateParams& params) {
  validate_gap_args(samples, n, start, end);
  const size_t gap = end - start;
  if (gap > params.max_gap) return false;
  if (gap == 0) return true;

  const size_t requested_order = static_cast<size_t>(std::max(params.order, 0));
  // The order-derived term is caller-controlled and would otherwise pull the whole
  // input into the solver; cap it so the context is a function of the caps alone.
  const size_t context_radius =
      std::min({n, params.max_context_radius, std::max<size_t>(4 * requested_order, 8 * gap)});
  const size_t context_start = start > context_radius ? start - context_radius : 0;
  const size_t context_end = std::min(n, end + context_radius);
  std::vector<float> context(samples + context_start, samples + context_end);
  const size_t local_start = start - context_start;
  const size_t local_end = end - context_start;

  // Step 1 – cubic / linear initialisation of the unknown region. Keep a copy of
  // the interpolated baseline so the final LPC estimate can be blended against it
  // (params.blend); the fallback gets weight (1 - blend).
  std::vector<float> baseline(gap);
  for (size_t j = local_start; j < local_end; ++j) {
    context[j] = interpolate_gap_sample(context.data(), context.size(), local_start, local_end, j);
    baseline[j - local_start] = context[j];
  }
  std::copy(context.begin() + static_cast<std::ptrdiff_t>(local_start),
            context.begin() + static_cast<std::ptrdiff_t>(local_end), samples + start);

  const size_t context_size = context.size();
  const int max_order =
      std::min(params.order, static_cast<int>(std::max<size_t>(1, context_size / 4)));
  // Skip LPC refinement when there isn't enough context or model order for it to
  // outperform the cubic / linear baseline. Short signals (n < 32) or very low
  // AR orders (< 4) tend to push the interpolated values toward the global mean
  // and undo the smooth shape produced by the cubic Hermite step.
  constexpr size_t kMinLpcSignalLength = 32;
  constexpr int kMinLpcOrder = 4;
  if (context_size < kMinLpcSignalLength || max_order < kMinLpcOrder) return true;
  if (!can_use_lpc(context_size, max_order)) return true;

  const int nu = static_cast<int>(gap);
  const int nrows = static_cast<int>(context_size) - max_order;  // number of filter-output rows
  if (nrows <= 0) return true;

  const int iters = std::max(params.iterations, 1);
  // Blend weight for the LPC estimate vs. the Step-1 interpolation baseline,
  // clamped to [0, 1]. blend == 1 reproduces the pure-LPC behaviour; blend == 0
  // leaves the cubic / linear interpolation untouched.
  const float blend = std::clamp(params.blend, 0.0f, 1.0f);
  Eigen::MatrixXf au(nrows, nu);
  Eigen::VectorXf rhs(nrows);
  Eigen::MatrixXf ata(nu, nu);
  Eigen::VectorXf at_rhs(nu);

  for (int outer = 0; outer < iters; ++outer) {
    // Step 2a – AR estimation from the bounded local context.
    const auto model = lpc_burg(context.data(), context_size, max_order);
    const int p = max_order;

    // Step 2b – build A_u and A_k vectors for the normal equations.
    // A is (nrows x context_size) Toeplitz.  Row r (r = 0..nrows-1) acts at sample r+p:
    //   (A x)[r] = x[r+p] + a[1]*x[r+p-1] + ... + a[p]*x[r]
    // We only need A_u (nrows x nu) and the product A_k * y_k.

    // A_u: each column j corresponds to the contiguous local gap sample.
    au.setZero();

    // rhs = -A_k y_k, initialised as -A * samples (all columns),
    // then we subtract A_u * x_u to isolate A_k * y_k.
    rhs.setZero();

    for (int r = 0; r < nrows; ++r) {
      // Compute (A * samples)[r] = sum_k a[k] * samples[r+p-k] for k=0..p
      double Ax_r = 0.0;
      for (int k = 0; k <= p; ++k) {
        Ax_r += static_cast<double>(model.ar[static_cast<size_t>(k)]) *
                context[static_cast<size_t>(r + p - k)];
      }
      rhs[r] = -static_cast<float>(Ax_r);  // start with -A*samples

      // Now fill A_u columns for this row and adjust rhs to isolate A_k*y_k.
      for (int j = 0; j < nu; ++j) {
        const int col = static_cast<int>(local_start) + j;
        // A[r, col] is the coefficient for position col in row r.
        // Row r acts at time r+p; A[r, col] = a[r+p - col] if 0 <= r+p-col <= p, else 0.
        const int lag = (r + p) - col;
        if (lag >= 0 && lag <= p) {
          const float a_lag = model.ar[static_cast<size_t>(lag)];
          au(r, j) = a_lag;
          // rhs = -A_k*y_k = -(A*x)[r] + A_u*x_u → we need to add back A_u[r,j]*x_u[j]
          rhs[r] += a_lag * context[static_cast<size_t>(col)];
        }
      }
    }
    // rhs now holds -A_k * y_k (correctly isolating known-column contributions).

    // Step 2d – solve (A_u^T A_u + lambda I) x_u = A_u^T rhs
    // Tikhonov regularization lambda guards rank-deficient systems (gap > model rank).
    const float lambda = static_cast<float>(nrows) * kEpsilon;
    ata.noalias() = au.transpose() * au;
    ata.diagonal().array() += lambda;
    at_rhs.noalias() = au.transpose() * rhs;

    Eigen::LDLT<Eigen::MatrixXf> solver(ata);
    if (solver.info() != Eigen::Success) break;  // numerical failure: keep current estimate

    const Eigen::VectorXf x_u = solver.solve(at_rhs);
    if (!x_u.allFinite()) break;

    // Step 2e – write the LDLT estimate back into the unknown positions, blended
    // against the Step-1 interpolation baseline by params.blend. No clipping
    // constraint is applied: see the declaration for why this stays a general gap
    // filler rather than a strict clipping-consistency reconstructor.
    for (int j = 0; j < nu; ++j) {
      const size_t idx = local_start + static_cast<size_t>(j);
      context[idx] = blend * x_u[j] + (1.0f - blend) * baseline[idx - local_start];
    }
  }

  std::copy(context.begin() + static_cast<std::ptrdiff_t>(local_start),
            context.begin() + static_cast<std::ptrdiff_t>(local_end), samples + start);
  return true;
}

}  // namespace sonare
