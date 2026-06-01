/// @file padding.cpp
/// @brief Implementation of padding and length-adjustment utilities.

#include "util/padding.h"

#include <algorithm>
#include <cstring>

#include "util/exception.h"

namespace sonare {

std::vector<float> pad_center(const float* x, std::size_t n, std::size_t size, float pad_value) {
  if (size < n) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pad_center: target size smaller than input");
  }
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pad_center: null input with non-zero length");
  }
  std::vector<float> out(size, pad_value);
  const std::size_t lpad = (size - n) / 2;
  if (n > 0) {
    std::memcpy(out.data() + lpad, x, n * sizeof(float));
  }
  return out;
}

std::vector<float> pad_center(const std::vector<float>& x, std::size_t size, float pad_value) {
  return pad_center(x.data(), x.size(), size, pad_value);
}

std::vector<float> fix_length(const float* x, std::size_t n, std::size_t size, float pad_value) {
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "fix_length: null input with non-zero length");
  }
  if (n == size) {
    return std::vector<float>(x, x + n);
  }
  if (n > size) {
    return std::vector<float>(x, x + size);
  }
  std::vector<float> out(size, pad_value);
  if (n > 0) {
    std::memcpy(out.data(), x, n * sizeof(float));
  }
  return out;
}

std::vector<float> fix_length(const std::vector<float>& x, std::size_t size, float pad_value) {
  return fix_length(x.data(), x.size(), size, pad_value);
}

std::vector<int> fix_frames(const std::vector<int>& frames, int x_min, int x_max, bool pad) {
  // Validate monotonicity and bounds.
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (frames[i] < frames[i - 1]) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "fix_frames: frames must be non-decreasing");
    }
  }
  if (x_min >= 0) {
    for (int f : frames) {
      if (f < 0) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "fix_frames: negative frame with x_min >= 0");
      }
    }
  }

  std::vector<int> out;
  out.reserve(frames.size() + 2);

  if (pad && x_min >= 0) {
    out.push_back(x_min);
  }
  for (int f : frames) {
    if (x_min >= 0 && f < x_min) continue;
    if (x_max >= 0 && f > x_max) continue;
    out.push_back(f);
  }
  if (pad && x_max >= 0) {
    out.push_back(x_max);
  }

  // Sort and remove duplicates (librosa uses np.unique which sorts).
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace sonare
