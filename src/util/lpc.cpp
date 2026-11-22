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

// Handles the empty-input and order-0 cases, where the variance is the mean energy; returns
// whether it did.
bool fill_trivial_variance(const float* x, size_t n, int order, LpcResult* result) {
  if (n != 0 && order != 0) return false;
  double energy = 0.0;
  for (size_t i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
  result->variance = n == 0 ? 0.0f : static_cast<float>(energy / static_cast<double>(n));
  return true;
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
  if (fill_trivial_variance(x, n, order, &result)) return result;

  std::vector<double> ar(static_cast<size_t>(order + 1), 0.0);
  std::vector<double> ar_prev(static_cast<size_t>(order + 1), 0.0);
  ar[0] = 1.0;
  ar_prev[0] = 1.0;

  std::vector<double> fwd(x + 1, x + n);
  std::vector<double> bwd(x, x + n - 1);
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
    // The recursive energy update drifts once the residual is exhausted; a reflection at or past
    // 1 there would make the model unstable, so the recursion ends at the last stable order.
    if (!(std::abs(reflection) < 1.0)) break;
    result.order_reached = i + 1;

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

    // order < n, so active_len is still positive here.
    const double q = 1.0 - reflection * reflection;
    const double bwd_back = bwd[active_len - 1];
    const double fwd_front = fwd[fwd_start];
    den = q * den - bwd_back * bwd_back - fwd_front * fwd_front;

    ++fwd_start;
    --active_len;
  }

  for (int i = 0; i <= order; ++i) {
    result.ar[static_cast<size_t>(i)] = static_cast<float>(ar[static_cast<size_t>(i)]);
  }
  result.variance = static_cast<float>(std::max(den, 0.0) / static_cast<double>(n));
  return result;
}

void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out) {
  LpcWorkspace workspace;
  lpc_autocorrelation(x, n, order, out, &workspace);
}

