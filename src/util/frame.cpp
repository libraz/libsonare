/// @file frame.cpp
/// @brief Implementation of sliding-window framing.

#include "util/frame.h"

#include <cstring>

#include "util/exception.h"

namespace sonare {

int frame_count(std::size_t n, int frame_length, int hop_length) {
  if (frame_length <= 0 || hop_length <= 0) return 0;
  if (n < static_cast<std::size_t>(frame_length)) return 0;
  return static_cast<int>((n - static_cast<std::size_t>(frame_length)) /
                          static_cast<std::size_t>(hop_length)) +
         1;
}

std::vector<float> frame(const float* x, std::size_t n, int frame_length, int hop_length) {
  if (frame_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "frame: frame_length must be > 0");
  }
  if (hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "frame: hop_length must be > 0");
  }
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "frame: null input with non-zero length");
  }

  const int n_frames = frame_count(n, frame_length, hop_length);
  if (n_frames == 0) {
    return {};
  }

  std::vector<float> out(static_cast<std::size_t>(n_frames) *
                         static_cast<std::size_t>(frame_length));
  const std::size_t flen = static_cast<std::size_t>(frame_length);
  const std::size_t hop = static_cast<std::size_t>(hop_length);
  for (int i = 0; i < n_frames; ++i) {
    std::memcpy(out.data() + static_cast<std::size_t>(i) * flen,
                x + static_cast<std::size_t>(i) * hop, flen * sizeof(float));
  }
  return out;
}

std::vector<float> frame(const std::vector<float>& x, int frame_length, int hop_length) {
  return frame(x.data(), x.size(), frame_length, hop_length);
}

}  // namespace sonare
