#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/exception.h"

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
  std::vector<float> padded(size + 2 * static_cast<size_t>(pad), 0.0f);
  if (data == nullptr || size == 0) return padded;
  for (size_t i = 0; i < padded.size(); ++i) {
    padded[i] = data[reflect_index(static_cast<int64_t>(i) - pad, size)];
  }
  return padded;
}

}  // namespace sonare
