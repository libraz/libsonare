#pragma once

/// @file dc_blocker.h
/// @brief First-order DC blocker for feedback/nonlinear processors.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::effects::common {

class DcBlocker : public rt::ProcessorBase {
 public:
  explicit DcBlocker(float pole = 0.995f);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  float process_sample(int channel, float sample);

  /// @brief Returns the per-channel history to rest once a non-finite value has
  /// reached it (see util/non_finite_state.h). process() applies this to its own
  /// cells; an owner driving process_sample() applies it for this filter.
  /// @return true when any channel was discarded.
  bool discard_non_finite() noexcept;

  void set_pole(float pole) noexcept;
  float pole() const noexcept { return pole_; }

  /// @brief Sets the high-pass corner frequency (Hz). Takes effect on the next
  /// prepare() call, which derives the filter pole from the sample rate.
  void set_cutoff_hz(float cutoff_hz) noexcept;
  float cutoff_hz() const noexcept { return cutoff_hz_; }

 private:
  static constexpr float kDefaultCutoffHz = 20.0f;

  float pole_ = 0.995f;
  float cutoff_hz_ = kDefaultCutoffHz;
  std::vector<float> x1_;
  std::vector<float> y1_;
};

}  // namespace sonare::effects::common
