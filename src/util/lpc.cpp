#include "util/lpc.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare {
namespace {

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

  std::vector<float> ef(x, x + n);
  std::vector<float> eb(x, x + n);
  std::vector<float> a(static_cast<size_t>(order + 1), 0.0f);
  a[0] = 1.0f;

  double error = 0.0;
  for (size_t i = 0; i < n; ++i) error += static_cast<double>(x[i]) * x[i];
  error /= static_cast<double>(n);

  for (int m = 1; m <= order; ++m) {
    double numerator = 0.0;
    double denominator = 0.0;
    for (size_t i = static_cast<size_t>(m); i < n; ++i) {
      numerator += static_cast<double>(ef[i]) * eb[i - 1];
      denominator +=
          static_cast<double>(ef[i]) * ef[i] + static_cast<double>(eb[i - 1]) * eb[i - 1];
    }
    if (denominator <= 1.0e-20) {
      break;
    }

    const float reflection = static_cast<float>(-2.0 * numerator / denominator);
    std::vector<float> next_a = a;
    for (int i = 1; i < m; ++i) {
      next_a[static_cast<size_t>(i)] =
          a[static_cast<size_t>(i)] + reflection * a[static_cast<size_t>(m - i)];
    }
    next_a[static_cast<size_t>(m)] = reflection;
    a = next_a;

    for (size_t i = n - 1; i >= static_cast<size_t>(m); --i) {
      const float f = ef[i];
      const float b = eb[i - 1];
      ef[i] = f + reflection * b;
      eb[i - 1] = b + reflection * f;
      if (i == static_cast<size_t>(m)) break;
    }

    error *= 1.0 - static_cast<double>(reflection) * reflection;
    error = std::max(error, 0.0);
  }

  result.ar = std::move(a);
  result.variance = static_cast<float>(error);
  return result;
}

LpcResult lpc_autocorrelation(const float* x, size_t n, int order) {
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

  std::vector<double> r(static_cast<size_t>(order + 1), 0.0);
  for (int lag = 0; lag <= order; ++lag) {
    for (size_t i = static_cast<size_t>(lag); i < n; ++i) {
      r[static_cast<size_t>(lag)] += static_cast<double>(x[i]) * x[i - static_cast<size_t>(lag)];
    }
    r[static_cast<size_t>(lag)] /= static_cast<double>(n);
  }

  std::vector<double> a(static_cast<size_t>(order + 1), 0.0);
  a[0] = 1.0;
  double error = r[0];
  if (error <= 1.0e-20) {
    result.variance = 0.0f;
    return result;
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

std::vector<float> ar_interpolate(const float* x, const bool* mask, size_t n,
                                  const LpcResult& model) {
  if ((x == nullptr || mask == nullptr) && n > 0) {
    throw SonareException(ErrorCode::InvalidParameter, "input and mask must not be null");
  }
  if (model.ar.empty() || model.ar[0] == 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid LPC model");
  }

  std::vector<float> out(x, x + n);
  const size_t order = model.ar.size() - 1;
  for (size_t i = 0; i < n; ++i) {
    if (mask[i]) {
      continue;
    }
    double predicted = 0.0;
    const size_t max_k = std::min(order, i);
    for (size_t k = 1; k <= max_k; ++k) {
      predicted -= static_cast<double>(model.ar[k]) * out[i - k];
    }
    out[i] = static_cast<float>(predicted);
  }
  return out;
}

}  // namespace sonare
