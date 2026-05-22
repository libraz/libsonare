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

  void set_low_shelf(float frequency_hz, float gain_db,
                     float q = sonare::constants::kButterworthQ, bool enabled = true);
  void set_high_shelf(float frequency_hz, float gain_db,
                      float q = sonare::constants::kButterworthQ, bool enabled = true);
  void clear_low_shelf();
  void clear_high_shelf();
  void clear();

  const EqBand& low_shelf() const;
  const EqBand& high_shelf() const;

 private:
  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
