#include "mastering/eq/cut_filter.h"

#include <algorithm>

namespace sonare::mastering::eq {
namespace {

float clamp_frequency(float frequency_hz, double sample_rate) {
  return std::clamp(frequency_hz, 1.0e-3f, static_cast<float>(sample_rate * 0.5) - 1.0e-3f);
}

}  // namespace

void CutFilter::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
  apply_high_pass();
  apply_low_pass();
}

void CutFilter::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void CutFilter::reset() { eq_.reset(); }

void CutFilter::set_high_pass(float frequency_hz, float q, CutFilterSlope slope, bool enabled) {
  high_pass_ = {EqBandType::HighPass, frequency_hz, 0.0f, q, enabled};
  high_pass_slope_ = slope;
  apply_high_pass();
}

void CutFilter::set_low_pass(float frequency_hz, float q, CutFilterSlope slope, bool enabled) {
  low_pass_ = {EqBandType::LowPass, frequency_hz, 0.0f, q, enabled};
  low_pass_slope_ = slope;
  apply_low_pass();
}

void CutFilter::clear_high_pass() {
  high_pass_.enabled = false;
  apply_high_pass();
}

void CutFilter::clear_low_pass() {
  low_pass_.enabled = false;
  apply_low_pass();
}

void CutFilter::clear() {
  high_pass_.enabled = false;
  low_pass_.enabled = false;
  eq_.clear();
}

bool CutFilter::set_parameter(unsigned int param_id, float value) {
  const double sample_rate = eq_.sample_rate();
  switch (param_id) {
    case 0:
      high_pass_.frequency_hz = clamp_frequency(value, sample_rate);
      apply_high_pass();
      return true;
    case 1:
      high_pass_.q = std::max(value, 1.0e-6f);
      apply_high_pass();
      return true;
    case 2:
      low_pass_.frequency_hz = clamp_frequency(value, sample_rate);
      apply_low_pass();
      return true;
    case 3:
      low_pass_.q = std::max(value, 1.0e-6f);
      apply_low_pass();
      return true;
    default:
      return false;
  }
}

void CutFilter::apply_high_pass() {
  if (high_pass_slope_ == CutFilterSlope::Db24PerOct && high_pass_.enabled) {
    EqBand stage_a = high_pass_;
    EqBand stage_b = high_pass_;
    stage_a.q = 0.54119610f;
    stage_b.q = 1.30656296f;
    eq_.set_band(0, stage_a);
    eq_.set_band(1, stage_b);
  } else {
    eq_.set_band(0, high_pass_);
    eq_.clear_band(1);
  }
}

void CutFilter::apply_low_pass() {
  if (low_pass_slope_ == CutFilterSlope::Db24PerOct && low_pass_.enabled) {
    EqBand stage_a = low_pass_;
    EqBand stage_b = low_pass_;
    stage_a.q = 0.54119610f;
    stage_b.q = 1.30656296f;
    eq_.set_band(2, stage_a);
    eq_.set_band(3, stage_b);
  } else {
    eq_.set_band(2, low_pass_);
    eq_.clear_band(3);
  }
}

}  // namespace sonare::mastering::eq
