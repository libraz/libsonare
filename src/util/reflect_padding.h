#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

inline size_t reflect_index(int64_t index, size_t size) {
  if (size <= 1) return 0;
  const int64_t period = static_cast<int64_t>(2 * size - 2);
  int64_t wrapped = index % period;
  if (wrapped < 0) wrapped += period;
  if (wrapped >= static_cast<int64_t>(size)) {
    wrapped = period - wrapped;
  }
  return static_cast<size_t>(wrapped);
}

inline std::vector<float> reflect_center_pad(const float* data, size_t size, int pad) {
  if (pad < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "reflect_center_pad: pad must be non-negative");
  }
  const size_t margin = static_cast<size_t>(pad);
  size_t padded_size = 0;
  if (!numeric::checked_add(size, 2 * margin, &padded_size)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "reflect_center_pad: output length overflows");
  }
  std::vector<float> padded(padded_size, 0.0f);
  if (data == nullptr || size == 0) return padded;
  std::copy_n(data, size, padded.data() + margin);
  for (size_t i = 0; i < margin; ++i) {
    padded[i] = data[reflect_index(static_cast<int64_t>(i) - pad, size)];
    const size_t right = margin + size + i;
    padded[right] = data[reflect_index(static_cast<int64_t>(size + i), size)];
  }
  return padded;
}

}  // namespace sonare
