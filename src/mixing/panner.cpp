#include "mixing/panner.h"

#include <algorithm>
#include <cmath>

namespace sonare::mixing {

PannerProcessor::PannerProcessor(PannerConfig config)
    : smoothing_ms_(config.smoothing_ms), pan_(config.pan), pan_law_(config.pan_law) {}

void PannerProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  left_.prepare(sample_rate_, smoothing_ms_);
  right_.prepare(sample_rate_, smoothing_ms_);
  reset();
}

void PannerProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const PanGains gains = compute_pan_gains(pan_.load(std::memory_order_relaxed),
                                           pan_law_.load(std::memory_order_relaxed));
  left_.set_target(gains.left);
  right_.set_target(gains.right);

  if (num_channels == 1) {
    // A mono channel has no L/R to spread, so we apply the panner's energy
    // contribution as a single gain. Advance each smoother exactly once per
    // sample to stay in sync with the stereo path. The RMS combination
    // sqrt(l^2 + r^2) keeps a centered signal at unity for the constant-power
    // default law (l == r == 1/sqrt(2) -> 1.0) instead of boosting it.
    for (int i = 0; i < num_samples; ++i) {
      const float l = left_.process();
      const float r = right_.process();
      channels[0][i] *= std::sqrt(l * l + r * r);
    }
    return;
  }

  if (channels[0] == nullptr || channels[1] == nullptr) {
    return;
  }

  for (int i = 0; i < num_samples; ++i) {
    channels[0][i] *= left_.process();
    channels[1][i] *= right_.process();
  }
}

void PannerProcessor::reset() {
  const PanGains gains = compute_pan_gains(pan_.load(std::memory_order_relaxed),
                                           pan_law_.load(std::memory_order_relaxed));
  left_.reset(gains.left);
  right_.reset(gains.right);
}

void PannerProcessor::set_pan(float pan) noexcept {
  pan_.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
