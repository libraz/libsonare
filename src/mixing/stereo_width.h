#pragma once

/// @file stereo_width.h
/// @brief Mid/side stereo width processor.

#include <algorithm>
#include <atomic>

#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

/// @brief The width factor the processor will actually store for @p width.
/// @details Shared with the C ABI setter and the scene walker for the same
///          reason as @ref clamp_pan: a cached copy that is echoed back must be
///          the value the processor is using.
inline float clamp_width(float width) noexcept { return std::clamp(width, 0.0f, 2.0f); }

/// @note The width factor is clamped to the range [0, 2] everywhere it is set
///       (constructor and @ref set_width): 0 collapses to mono, 1 preserves the
///       original image, 2 is the maximum widening. Values outside [0, 2] are
///       clamped, NOT honored — a request of 3 or 4 behaves as 2.
class StereoWidthProcessor : public rt::ProcessorBase {
 public:
  /// @brief Constructs the processor.
  /// @param width Initial width factor, clamped to [0, 2] (see class note).
  /// @param smoothing_ms Parameter-smoothing time constant in milliseconds.
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

  /// @brief Current (smoothed) width factor. Lags @ref width() while the
  /// internal smoother ramps toward a newly set target; equal to @ref width()
  /// once settled. Callers that skip processing at width 1 must also check this
  /// so an in-flight ramp back toward 1 is not cut off mid-glide (which would
  /// jump the side component and click).
  float current_width() const noexcept { return smoother_.current(); }

 private:
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother smoother_{1.0f, 5.0f, 48000.0};
  std::atomic<float> width_target_{1.0f};
};

}  // namespace sonare::mixing
