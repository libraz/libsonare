#pragma once

/// @file graphic_eq.h
/// @brief 31-band graphic equalizer.

#include <array>
#include <cstddef>

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class GraphicEq : public common::ProcessorBase {
 public:
  static constexpr size_t kNumBands = 31;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_gain_db(size_t index, float gain_db);
  void set_gain_for_frequency(float frequency_hz, float gain_db);
  void clear();

  float gain_db(size_t index) const;
  float center_frequency(size_t index) const;
  size_t nearest_band(float frequency_hz) const;

 private:
  void rebuild_bands();
  static void validate_index(size_t index);

  ParametricEq low_eq_;
  ParametricEq high_eq_;
  std::array<float, kNumBands> gains_db_{};
};

}  // namespace sonare::mastering::eq
