#pragma once

/// @file panner.h
/// @brief Stereo panner with selectable pan laws.

#include <atomic>

#include "mixing/pan_law.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

enum class PanMode {
  Balance,
  StereoPan,
  DualPan,
};

struct PannerConfig {
  float pan = 0.0f;
  PanLaw pan_law = PanLaw::Const3dB;
  float smoothing_ms = 5.0f;
  PanMode mode = PanMode::Balance;
};

class PannerProcessor : public rt::ProcessorBase {
 public:
  explicit PannerProcessor(PannerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_pan(float pan) noexcept;
  float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }

  void set_pan_law(PanLaw law) noexcept { pan_law_.store(law, std::memory_order_relaxed); }
  PanLaw pan_law() const noexcept { return pan_law_.load(std::memory_order_relaxed); }

  void set_pan_mode(PanMode mode) noexcept { pan_mode_.store(mode, std::memory_order_relaxed); }
  PanMode pan_mode() const noexcept { return pan_mode_.load(std::memory_order_relaxed); }

  void set_dual_pan(float left_pan, float right_pan) noexcept;
  float dual_pan_left() const noexcept { return dual_pan_left_.load(std::memory_order_relaxed); }
  float dual_pan_right() const noexcept { return dual_pan_right_.load(std::memory_order_relaxed); }

 private:
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother left_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother right_{1.0f, 5.0f, 48000.0};
  // Dual-pan 2x2 routing matrix coefficients (input -> output):
  // dual_ll_ = left in -> left out, dual_lr_ = left in -> right out,
  // dual_rl_ = right in -> left out, dual_rr_ = right in -> right out.
  rt::ParamSmoother dual_ll_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother dual_lr_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother dual_rl_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother dual_rr_{1.0f, 5.0f, 48000.0};
  std::atomic<float> pan_{0.0f};
  std::atomic<float> dual_pan_left_{-1.0f};
  std::atomic<float> dual_pan_right_{1.0f};
  std::atomic<PanLaw> pan_law_{PanLaw::Const3dB};
  std::atomic<PanMode> pan_mode_{PanMode::Balance};
};

}  // namespace sonare::mixing
