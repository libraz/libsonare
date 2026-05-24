#include "mixing/panner.h"

#include <algorithm>
#include <cmath>

namespace sonare::mixing {

PannerProcessor::PannerProcessor(PannerConfig config)
    : smoothing_ms_(config.smoothing_ms),
      pan_(config.pan),
      pan_law_(config.pan_law),
      pan_mode_(config.mode) {}

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
  const PanMode mode = pan_mode_.load(std::memory_order_relaxed);

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

  if (mode == PanMode::StereoPan) {
    for (int i = 0; i < num_samples; ++i) {
      const float mono = 0.5f * (channels[0][i] + channels[1][i]);
      channels[0][i] = mono * left_.process();
      channels[1][i] = mono * right_.process();
    }
    return;
  }

  if (mode == PanMode::DualPan) {
    const PanLaw law = pan_law_.load(std::memory_order_relaxed);
    const PanGains left_gains =
        compute_pan_gains(dual_pan_left_.load(std::memory_order_relaxed), law);
    const PanGains right_gains =
        compute_pan_gains(dual_pan_right_.load(std::memory_order_relaxed), law);
    // Keep the main smoothers advancing once per sample for mode switches while the
    // dual-pan gains themselves are applied sample-accurately as a routing matrix.
    for (int i = 0; i < num_samples; ++i) {
      (void)left_.process();
      (void)right_.process();
      const float in_l = channels[0][i];
      const float in_r = channels[1][i];
      channels[0][i] = in_l * left_gains.left + in_r * right_gains.left;
      channels[1][i] = in_l * left_gains.right + in_r * right_gains.right;
    }
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

void PannerProcessor::set_dual_pan(float left_pan, float right_pan) noexcept {
  dual_pan_left_.store(std::clamp(left_pan, -1.0f, 1.0f), std::memory_order_relaxed);
  dual_pan_right_.store(std::clamp(right_pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
