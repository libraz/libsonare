#include "mixing/panner.h"

#include <algorithm>
#include <cmath>

namespace sonare::mixing {

PannerProcessor::PannerProcessor(PannerConfig config)
    : smoothing_ms_(std::isfinite(config.smoothing_ms) && config.smoothing_ms >= 0.0f
                        ? config.smoothing_ms
                        : 5.0f),
      pan_(std::isfinite(config.pan) ? std::clamp(config.pan, -1.0f, 1.0f) : 0.0f),
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

  // One load per block: the law is read again below to normalize the smoothed
  // pair, and both reads must describe the same law for the whole block.
  const PanLaw law = pan_law_.load(std::memory_order_relaxed);
  const PanGains gains = compute_pan_gains(pan_.load(std::memory_order_relaxed), law);
  left_.set_target(gains.left);
  right_.set_target(gains.right);
  const PanMode mode = pan_mode_.load(std::memory_order_relaxed);

  // Every branch below writes to channels[0]; the stereo branches also write to
  // channels[1]. An unbound plane is a supported state in this layer -- the
  // engine and the monitor runtime both hand over partially-bound plane tables,
  // and every sibling processor (gain, alignment delay, stereo width, meter)
  // tolerates it -- so the check belongs above the mono short-circuit rather
  // than after it, where the mono path could never reach it.
  if (channels[0] == nullptr) {
    return;
  }

  if (num_channels == 1) {
    // A mono channel has no L/R stereo image to balance, so — unlike the stereo
    // Balance path below, which normalizes the near channel to unity for every
    // law — the mono path applies the pan law's literal combined energy as a
    // single gain: sqrt(l^2 + r^2). By design this keeps a centered signal at
    // unity for the constant-power default law (l == r == 1/sqrt(2) -> 1.0) and
    // otherwise follows the law's raw energy, so a centered mono strip and a
    // centered stereo strip agree only under the constant-power default. That
    // difference is intentional: a mono strip conveys the pan law's energy
    // directly rather than re-balancing a stereo image it does not have.
    // Advance each smoother exactly once per sample to stay in sync with the
    // stereo path.
    for (int i = 0; i < num_samples; ++i) {
      const float l = left_.process();
      const float r = right_.process();
      channels[0][i] *= std::sqrt(l * l + r * r);
    }
    return;
  }

  if (channels[1] == nullptr) {
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
  // pan direction — PanNormalization::NearUnity. Multiplying each channel by its
  // raw pan gain would instead attenuate a centered signal by ~3 dB under the
  // constant-power default law (both gains = cos(pi/4) = 0.707). The smoothers
  // interpolate the raw law gains, so the normalization is applied to the
  // interpolated pair rather than to the targets. This matches the mono path's
  // "centered signal stays at unity" intent.
  for (int i = 0; i < num_samples; ++i) {
    const float l = left_.process();
    const float r = right_.process();
    const PanGains g = normalize_pan_gains({l, r}, law, PanNormalization::NearUnity);
    channels[0][i] *= g.left;
    channels[1][i] *= g.right;
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
  if (!std::isfinite(pan)) return;
  pan_.store(clamp_pan(pan), std::memory_order_relaxed);
}

void PannerProcessor::set_dual_pan(float left_pan, float right_pan) noexcept {
  if (!std::isfinite(left_pan) || !std::isfinite(right_pan)) return;
  dual_pan_left_.store(clamp_pan(left_pan), std::memory_order_relaxed);
  dual_pan_right_.store(clamp_pan(right_pan), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
