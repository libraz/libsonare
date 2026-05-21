#pragma once

/// @file mid_side_eq.h
/// @brief Independent mid/side parametric equalizer.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class MidSideEq : public common::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_mid_band(size_t index, const EqBand& band);
  void set_side_band(size_t index, const EqBand& band);
  void clear_mid_band(size_t index);
  void clear_side_band(size_t index);
  void clear();

  const EqBand& mid_band(size_t index) const;
  const EqBand& side_band(size_t index) const;

 private:
  ParametricEq mid_eq_;
  ParametricEq side_eq_;
};

}  // namespace sonare::mastering::eq