void lpc_autocorrelation(const float* x, size_t n, int order, LpcResult* out,
                         LpcWorkspace* workspace) {
  if (out == nullptr || workspace == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  validate_lpc_args(x, n, order);
  LpcResult& result = *out;
  result.variance = 0.0f;
  result.ar.assign(static_cast<size_t>(order + 1), 0.0f);
  result.ar[0] = 1.0f;
  result.order_reached = 0;
  if (fill_trivial_variance(x, n, order, &result)) return;

  std::vector<float>& raw = workspace->lags;
  unnormalized_autocorrelation(x, n, static_cast<size_t>(order + 1), &workspace->autocorrelation,
                               &raw);
  std::vector<double>& r = workspace->r;
  r.assign(static_cast<size_t>(order + 1), 0.0);
  for (int lag = 0; lag <= order; ++lag) {
    r[static_cast<size_t>(lag)] =
        static_cast<double>(raw[static_cast<size_t>(lag)]) / static_cast<double>(n);
  }

  std::vector<double>& a = workspace->a;
  a.assign(static_cast<size_t>(order + 1), 0.0);
  a[0] = 1.0;
  double error = r[0];
  if (error <= 1.0e-20) {
    result.variance = 0.0f;
    return;
  }

  std::vector<double>& next_a = workspace->next_a;
  for (int i = 1; i <= order; ++i) {
    double acc = r[static_cast<size_t>(i)];
    for (int j = 1; j < i; ++j) {
      acc += a[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];
    }
    const double reflection = -acc / error;
    next_a = a;
    for (int j = 1; j < i; ++j) {
      next_a[static_cast<size_t>(j)] =
          a[static_cast<size_t>(j)] + reflection * a[static_cast<size_t>(i - j)];
    }
    next_a[static_cast<size_t>(i)] = reflection;
    a.swap(next_a);
    result.order_reached = i;
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
  std::vector<float> residual;
  lpc_residual(x, n, model, &residual);
  return residual;
}

void lpc_residual(const float* x, size_t n, const LpcResult& model, std::vector<float>* out) {
  if (x == nullptr && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "input must not be null");
  }
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  if (model.ar.empty() || model.ar[0] == 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid LPC model");
  }
  const size_t order = model.ar.size() - 1;
  std::vector<float>& residual = *out;
  residual.assign(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    double e = x[i];
    const size_t max_k = std::min(order, i);
    for (size_t k = 1; k <= max_k; ++k) {
      e += static_cast<double>(model.ar[k]) * x[i - k];
    }
    residual[i] = static_cast<float>(e);
  }
}

void interpolate_gap(float* samples, size_t n, size_t start, size_t end) {
  validate_gap_args(samples, n, start, end);
  for (size_t j = start; j < end; ++j) {
    samples[j] = interpolate_gap_sample(samples, n, start, end, j);
  }
}

ArFill ar_interpolate_region(float* samples, size_t n, size_t start, size_t end,
                             const ArInterpolateParams& params) {
  validate_gap_args(samples, n, start, end);
  const size_t gap = end - start;
  if (gap > params.max_gap) return ArFill::kGapTooLong;
  if (gap == 0) return ArFill::kFilled;

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
  const size_t context_size = context.size();

  // Signed bound of each constrained sample, at least as far out as the sample itself; 0 where
  // the input sits inside min_magnitude.
  std::vector<double> bound(gap, 0.0);
  if (params.min_magnitude > 0.0f) {
    for (size_t j = 0; j < gap; ++j) {
      const float v = context[local_start + j];
      if (std::abs(v) >= params.min_magnitude) bound[j] = v;
    }
  }
  const bool constrained =
      std::any_of(bound.begin(), bound.end(), [](double b) { return b != 0.0; });

  for (size_t j = local_start; j < local_end; ++j) {
    context[j] = interpolate_gap_sample(context.data(), context_size, local_start, local_end, j);
  }

  // Too little context or model order for an AR estimate to beat the baseline.
  constexpr size_t kMinLpcSignalLength = 32;
  constexpr int kMinLpcOrder = 4;
  const int p = std::min(params.order, static_cast<int>(std::max<size_t>(1, context_size / 4)));
  if (context_size < kMinLpcSignalLength || p < kMinLpcOrder) return ArFill::kNotSolved;

  const size_t order = static_cast<size_t>(p);
  const size_t last_row = context_size - 1;
  // Rows t in [order, last_row] carry e_t = sum_k a[k] x[t-k]; only those touching the gap matter.
  const size_t row_lo = std::max(order, local_start);
  const size_t row_hi = std::min(last_row, local_end - 1 + order);
  const double lambda = static_cast<double>(context_size - order) * static_cast<double>(kEpsilon);
  const int iters = std::max(params.iterations, 1);

  Eigen::MatrixXd ata(gap, gap);
  std::vector<double> a(order + 1);
  std::vector<double> r_a(order + 1);
  std::vector<double> residual(row_hi - row_lo + 1);
  std::vector<size_t> free_cols;
  std::vector<char> pinned(gap);
  Eigen::MatrixXd system;
  Eigen::VectorXd at_rhs;
  Eigen::LDLT<Eigen::MatrixXd> solver;

  for (int outer = 0; outer < iters; ++outer) {
    const LpcResult model = lpc_burg(context.data(), context_size, p);
    for (size_t k = 0; k <= order; ++k) a[k] = static_cast<double>(model.ar[k]);
    for (size_t d = 0; d <= order; ++d) {
      double sum = 0.0;
      for (size_t m = 0; m + d <= order; ++m) sum += a[m] * a[m + d];
      r_a[d] = sum;
    }
    // A_u^T A_u straight from the banded filter: R_a(d) where every row touching both columns
    // exists, a truncated sum at the context edges.
    for (size_t i = 0; i < gap; ++i) {
      const size_t ci = local_start + i;
      for (size_t j = i; j < gap; ++j) {
        const size_t cj = local_start + j;
        const size_t d = j - i;
        double value = 0.0;
        if (d <= order) {
          const size_t t_lo = std::max(order, cj);
          const size_t t_hi = std::min(last_row, ci + order);
          if (t_lo == cj && t_hi == ci + order) {
            value = r_a[d];
          } else {
            for (size_t t = t_lo; t <= t_hi; ++t) value += a[t - ci] * a[t - cj];
          }
        }
        ata(i, j) = value;
        ata(j, i) = value;
      }
    }

    // Active set: pin each constrained sample whose solution falls inside its bound, re-solve.
    std::fill(pinned.begin(), pinned.end(), 0);
    for (;;) {
      free_cols.clear();
      for (size_t j = 0; j < gap; ++j) {
        if (pinned[j] != 0) {
          context[local_start + j] = static_cast<float>(bound[j]);
        } else {
          free_cols.push_back(j);
          context[local_start + j] = 0.0f;
        }
      }
      const Eigen::Index nu = static_cast<Eigen::Index>(free_cols.size());
      // Every sample on its bound: the constrained solution is the bound itself.
      if (nu == 0) break;

      for (size_t t = row_lo; t <= row_hi; ++t) {
        double e = 0.0;
        for (size_t k = 0; k <= order; ++k) e += a[k] * static_cast<double>(context[t - k]);
        residual[t - row_lo] = e;
      }
      system.resize(nu, nu);
      at_rhs.resize(nu);
      for (Eigen::Index fi = 0; fi < nu; ++fi) {
        const size_t i = free_cols[static_cast<size_t>(fi)];
        const size_t c = local_start + i;
        double rhs = 0.0;
        const size_t t_lo = std::max(row_lo, c);
        const size_t t_hi = std::min(row_hi, c + order);
        for (size_t t = t_lo; t <= t_hi; ++t) rhs -= a[t - c] * residual[t - row_lo];
        at_rhs[fi] = rhs;
        for (Eigen::Index fj = 0; fj < nu; ++fj) {
          system(fi, fj) = ata(static_cast<Eigen::Index>(i),
                               static_cast<Eigen::Index>(free_cols[static_cast<size_t>(fj)]));
        }
        system(fi, fi) += lambda;
      }
      solver.compute(system);
      if (solver.info() != Eigen::Success) return ArFill::kNotSolved;
      const Eigen::VectorXd x = solver.solve(at_rhs);
      if (!x.allFinite()) return ArFill::kNotSolved;

      bool changed = false;
      for (Eigen::Index fi = 0; fi < nu; ++fi) {
        const size_t j = free_cols[static_cast<size_t>(fi)];
        context[local_start + j] = static_cast<float>(x[fi]);
        const double b = bound[j];
        if ((b > 0.0 && x[fi] < b) || (b < 0.0 && x[fi] > b)) {
          pinned[j] = 1;
          changed = true;
        }
      }
      if (!constrained || !changed) break;
    }
  }

  for (size_t j = local_start; j < local_end; ++j) {
    if (!std::isfinite(context[j])) return ArFill::kNotSolved;
  }
  std::copy(context.begin() + static_cast<std::ptrdiff_t>(local_start),
            context.begin() + static_cast<std::ptrdiff_t>(local_end), samples + start);
  return ArFill::kFilled;
}

}  // namespace sonare
