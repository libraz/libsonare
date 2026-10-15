#pragma once

#include <algorithm>
#include <cmath>

namespace sonare::playback {

/// Milliseconds to a frame count at @p sample_rate, rounded and floored at one frame.
inline int ms_to_frames(double ms, double sample_rate) {
  constexpr double kSecondsPerMs = 1e-3;
  return std::max(1, static_cast<int>(std::lround(ms * kSecondsPerMs * sample_rate)));
}

}  // namespace sonare::playback
