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
  dual_ll_.prepare(sample_rate_, smoothing_ms_);
  dual_lr_.prepare(sample_rate_, smoothing_ms_);
  dual_rl_.prepare(sample_rate_, smoothing_ms_);
  dual_rr_.prepare(sample_rate_, smoothing_ms_);
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
    dual_ll_.set_target(left_gains.left);
    dual_lr_.set_target(left_gains.right);
    dual_rl_.set_target(right_gains.left);
    dual_rr_.set_target(right_gains.right);
    // Apply the dual-pan gains sample-accurately as a smoothed routing matrix while
    // keeping the main smoothers advancing once per sample for continuous mode switches.
    for (int i = 0; i < num_samples; ++i) {
      (void)left_.process();
      (void)right_.process();
      const float ll = dual_ll_.process();
      const float lr = dual_lr_.process();
      const float rl = dual_rl_.process();
      const float rr = dual_rr_.process();
      const float in_l = channels[0][i];
      const float in_r = channels[1][i];
      channels[0][i] = in_l * ll + in_r * rl;
      channels[1][i] = in_l * lr + in_r * rr;
    }
    return;
  }

  // Balance (default): a balance control leaves the existing stereo image
  // intact and is unity at center, attenuating only the channel away from the
  // pan direction. Multiplying each channel by its raw pan gain would attenuate
  // a centered signal by ~3 dB under the constant-power default law (both gains
  // = cos(pi/4) = 0.707). To keep center at unity for any law, normalize by the
  // louder (near) channel so it stays at unity and only the away channel is
  // pulled down by the same ratio the raw pan law dictates. This matches the
  // mono path's "centered signal stays at unity" intent.
  for (int i = 0; i < num_samples; ++i) {
    const float l = left_.process();
    const float r = right_.process();
    const float norm = std::max(l, r);
    const float inv_norm = norm > 0.0f ? 1.0f / norm : 0.0f;
    channels[0][i] *= l * inv_norm;
    channels[1][i] *= r * inv_norm;
  }
}

void PannerProcessor::reset() {
  const PanGains gains = compute_pan_gains(pan_.load(std::memory_order_relaxed),
                                           pan_law_.load(std::memory_order_relaxed));
  left_.reset(gains.left);
  right_.reset(gains.right);

  const PanLaw law = pan_law_.load(std::memory_order_relaxed);
  const PanGains left_gains =
      compute_pan_gains(dual_pan_left_.load(std::memory_order_relaxed), law);
  const PanGains right_gains =
      compute_pan_gains(dual_pan_right_.load(std::memory_order_relaxed), law);
  dual_ll_.reset(left_gains.left);
  dual_lr_.reset(left_gains.right);
  dual_rl_.reset(right_gains.left);
  dual_rr_.reset(right_gains.right);
}

void PannerProcessor::set_pan(float pan) noexcept {
  pan_.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void PannerProcessor::set_dual_pan(float left_pan, float right_pan) noexcept {
  dual_pan_left_.store(std::clamp(left_pan, -1.0f, 1.0f), std::memory_order_relaxed);
  dual_pan_right_.store(std::clamp(right_pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
