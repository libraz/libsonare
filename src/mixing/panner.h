#pragma once

/// @file panner.h
/// @brief Stereo panner with selectable pan laws.

#include <atomic>

#include "mixing/pan_law.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

struct PannerConfig {
  float pan = 0.0f;
  PanLaw pan_law = PanLaw::Const3dB;
  float smoothing_ms = 5.0f;
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

 private:
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother left_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother right_{1.0f, 5.0f, 48000.0};
  std::atomic<float> pan_{0.0f};
  std::atomic<PanLaw> pan_law_{PanLaw::Const3dB};
};

}  // namespace sonare::mixing
