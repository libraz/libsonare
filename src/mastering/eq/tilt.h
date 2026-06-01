#pragma once

/// @file tilt.h
/// @brief Simple two-shelf tilt equalizer.

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

class TiltEq : public rt::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_tilt_db(float tilt_db);
  void set_pivot_hz(float pivot_hz);

  float tilt_db() const { return tilt_db_; }
  float pivot_hz() const { return pivot_hz_; }

  // Automatable parameters (RT-safe: recomputes the two shelf biquads in place
  // via update_bands(), preserves filter state):
  //   0 = tilt_db (positive boosts highs / cuts lows; signed)
  //   1 = pivot_hz (clamped to (0 Hz, Nyquist))
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  void update_bands();

  ParametricEq eq_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float tilt_db_ = 0.0f;
  float pivot_hz_ = 1000.0f;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::eq
