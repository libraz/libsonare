#pragma once

/// @file math_utils.h
/// @brief Mathematical utility functions for signal processing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace sonare {

/// @brief Clamps a value between min and max.
/// @tparam T Numeric type
/// @param value Value to clamp
/// @param min_val Minimum bound
/// @param max_val Maximum bound
/// @return Clamped value
template <typename T>
T clamp(T value, T min_val, T max_val) {
  return std::max(min_val, std::min(value, max_val));
}

/// @brief Returns the index of the maximum element.
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
/// @return Index of maximum element (0 if empty)
template <typename T>
size_t argmax(const T* data, size_t size) {
  if (size == 0) return 0;
  return std::distance(data, std::max_element(data, data + size));
}

/// @brief Computes the arithmetic mean.
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
/// @return Mean value (0 if empty)
template <typename T>
T mean(const T* data, size_t size) {
  if (size == 0) return T{0};
  T sum = std::accumulate(data, data + size, T{0});
  return sum / static_cast<T>(size);
}

/// @brief Computes the population variance.
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
/// @return Variance (0 if size < 2)
template <typename T>
T variance(const T* data, size_t size) {
  if (size < 2) return T{0};
  T m = mean(data, size);
  T sum_sq = T{0};
  for (size_t i = 0; i < size; ++i) {
    T diff = data[i] - m;
    sum_sq += diff * diff;
  }
  return sum_sq / static_cast<T>(size);
}

/// @brief Computes the standard deviation.
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
/// @return Standard deviation
template <typename T>
T stddev(const T* data, size_t size) {
  return std::sqrt(variance(data, size));
}

/// @brief Computes the L2 norm (Euclidean length).
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
/// @return L2 norm
template <typename T>
T norm_l2(const T* data, size_t size) {
  T sum_sq = T{0};
  for (size_t i = 0; i < size; ++i) {
    sum_sq += data[i] * data[i];
  }
  return std::sqrt(sum_sq);
}

/// @brief Normalizes array to unit L2 norm in-place.
/// @tparam T Numeric type
/// @param data Pointer to data array
/// @param size Number of elements
template <typename T>
void normalize_l2(T* data, size_t size) {
  T n = norm_l2(data, size);
  if (n > T{1e-10}) {
    for (size_t i = 0; i < size; ++i) {
      data[i] /= n;
    }
  }
}

/// @brief Computes cosine similarity between two vectors.
/// @param a First vector
/// @param b Second vector
/// @param size Number of elements (must be same for both)
/// @return Cosine similarity in [-1, 1]
float cosine_similarity(const float* a, const float* b, size_t size);

/// @brief Computes Pearson correlation coefficient.
/// @param a First vector
/// @param b Second vector
/// @param size Number of elements (must be same for both)
/// @return Correlation coefficient in [-1, 1]
float pearson_correlation(const float* a, const float* b, size_t size);

/// @brief Computes the median value.
/// @param data Pointer to data array
/// @param size Number of elements
/// @return Median value (0 if empty)
float median(const float* data, size_t size);

/// @brief Computes the p-th percentile.
/// @param data Pointer to data array
/// @param size Number of elements
/// @param p Percentile in [0, 100]
/// @return Percentile value (0 if empty)
float percentile(const float* data, size_t size, float p);

/// @brief Returns the smallest power of 2 greater than or equal to n.
/// @param n Input value
/// @return Smallest power of 2 >= n (returns 1 if n <= 0)
inline int next_power_of_2(int n) {
  if (n <= 0) return 1;
  int power = 1;
  while (power < n) {
    power *= 2;
  }
  return power;
}

}  // namespace sonare
