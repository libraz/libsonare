#include "mastering/eq/minimum_phase.h"

namespace sonare::mastering::eq {

void MinimumPhaseEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
}

void MinimumPhaseEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void MinimumPhaseEq::reset() { eq_.reset(); }

void MinimumPhaseEq::set_band(size_t index, const EqBand& band) { eq_.set_band(index, band); }

void MinimumPhaseEq::clear_band(size_t index) { eq_.clear_band(index); }

void MinimumPhaseEq::clear() { eq_.clear(); }

const EqBand& MinimumPhaseEq::band(size_t index) const { return eq_.band(index); }

}  // namespace sonare::mastering::eq
