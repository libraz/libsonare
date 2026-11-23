/// @file padding.cpp
/// @brief Implementation of padding and length-adjustment utilities.

#include "util/padding.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare {

namespace {

std::size_t padded_length(std::size_t size) {
  return util::check::element_count(static_cast<std::uint64_t>(size), 1u,
                                    resource::kMaxOfflineAudioSamples, "target_size",
                                    "the padded output");
}

}  // namespace

std::vector<float> pad_center(const float* x, std::size_t n, std::size_t size, float pad_value) {
  if (size < n) {
    throw SonareException(ErrorCode::InvalidParameter, "target size smaller than input");
  }
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "null input with non-zero length");
  }
  std::vector<float> out(padded_length(size), pad_value);
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
    throw SonareException(ErrorCode::InvalidParameter, "null input with non-zero length");
  }
  padded_length(size);
  // A null pointer with zero length is a valid empty input.  Return the
  // requested all-padding result directly instead of constructing the
  // [nullptr, nullptr) iterator range used by the equal-length path.
  if (n == 0) {
    return std::vector<float>(size, pad_value);
  }
  if (n == size) {
    return std::vector<float>(x, x + n);
  }
  if (n > size) {
    return std::vector<float>(x, x + size);
  }
  std::vector<float> out(size, pad_value);
  std::memcpy(out.data(), x, n * sizeof(float));
  return out;
}

std::vector<float> fix_length(const std::vector<float>& x, std::size_t size, float pad_value) {
  return fix_length(x.data(), x.size(), size, pad_value);
}

std::vector<int> fix_frames(const std::vector<int>& frames, int x_min, int x_max, bool pad) {
  // An empty frame list has no honest answer: with pad the padded bounds would
  // be indistinguishable from real frames at those positions, and returning an
  // empty vector would silently drop the requested padding. Reject instead of
  // inventing either result.
  if (frames.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "frames must not be empty");
  }
  // Validate monotonicity and bounds.
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (frames[i] < frames[i - 1]) {
      throw SonareException(ErrorCode::InvalidParameter, "frames must be non-decreasing");
    }
  }
  if (x_min >= 0) {
    for (int f : frames) {
      if (f < 0) {
        throw SonareException(ErrorCode::InvalidParameter, "negative frame with x_min >= 0");
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
