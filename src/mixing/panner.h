#pragma once

/// @file panner.h
/// @brief Stereo panner with selectable pan laws.

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
  float pan() const noexcept { return config_.pan; }

  void set_pan_law(PanLaw law) noexcept { config_.pan_law = law; }
  PanLaw pan_law() const noexcept { return config_.pan_law; }

 private:
  PannerConfig config_{};
  double sample_rate_ = 48000.0;
  rt::ParamSmoother left_{1.0f, 5.0f, 48000.0};
  rt::ParamSmoother right_{1.0f, 5.0f, 48000.0};
};

}  // namespace sonare::mixing
