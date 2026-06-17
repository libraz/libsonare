#include "mastering/eq/shelving.h"

namespace sonare::mastering::eq {

void ShelvingEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
}

void ShelvingEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void ShelvingEq::reset() { eq_.reset(); }

std::vector<rt::ParamDescriptor> ShelvingEq::parameter_descriptors() const {
  return {{"lowFrequencyHz", 0},  {"lowGainDb", 1},  {"lowQ", 2},
          {"highFrequencyHz", 3}, {"highGainDb", 4}, {"highQ", 5}};
}

void ShelvingEq::set_low_shelf(float frequency_hz, float gain_db, float q, bool enabled) {
  eq_.set_band(0, {EqBandType::LowShelf, frequency_hz, gain_db, q, enabled});
}

void ShelvingEq::set_high_shelf(float frequency_hz, float gain_db, float q, bool enabled) {
  eq_.set_band(1, {EqBandType::HighShelf, frequency_hz, gain_db, q, enabled});
}

void ShelvingEq::clear_low_shelf() { eq_.clear_band(0); }

void ShelvingEq::clear_high_shelf() { eq_.clear_band(1); }

void ShelvingEq::clear() { eq_.clear(); }

const EqBand& ShelvingEq::low_shelf() const { return eq_.band(0); }

const EqBand& ShelvingEq::high_shelf() const { return eq_.band(1); }

}  // namespace sonare::mastering::eq
