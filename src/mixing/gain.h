#pragma once

/// @file gain.h
/// @brief Realtime-safe channel gain processor with VCA offset support.

#include <atomic>

#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

struct GainConfig {
  float gain_db = 0.0f;
  float smoothing_ms = 5.0f;
};

class GainProcessor : public rt::ProcessorBase {
 public:
  explicit GainProcessor(GainConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_gain_db(float gain_db) noexcept;
  float gain_db() const noexcept { return config_.gain_db; }

  void set_vca_offset_db(float offset_db) noexcept;
  float vca_offset_db() const noexcept;

 private:
  GainConfig config_{};
  double sample_rate_ = 48000.0;
  rt::ParamSmoother smoother_{1.0f, 5.0f, 48000.0};
  std::atomic<float> vca_offset_db_{0.0f};
};

}  // namespace sonare::mixing
