/// @file preemphasis.cpp
/// @brief Implementation of pre/de-emphasis filters.

#include "effects/preemphasis.h"

#include <stdexcept>

namespace sonare {

namespace {

/// @brief Default zi for preemphasis (matches librosa.effects.preemphasis).
float default_zi_pre(const float* x, std::size_t n) {
  if (n == 0) return 0.0f;
  if (n == 1) return x[0];
  return 2.0f * x[0] - x[1];
}

/// @brief Default zi for deemphasis (matches librosa.effects.deemphasis).
/// @details librosa runs the deemphasis filter with zi=0 then subtracts a
///          decaying transient k*coef^n where k = ((2-coef)*y[0] - y[1])/(3-coef).
///          That subtraction is equivalent to running scipy.lfilter with
///          zi = -k, which is what we apply directly.
float default_zi_de(const float* x, std::size_t n, float coef) {
  if (n == 0) return 0.0f;
  if (n == 1) return -x[0];
  const float denom = 3.0f - coef;
  if (denom == 0.0f) return 0.0f;
  return -((2.0f - coef) * x[0] - x[1]) / denom;
}

}  // namespace

std::vector<float> preemphasis(const float* x, std::size_t n, float coef, std::optional<float> zi) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("preemphasis: null input with non-zero length");
  }
  std::vector<float> y(n);
  if (n == 0) return y;
  const float z = zi.has_value() ? *zi : default_zi_pre(x, n);
  y[0] = x[0] + z;
  for (std::size_t i = 1; i < n; ++i) {
    y[i] = x[i] - coef * x[i - 1];
  }
  return y;
}

std::vector<float> preemphasis(const std::vector<float>& x, float coef, std::optional<float> zi) {
  return preemphasis(x.data(), x.size(), coef, zi);
}

std::vector<float> deemphasis(const float* x, std::size_t n, float coef, std::optional<float> zi) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("deemphasis: null input with non-zero length");
  }
  std::vector<float> y(n);
  if (n == 0) return y;
  const float z = zi.has_value() ? *zi : default_zi_de(x, n, coef);
  y[0] = x[0] + z;
  for (std::size_t i = 1; i < n; ++i) {
    y[i] = x[i] + coef * y[i - 1];
  }
  return y;
}

std::vector<float> deemphasis(const std::vector<float>& x, float coef, std::optional<float> zi) {
  return deemphasis(x.data(), x.size(), coef, zi);
}

}  // namespace sonare
