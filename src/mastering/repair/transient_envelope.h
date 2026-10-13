#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sonare::mastering::repair {

/// @brief Local level of the sample-to-sample difference, the envelope impulsive detectors read
///        a candidate against.
/// @details A broadband transient that is the music (a drum hit) raises the difference level
///   around itself for milliseconds; a crackle or a click stands alone above whatever surrounds
///   it. The level beside a candidate is the mean |x[n] - x[n-1]| over a window on each side of
///   it, and the louder side is the one read, so a candidate at the onset of a hit is measured
///   against the hit rather than against the silence before it.
class TransientEnvelope {
 public:
  /// Window on each side, in seconds.
  static constexpr double kWindowSeconds = 0.002;
  /// Samples set aside on each side of a candidate, which an impulse's own edges occupy.
  static constexpr std::size_t kGuardSamples = 2;

  TransientEnvelope(const float* samples, std::size_t size, int sample_rate)
      : radius_(std::max<std::size_t>(
            1, static_cast<std::size_t>(kWindowSeconds * static_cast<double>(sample_rate)))),
        prefix_(size + 1, 0.0) {
    for (std::size_t i = 1; i < size; ++i) {
      prefix_[i + 1] = prefix_[i] + std::abs(static_cast<double>(samples[i]) - samples[i - 1]);
    }
  }

  /// Level beside the run [@p begin, @p end): the louder of the two side windows' mean.
  double level(std::size_t begin, std::size_t end) const {
    const std::size_t size = prefix_.size() - 1;
    const std::size_t left_end = begin > kGuardSamples ? begin - kGuardSamples : 0;
    const std::size_t left_begin = left_end > radius_ ? left_end - radius_ : 0;
    const std::size_t right_begin = std::min(size, end + kGuardSamples + 1);
    const std::size_t right_end = std::min(size, right_begin + radius_);
    return std::max(mean(left_begin, left_end), mean(right_begin, right_end));
  }

 private:
  double mean(std::size_t begin, std::size_t end) const {
    if (end <= begin) return 0.0;
    return (prefix_[end] - prefix_[begin]) / static_cast<double>(end - begin);
  }

  std::size_t radius_;
  std::vector<double> prefix_;  ///< prefix_[n] sums |x[i] - x[i-1]| for i < n.
};

}  // namespace sonare::mastering::repair
