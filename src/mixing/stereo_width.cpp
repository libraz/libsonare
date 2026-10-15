#include "mixing/stereo_width.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mixing {

using constants::kDefaultDawSampleRate;

StereoWidthProcessor::StereoWidthProcessor(float width, float smoothing_ms)
    : smoothing_ms_(std::isfinite(smoothing_ms) && smoothing_ms >= 0.0f ? smoothing_ms : 5.0f),
      width_target_(width) {
  if (!width_in_range(width)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StereoWidthProcessor width must be finite and within [0, 2]");
  }
  // Seed the smoother at the initial width so a processor used without an explicit prepare()
  // (e.g. the direct process() path in tests) reflects the requested width from the first
  // sample, instead of ramping from the smoother's default starting value of 1.0.
  smoother_.reset(width_target_.load(std::memory_order_relaxed));
}

void StereoWidthProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  smoother_.prepare(sample_rate_, smoothing_ms_);
  reset();
}

void StereoWidthProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels < 2 || num_samples <= 0 || channels[0] == nullptr ||
      channels[1] == nullptr) {
    return;
  }
  smoother_.set_target(width_target_.load(std::memory_order_relaxed));
  for (int i = 0; i < num_samples; ++i) {
    const float w = smoother_.process();
    // Standard M/S width law: the mid (mono/center) component is left untouched so
    // raising the width never attenuates a centered or mono source, and only the side
    // component is scaled by w. A purely panned or mono signal therefore keeps its
    // level as width increases, while the stereo difference is widened or narrowed.
    const float mid = 0.5f * (channels[0][i] + channels[1][i]);
    const float side = 0.5f * (channels[0][i] - channels[1][i]) * w;
    channels[0][i] = mid + side;
    channels[1][i] = mid - side;
  }
}

void StereoWidthProcessor::reset() {
  smoother_.reset(width_target_.load(std::memory_order_relaxed));
}

bool StereoWidthProcessor::set_width(float width) noexcept {
  if (!width_in_range(width)) return false;
  width_target_.store(width, std::memory_order_relaxed);
  return true;
}

void StereoWidthProcessor::copy_state_from(const StereoWidthProcessor& other) noexcept {
  if (this == &other) return;
  sample_rate_ = other.sample_rate_;
  smoothing_ms_ = other.smoothing_ms_;
  smoother_ = other.smoother_;
  width_target_.store(other.width_target_.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
}

}  // namespace sonare::mixing
