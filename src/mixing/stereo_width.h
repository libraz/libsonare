#pragma once

/// @file stereo_width.h
/// @brief Mid/side stereo width processor.

#include "rt/processor_base.h"

namespace sonare::mixing {

class StereoWidthProcessor : public rt::ProcessorBase {
 public:
  explicit StereoWidthProcessor(float width = 1.0f);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override {}

  /// @brief Sets the stereo width factor.
  /// @param width Width in the range [0, 2]; values are clamped. 0 collapses to
  /// mono, 1 preserves the original stereo image, and 2 is the maximum widening.
  /// Values above 1 can drive the side signal beyond the original amplitude and
  /// are intentionally not amplitude-limited here.
  void set_width(float width) noexcept;
  float width() const noexcept { return width_; }

 private:
  float width_ = 1.0f;
};

}  // namespace sonare::mixing
