#include "filters/dct.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"
#include "util/thread_local_cache.h"

namespace sonare {

using sonare::constants::kPi;

namespace {

/// @brief Maximum number of cached DCT matrices per thread.
constexpr size_t kMaxDctCacheSize = 8;

/// @brief Thread-local cache for DCT matrices.
thread_local std::map<std::pair<int, int>, std::vector<float>> g_dct_cache;

bool checked_dct_element_count(int n_output, int n_input, std::size_t* element_count) noexcept {
  if (element_count == nullptr || n_output <= 0 || n_input <= 0) return false;

  const std::vector<float> empty;
  const std::size_t limit =
      std::min(static_cast<std::size_t>(std::numeric_limits<int>::max()), empty.max_size());
  return numeric::checked_size_product(static_cast<std::size_t>(n_output),
                                       static_cast<std::size_t>(n_input), limit, element_count);
}

void validate_dct_dimensions(int n_output, int n_input) {
  std::size_t element_count = 0;
  SONARE_CHECK(checked_dct_element_count(n_output, n_input, &element_count),
               ErrorCode::InvalidParameter);
}
}  // namespace

std::vector<float> create_dct_matrix(int n_output, int n_input) {
  std::size_t element_count = 0;
  SONARE_CHECK(checked_dct_element_count(n_output, n_input, &element_count),
               ErrorCode::InvalidParameter);

  std::vector<float> matrix(element_count);

  // DCT-II with orthonormal normalization
  // D[k, n] = sqrt(2/N) * cos(pi * k * (2n + 1) / (2N))
  // with D[0, n] *= 1/sqrt(2) for k=0

  float scale = std::sqrt(2.0f / n_input);

  for (int k = 0; k < n_output; ++k) {
    float k_scale = (k == 0) ? scale / std::sqrt(2.0f) : scale;
    for (int n = 0; n < n_input; ++n) {
      float angle = kPi * k * (2.0f * n + 1.0f) / (2.0f * n_input);
      const std::size_t index = static_cast<std::size_t>(k) * static_cast<std::size_t>(n_input) +
                                static_cast<std::size_t>(n);
      matrix[index] = k_scale * std::cos(angle);
    }
  }

  return matrix;
}

const std::vector<float>& get_dct_matrix_cached(int n_output, int n_input) {
  validate_dct_dimensions(n_output, n_input);
  auto key = std::make_pair(n_output, n_input);
  return get_or_create_bounded_cache_entry(g_dct_cache, key, kMaxDctCacheSize,
                                           [&] { return create_dct_matrix(n_output, n_input); });
}

std::vector<float> dct_ii(const float* input, int n_input, int n_output) {
  SONARE_CHECK(input != nullptr && n_input > 0, ErrorCode::InvalidParameter);

  if (n_output <= 0) {
    n_output = n_input;
  }

  validate_dct_dimensions(n_output, n_input);
  std::vector<float> output(n_output, 0.0f);

  // Use the cached DCT-II matrix (D[k, n] = k_scale * cos(angle)) so the
  // cosines are computed once per (n_output, n_input) shape instead of on every
  // call (mfcc_to_mel invokes this per frame). Numerically equivalent to the
  // direct formula: output[k] = sum_n input[n] * D[k, n].
  const std::vector<float>& matrix = get_dct_matrix_cached(n_output, n_input);

  for (int k = 0; k < n_output; ++k) {
    float sum = 0.0f;
    const std::size_t row_offset = static_cast<std::size_t>(k) * static_cast<std::size_t>(n_input);
    const float* row = matrix.data() + row_offset;
    for (int n = 0; n < n_input; ++n) {
      sum += input[n] * row[n];
    }
    output[static_cast<std::size_t>(k)] = sum;
  }

  return output;
}

std::vector<float> dct_ii(const std::vector<float>& input, int n_output) {
  SONARE_CHECK(input.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
               ErrorCode::InvalidParameter);
  return dct_ii(input.data(), static_cast<int>(input.size()), n_output);
}

std::vector<float> idct_ii(const float* input, int n_input, int n_output) {
  SONARE_CHECK(input != nullptr && n_input > 0, ErrorCode::InvalidParameter);

  if (n_output <= 0) {
    n_output = n_input;
  }

  validate_dct_dimensions(n_input, n_output);
  std::vector<float> output(n_output, 0.0f);
  idct_ii_into(input, n_input, output.data(), n_output);
  return output;
}

void idct_ii_into(const float* input, int n_input, float* output, int n_output) {
  SONARE_CHECK(input != nullptr && output != nullptr && n_input > 0 && n_output > 0,
               ErrorCode::InvalidParameter);
  validate_dct_dimensions(n_input, n_output);
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
      const std::size_t matrix_index =
          static_cast<std::size_t>(k) * static_cast<std::size_t>(n_output) +
          static_cast<std::size_t>(n);
      sum += input[k] * matrix[matrix_index];
    }
    output[static_cast<std::size_t>(n)] = sum;
  }
}

}  // namespace sonare
