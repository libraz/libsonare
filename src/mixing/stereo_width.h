#pragma once

/// @file stereo_width.h
/// @brief Mid/side stereo width processor.

#include <atomic>
#include <cmath>

#include "rt/param_smoother.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::mixing {

/// Documented width domain; a factor outside it is refused, never clamped.
inline constexpr float kWidthMin = 0.0f;
inline constexpr float kWidthMax = 2.0f;

/// @brief True for a finite width factor inside [kWidthMin, kWidthMax].
inline bool width_in_range(float width) noexcept {
  return std::isfinite(width) && width >= kWidthMin && width <= kWidthMax;
}

/// @note The width factor is in [0, 2]: 0 collapses to mono, 1 preserves the original image,
///       2 is the maximum widening. A value outside [0, 2] is refused by the constructor
///       (SonareException) and by @ref set_width (returns false).
class StereoWidthProcessor : public rt::ProcessorBase {
 public:
  /// @brief Constructs the processor.
  /// @param width Initial width factor in [0, 2].
  /// @throws SonareException (InvalidParameter) when @p width is outside [0, 2] or non-finite.
  /// @param smoothing_ms Parameter-smoothing time constant in milliseconds.
  explicit StereoWidthProcessor(float width = 1.0f, float smoothing_ms = 5.0f);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Sets the stereo width factor.
  /// @param width Width in the range [0, 2]. 0 collapses to
  /// mono, 1 preserves the original stereo image, and 2 is the maximum widening.
  /// The mid (mono/center) component is preserved at all widths; only the side
  /// component is scaled by @p width, so a centered or mono source keeps its level.
  /// @return false, leaving the width unchanged, when @p width is outside [0, 2] or non-finite.
  bool set_width(float width) noexcept;

  /// @brief Copies the in-flight DSP state from another width processor.
  /// @details The source and destination must be control-quiescent: neither may
  ///          be processed or mutated concurrently. This is an allocation-free
  ///          state handoff for a retained bus slot and deliberately excludes
  ///          generic @c ProcessorBase metadata such as bypass and detector state.
  void copy_state_from(const StereoWidthProcessor& other) noexcept;

  float width() const noexcept { return width_target_.load(std::memory_order_relaxed); }

  /// @brief Current (smoothed) width factor. Lags @ref width() while the
  /// internal smoother ramps toward a newly set target; equal to @ref width()
  /// once settled. Callers that skip processing at width 1 must also check this
  /// so an in-flight ramp back toward 1 is not cut off mid-glide (which would
  /// jump the side component and click).
  float current_width() const noexcept { return smoother_.current(); }

 private:
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother smoother_{1.0f, 5.0f, sonare::constants::kDefaultDawSampleRate};
  std::atomic<float> width_target_{1.0f};
};

}  // namespace sonare::mixing
