#pragma once

/// @file api_style.h
/// @brief Stepped proportional-Q equalizer.

#include <array>
#include <cstddef>
#include <vector>

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

class ApiStyleEq : public rt::ProcessorBase {
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

  // Automatable parameters (RT-safe: recomputes the affected band's biquad
  // coefficients in place via rebuild_band(), preserves filter state). The four
  // bands (Low=0, LowMid=1, HighMid=2, High=3) each expose frequency and gain
  // in a block of 2 (band b -> ids 2*b, 2*b+1):
  //   2*b + 0 = frequency_hz (snapped to the band's stepped frequency table)
  //   2*b + 1 = gain_db (snapped to the 2 dB gain steps; enables the band when
  //             non-zero, matching set_band())
  // Values are snapped exactly as set_band() does, preserving the stepped
  // proportional-Q character; Q is still derived from gain via proportional_q().
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=lowFrequencyHz, 1=lowGainDb, 2=lowMidFrequencyHz,
  // 3=lowMidGainDb, 4=highMidFrequencyHz, 5=highMidGainDb, 6=highFrequencyHz,
  // 7=highGainDb.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

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
