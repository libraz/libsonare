#pragma once

/// @file cut_filter.h
/// @brief Dedicated high-pass and low-pass cut filters.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"
#include "util/constants.h"

namespace sonare::mastering::eq {

enum class CutFilterSlope {
  Db12PerOct,
  Db24PerOct,
};

class CutFilter : public common::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_high_pass(float frequency_hz, float q = sonare::constants::kButterworthQ,
                     CutFilterSlope slope = CutFilterSlope::Db12PerOct, bool enabled = true);
  void set_low_pass(float frequency_hz, float q = sonare::constants::kButterworthQ,
                    CutFilterSlope slope = CutFilterSlope::Db12PerOct, bool enabled = true);
  void clear_high_pass();
  void clear_low_pass();
  void clear();

  CutFilterSlope high_pass_slope() const { return high_pass_slope_; }
  CutFilterSlope low_pass_slope() const { return low_pass_slope_; }

  // Automatable parameters (RT-safe: recomputes affected biquad coefficients in
  // place via apply_high_pass()/apply_low_pass(), preserves filter state):
  //   0 = high-pass frequency_hz (clamped to (0 Hz, Nyquist))
  //   1 = high-pass Q (clamped to > 0; only audible at 12 dB/oct, the 24 dB/oct
  //       cascade uses fixed Butterworth-stage Q values)
  //   2 = low-pass frequency_hz (clamped to (0 Hz, Nyquist))
  //   3 = low-pass Q (clamped to > 0; same 24 dB/oct caveat as id 1)
  // The slope enum is not automatable.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  void apply_high_pass();
  void apply_low_pass();

  ParametricEq eq_;
  EqBand high_pass_{EqBandType::HighPass, 20.0f, 0.0f, sonare::constants::kButterworthQ, false};
  EqBand low_pass_{EqBandType::LowPass, 20000.0f, 0.0f, sonare::constants::kButterworthQ, false};
  CutFilterSlope high_pass_slope_ = CutFilterSlope::Db12PerOct;
  CutFilterSlope low_pass_slope_ = CutFilterSlope::Db12PerOct;
};

}  // namespace sonare::mastering::eq
