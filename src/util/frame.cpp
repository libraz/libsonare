/// @file frame.cpp
/// @brief Implementation of sliding-window framing.

#include "util/frame.h"

#include <cstdint>
#include <cstring>
#include <limits>

#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare {

int frame_count(std::size_t n, int frame_length, int hop_length) {
  if (frame_length <= 0 || hop_length <= 0) return 0;
  if (n < static_cast<std::size_t>(frame_length)) return 0;
  const std::size_t count =
      (n - static_cast<std::size_t>(frame_length)) / static_cast<std::size_t>(hop_length) + 1;
  SONARE_CHECK_MSG(count <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
                   ErrorCode::InvalidParameter, "frame count exceeds int range");
  return static_cast<int>(count);
}

std::vector<float> frame(const float* x, std::size_t n, int frame_length, int hop_length) {
  util::check::integer_within(frame_length, kFrameLengthDomain, "frame_length");
  util::check::integer_within(hop_length, kFrameHopLengthDomain, "hop_length");
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "null input with non-zero length");
  }
  if (!numeric::all_finite(x, n)) {
    throw SonareException(ErrorCode::InvalidParameter, "input contains a non-finite sample");
  }

  const int n_frames = frame_count(n, frame_length, hop_length);
  if (n_frames == 0) {
    return {};
  }

  // The frame count fits an int, but its product with the frame length can wrap a 32-bit size_t.
  const auto total = util::check::element_count(static_cast<std::uint64_t>(n_frames), frame_length,
                                                resource::kMaxOfflineAudioSamples, "hop_length",
                                                "the framed output");
  std::vector<float> out(total);
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
