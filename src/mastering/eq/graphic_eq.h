#pragma once

/// @file graphic_eq.h
/// @brief 31-band graphic equalizer.

#include <array>
#include <cstddef>

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

class GraphicEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kNumBands = 31;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_gain_db(size_t index, float gain_db);
  void set_gain_for_frequency(float frequency_hz, float gain_db);
  void clear();

  // Automatable parameters (RT-safe: recomputes only the affected band's biquad
  // coefficients in place, preserves filter state). Each of the 31 ISO bands
  // exposes a single gain control; param_id is the band index:
  //   id b (0 .. kNumBands-1) = gain_db for band b (center frequency is fixed;
  //                             band Q is derived from gain via
  //                             band_q_for_gain_db()).
  bool set_parameter(unsigned int param_id, float value) override;

  float gain_db(size_t index) const;
  float center_frequency(size_t index) const;
  size_t nearest_band(float frequency_hz) const;
  static float band_q_for_gain_db(float gain_db);

 private:
  void rebuild_bands();
  // Recomputes a single band's coefficients in place (no state reset).
  void rebuild_band(size_t index);
  static void validate_index(size_t index);

  ParametricEq low_eq_;
  ParametricEq high_eq_;
  std::array<float, kNumBands> gains_db_{};
  // Captured in prepare(); used to clamp fixed ISO center frequencies to the
  // open (0 Hz, Nyquist) interval so bands above Nyquist do not throw.
  double sample_rate_ = 44100.0;
};

}  // namespace sonare::mastering::eq
