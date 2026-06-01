#pragma once

/// @file stereo_width.h
/// @brief Mid/side stereo width processor.

#include <atomic>

#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

class StereoWidthProcessor : public rt::ProcessorBase {
 public:
  explicit StereoWidthProcessor(float width = 1.0f, float smoothing_ms = 5.0f);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Sets the stereo width factor.
  /// @param width Width in the range [0, 2]; values are clamped. 0 collapses to
  /// mono, 1 preserves the original stereo image, and 2 is the maximum widening.
  /// The mid (mono/center) component is preserved at all widths; only the side
  /// component is scaled by @p width, so a centered or mono source keeps its level.
  void set_width(float width) noexcept;
  float width() const noexcept { return width_target_.load(std::memory_order_relaxed); }

 private:
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother smoother_{1.0f, 5.0f, 48000.0};
  std::atomic<float> width_target_{1.0f};
};

}  // namespace sonare::mixing
