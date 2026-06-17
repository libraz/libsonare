#include "mastering/eq/band_pass.h"

namespace sonare::mastering::eq {

void BandPassEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
}

void BandPassEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void BandPassEq::reset() { eq_.reset(); }

void BandPassEq::set_band_pass(float frequency_hz, float q, bool enabled) {
  eq_.set_band(0, {EqBandType::BandPass, frequency_hz, 0.0f, q, enabled});
}

void BandPassEq::set_notch(float frequency_hz, float q, bool enabled) {
  eq_.set_band(1, {EqBandType::Notch, frequency_hz, 0.0f, q, enabled});
}

bool BandPassEq::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      return eq_.set_parameter(0, value);  // band-pass frequency
    case 1:
      return eq_.set_parameter(2, value);  // band-pass Q
    case 2:
      return eq_.set_parameter(3, value);  // notch frequency
    case 3:
      return eq_.set_parameter(5, value);  // notch Q
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> BandPassEq::parameter_descriptors() const {
  return {{"bandPassFrequencyHz", 0}, {"bandPassQ", 1}, {"notchFrequencyHz", 2}, {"notchQ", 3}};
}

void BandPassEq::clear_band_pass() { eq_.clear_band(0); }

void BandPassEq::clear_notch() { eq_.clear_band(1); }

void BandPassEq::clear() { eq_.clear(); }

const EqBand& BandPassEq::band_pass() const { return eq_.band(0); }

const EqBand& BandPassEq::notch() const { return eq_.band(1); }

}  // namespace sonare::mastering::eq
