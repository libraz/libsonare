#include "mixing/stereo_width.h"

#include <algorithm>
#include <cmath>

namespace sonare::mixing {

StereoWidthProcessor::StereoWidthProcessor(float width, float smoothing_ms)
    : smoothing_ms_(smoothing_ms), width_target_(std::clamp(width, 0.0f, 2.0f)) {
  // Seed the smoother at the initial width so a processor used without an explicit prepare()
  // (e.g. the direct process() path in tests) reflects the requested width from the first
  // sample, instead of ramping from the smoother's default starting value of 1.0.
  smoother_.reset(width_target_.load(std::memory_order_relaxed));
}

void StereoWidthProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
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

void StereoWidthProcessor::set_width(float width) noexcept {
  width_target_.store(std::clamp(width, 0.0f, 2.0f), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
