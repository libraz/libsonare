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

  // Automatable parameters (RT-safe: recomputes affected biquad coefficients in
  // place, preserves filter state). The band-pass (slot 0) and notch (slot 1)
  // each expose frequency and Q:
  //   0 = band-pass frequency_hz (clamped to (0 Hz, Nyquist))
  //   1 = band-pass Q (clamped to > 0)
  //   2 = notch frequency_hz (clamped to (0 Hz, Nyquist))
  //   3 = notch Q (clamped to > 0)
  bool set_parameter(unsigned int param_id, float value) override;

  const EqBand& band_pass() const;
  const EqBand& notch() const;

 private:
  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
