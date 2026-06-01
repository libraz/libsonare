#include "filters/dct.h"

#include <cmath>
#include <map>
#include <utility>

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kPi;

namespace {

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

  // Use the cached DCT-II matrix (D[k, n] = k_scale * cos(angle)) so the
  // cosines are computed once per (n_output, n_input) shape instead of on every
  // call (mfcc_to_mel invokes this per frame). Numerically equivalent to the
  // direct formula: output[k] = sum_n input[n] * D[k, n].
  const std::vector<float>& matrix = get_dct_matrix_cached(n_output, n_input);

  for (int k = 0; k < n_output; ++k) {
    float sum = 0.0f;
    const float* row = &matrix[static_cast<size_t>(k) * n_input];
    for (int n = 0; n < n_input; ++n) {
      sum += input[n] * row[n];
    }
    output[k] = sum;
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

  // Inverse DCT-II (DCT-III with orthonormal scaling). The orthonormal inverse
  // is the transpose of the forward matrix, so reuse the cached DCT-II matrix
  // D = create_dct_matrix(n_input, n_output) where
  //   D[k, n] = k_scale(k) * cos(pi * k * (2n + 1) / (2 * n_output))
  // and output[n] = sum_k input[k] * D[k, n]. k_scale already folds in the
  // sqrt(2/n_output) scaling and the 1/sqrt(2) DC factor, matching the direct
  // formula. This computes the cosines once per shape instead of every call.
  const std::vector<float>& matrix = get_dct_matrix_cached(n_input, n_output);

  for (int n = 0; n < n_output; ++n) {
    float sum = 0.0f;
    for (int k = 0; k < n_input; ++k) {
      sum += input[k] * matrix[static_cast<size_t>(k) * n_output + n];
    }
    output[n] = sum;
  }

  return output;
}

}  // namespace sonare
