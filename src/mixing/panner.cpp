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
  pan_smoother_.prepare(sample_rate_, smoothing_ms_);
  dual_left_smoother_.prepare(sample_rate_, smoothing_ms_);
  dual_right_smoother_.prepare(sample_rate_, smoothing_ms_);
  reset();
}

void PannerProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  // One load per block: the law applies to the whole block. A law or mode change
  // takes effect at the next block as a step, like the engine lane pan.
  const PanLaw law = pan_law_.load(std::memory_order_relaxed);
  const PanMode mode = pan_mode_.load(std::memory_order_relaxed);
  pan_smoother_.set_target(pan_.load(std::memory_order_relaxed));

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
    for (int i = 0; i < num_samples; ++i) {
      const PanGains g =
          pan_cache_.get(pan_smoother_.process_settling(), law, PanNormalization::Raw);
      channels[0][i] *= std::sqrt(g.left * g.left + g.right * g.right);
    }
    return;
  }

  if (channels[1] == nullptr) {
    return;
  }

  if (mode == PanMode::StereoPan) {
    for (int i = 0; i < num_samples; ++i) {
      const PanGains g =
          pan_cache_.get(pan_smoother_.process_settling(), law, PanNormalization::Raw);
      const float mono = 0.5f * (channels[0][i] + channels[1][i]);
      channels[0][i] = mono * g.left;
      channels[1][i] = mono * g.right;
    }
    return;
  }

  if (mode == PanMode::DualPan) {
    dual_left_smoother_.set_target(dual_pan_left_.load(std::memory_order_relaxed));
    dual_right_smoother_.set_target(dual_pan_right_.load(std::memory_order_relaxed));
    // The main pan smoother keeps advancing once per sample so a switch back to
    // Balance or StereoPan continues from the same position.
    for (int i = 0; i < num_samples; ++i) {
      (void)pan_smoother_.process_settling();
      const PanGains a =
          dual_left_cache_.get(dual_left_smoother_.process_settling(), law, PanNormalization::Raw);
      const PanGains b = dual_right_cache_.get(dual_right_smoother_.process_settling(), law,
                                               PanNormalization::Raw);
      const float in_l = channels[0][i];
      const float in_r = channels[1][i];
      channels[0][i] = in_l * a.left + in_r * b.left;
      channels[1][i] = in_l * a.right + in_r * b.right;
    }
    return;
  }

  // Balance (default): a balance control leaves the existing stereo image
  // intact and is unity at center, attenuating only the channel away from the
  // pan direction — PanNormalization::NearUnity. Multiplying each channel by its
  // raw pan gain would instead attenuate a centered signal by ~3 dB under the
  // constant-power default law (both gains = cos(pi/4) = 0.707).
  for (int i = 0; i < num_samples; ++i) {
    const PanGains g =
        pan_cache_.get(pan_smoother_.process_settling(), law, PanNormalization::NearUnity);
    channels[0][i] *= g.left;
    channels[1][i] *= g.right;
  }
}

void PannerProcessor::reset() {
  pan_smoother_.reset(pan_.load(std::memory_order_relaxed));
  dual_left_smoother_.reset(dual_pan_left_.load(std::memory_order_relaxed));
  dual_right_smoother_.reset(dual_pan_right_.load(std::memory_order_relaxed));
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

void PannerProcessor::copy_state_from(const PannerProcessor& other) noexcept {
  if (this == &other) return;
  sample_rate_ = other.sample_rate_;
  smoothing_ms_ = other.smoothing_ms_;
  pan_smoother_ = other.pan_smoother_;
  dual_left_smoother_ = other.dual_left_smoother_;
  dual_right_smoother_ = other.dual_right_smoother_;
  pan_.store(other.pan_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  dual_pan_left_.store(other.dual_pan_left_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
  dual_pan_right_.store(other.dual_pan_right_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  pan_law_.store(other.pan_law_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  pan_mode_.store(other.pan_mode_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool PannerProcessor::at_rest_identity() const noexcept {
  if (pan_mode_.load(std::memory_order_relaxed) != PanMode::Balance ||
      pan_.load(std::memory_order_relaxed) != 0.0f) {
    return false;
  }
  // The smoother's target only moves inside process(), so settle state is the current position.
  return pan_smoother_.current() == 0.0f;
}

}  // namespace sonare::mixing
