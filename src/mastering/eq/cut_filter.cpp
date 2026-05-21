#include "mastering/eq/cut_filter.h"

namespace sonare::mastering::eq {

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
