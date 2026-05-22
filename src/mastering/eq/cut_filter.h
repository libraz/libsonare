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
