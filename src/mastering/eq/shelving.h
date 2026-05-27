#pragma once

/// @file shelving.h
/// @brief Dedicated low/high shelving equalizer.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"
#include "util/constants.h"

namespace sonare::mastering::eq {

class ShelvingEq : public common::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_low_shelf(float frequency_hz, float gain_db, float q = sonare::constants::kButterworthQ,
                     bool enabled = true);
  void set_high_shelf(float frequency_hz, float gain_db, float q = sonare::constants::kButterworthQ,
                      bool enabled = true);
  void clear_low_shelf();
  void clear_high_shelf();
  void clear();

  // Automatable parameters (RT-safe: recomputes affected biquad coefficients in
  // place, preserves filter state). Low shelf is band 0, high shelf is band 1,
  // matching ParametricEq's block-of-3 layout:
  //   0 = low-shelf frequency_hz (clamped to (0 Hz, Nyquist))
  //   1 = low-shelf gain_db
  //   2 = low-shelf Q (clamped to > 0)
  //   3 = high-shelf frequency_hz (clamped to (0 Hz, Nyquist))
  //   4 = high-shelf gain_db
  //   5 = high-shelf Q (clamped to > 0)
  bool set_parameter(unsigned int param_id, float value) override {
    return param_id <= 5u && eq_.set_parameter(param_id, value);
  }

  const EqBand& low_shelf() const;
  const EqBand& high_shelf() const;

 private:
  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
