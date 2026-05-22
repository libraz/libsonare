#pragma once

/// @file api_style.h
/// @brief API 550 style stepped proportional-Q equalizer.

#include <array>
#include <cstddef>

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class ApiStyleEq : public common::ProcessorBase {
 public:
  enum class Band {
    Low = 0,
    LowMid = 1,
    HighMid = 2,
    High = 3,
  };

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_band(Band band, float frequency_hz, float gain_db);
  void clear_band(Band band);
  void clear();

  float frequency(Band band) const;
  float gain_db(Band band) const;
  float snapped_frequency(Band band, float requested_frequency_hz) const;
  float snapped_gain(float requested_gain_db) const;

 private:
  struct BandState {
    float frequency_hz = 1000.0f;
    float gain_db = 0.0f;
    bool enabled = false;
  };

  void rebuild_band(Band band);
  static size_t index(Band band);
  static float nearest(float value, const float* values, size_t count);
  static float proportional_q(float gain_db);

  ParametricEq eq_;
  std::array<BandState, 4> bands_{};
};

}  // namespace sonare::mastering::eq
