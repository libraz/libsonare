#include "filters/dct.h"

#include <cmath>
#include <map>
#include <utility>

#include "util/exception.h"

namespace sonare {

namespace {
constexpr float kPi = 3.14159265358979323846f;

/// @brief Maximum number of cached DCT matrices per thread.
constexpr size_t kMaxDctCacheSize = 8;

/// @brief Thread-local cache for DCT matrices.
thread_local std::map<std::pair<int, int>, std::vector<float>> g_dct_cache;
}  // namespace

std::vector<float> create_dct_matrix(int n_output, int n_input) {
  SONARE_CHECK(n_output > 0 && n_input > 0, ErrorCode::InvalidParameter);

  std::vector<float> matrix(n_output * n_input);

  // DCT-II with orthonormal normalization
  // D[k, n] = sqrt(2/N) * cos(pi * k * (2n + 1) / (2N))
  // with D[0, n] *= 1/sqrt(2) for k=0

  float scale = std::sqrt(2.0f / n_input);

  for (int k = 0; k < n_output; ++k) {
    float k_scale = (k == 0) ? scale / std::sqrt(2.0f) : scale;
    for (int n = 0; n < n_input; ++n) {
      float angle = kPi * k * (2.0f * n + 1.0f) / (2.0f * n_input);
      matrix[k * n_input + n] = k_scale * std::cos(angle);
    }
  }

  return matrix;
}

const std::vector<float>& get_dct_matrix_cached(int n_output, int n_input) {
  auto key = std::make_pair(n_output, n_input);
  auto it = g_dct_cache.find(key);
  if (it != g_dct_cache.end()) {
    return it->second;
  }

  // Clear cache if it exceeds the size limit
  if (g_dct_cache.size() >= kMaxDctCacheSize) {
    g_dct_cache.clear();
  }

  // Create and cache the matrix
  auto result = g_dct_cache.emplace(key, create_dct_matrix(n_output, n_input));
  return result.first->second;
}

std::vector<float> dct_ii(const float* input, int n_input, int n_output) {
  SONARE_CHECK(input != nullptr && n_input > 0, ErrorCode::InvalidParameter);

  if (n_output <= 0) {
    n_output = n_input;
  }

  std::vector<float> output(n_output, 0.0f);

  // Direct computation of DCT-II
  float scale = std::sqrt(2.0f / n_input);

  for (int k = 0; k < n_output; ++k) {
    float k_scale = (k == 0) ? scale / std::sqrt(2.0f) : scale;
    float sum = 0.0f;
    for (int n = 0; n < n_input; ++n) {
      float angle = kPi * k * (2.0f * n + 1.0f) / (2.0f * n_input);
      sum += input[n] * std::cos(angle);
    }
    output[k] = k_scale * sum;
  }

  return output;
}

std::vector<float> dct_ii(const std::vector<float>& input, int n_output) {
  return dct_ii(input.data(), static_cast<int>(input.size()), n_output);
}

std::vector<float> idct_ii(const float* input, int n_input, int n_output) {
  SONARE_CHECK(input != nullptr && n_input > 0, ErrorCode::InvalidParameter);

  if (n_output <= 0) {
    n_output = n_input;
  }

  std::vector<float> output(n_output, 0.0f);

  // Inverse DCT-II (DCT-III with orthonormal scaling)
  float scale = std::sqrt(2.0f / n_output);

  for (int n = 0; n < n_output; ++n) {
    float sum = input[0] / std::sqrt(2.0f);  // DC component with special scaling
    for (int k = 1; k < n_input; ++k) {
      float angle = kPi * k * (2.0f * n + 1.0f) / (2.0f * n_output);
      sum += input[k] * std::cos(angle);
    }
    output[n] = scale * sum;
  }

  return output;
}

}  // namespace sonare
