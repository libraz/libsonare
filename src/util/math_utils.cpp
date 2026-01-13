/// @file math_utils.cpp
/// @brief Implementation of math utility functions.

#include "util/math_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonare {

float cosine_similarity(const float* a, const float* b, size_t size) {
  if (size == 0) return 0.0f;

  float dot = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;

  for (size_t i = 0; i < size; ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
  if (denom < 1e-10f) return 0.0f;
  return dot / denom;
}

float pearson_correlation(const float* a, const float* b, size_t size) {
  if (size < 2) return 0.0f;

  float mean_a = mean(a, size);
  float mean_b = mean(b, size);

  float num = 0.0f;
  float den_a = 0.0f;
  float den_b = 0.0f;

  for (size_t i = 0; i < size; ++i) {
    float da = a[i] - mean_a;
    float db = b[i] - mean_b;
    num += da * db;
    den_a += da * da;
    den_b += db * db;
  }

  float denom = std::sqrt(den_a * den_b);
  if (denom < 1e-10f) return 0.0f;
  return num / denom;
}

float median(const float* data, size_t size) {
  if (size == 0) return 0.0f;

  std::vector<float> sorted(data, data + size);
  std::sort(sorted.begin(), sorted.end());

  if (size % 2 == 0) {
    return (sorted[size / 2 - 1] + sorted[size / 2]) / 2.0f;
  }
  return sorted[size / 2];
}

float percentile(const float* data, size_t size, float p) {
  if (size == 0) return 0.0f;

  std::vector<float> sorted(data, data + size);
  std::sort(sorted.begin(), sorted.end());

  float idx = (p / 100.0f) * (size - 1);
  size_t lo = static_cast<size_t>(idx);
  size_t hi = std::min(lo + 1, size - 1);
  float frac = idx - lo;

  return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

}  // namespace sonare
