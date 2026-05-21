#pragma once

/// @file band_pass.h
/// @brief Dedicated band-pass and notch filters.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class BandPassEq : public common::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_band_pass(float frequency_hz, float q = 1.0f, bool enabled = true);
  void set_notch(float frequency_hz, float q = 1.0f, bool enabled = true);
  void clear_band_pass();
  void clear_notch();
  void clear();

  const EqBand& band_pass() const;
  const EqBand& notch() const;

 private:
  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
