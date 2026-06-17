#include "mastering/eq/minimum_phase.h"

namespace sonare::mastering::eq {

void MinimumPhaseEq::prepare(double sample_rate, int max_block_size) {
  eq_.prepare(sample_rate, max_block_size);
}

void MinimumPhaseEq::process(float* const* channels, int num_channels, int num_samples) {
  eq_.process(channels, num_channels, num_samples);
}

void MinimumPhaseEq::reset() { eq_.reset(); }

void MinimumPhaseEq::prepare_channels(int num_channels) { eq_.prepare_channels(num_channels); }

void MinimumPhaseEq::set_band(size_t index, const EqBand& band) {
  eq_.set_band(index, natural_band(band));
}

void MinimumPhaseEq::clear_band(size_t index) { eq_.clear_band(index); }

void MinimumPhaseEq::clear() { eq_.clear(); }

const EqBand& MinimumPhaseEq::band(size_t index) const { return eq_.band(index); }

std::vector<rt::ParamDescriptor> MinimumPhaseEq::parameter_descriptors() const {
  // Delegates to the same block-of-3 layout as ParametricEq (set_parameter
  // forwards to eq_); keys mirror the construction-time prefix: band<b>.<field>.
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(kMaxBands * 3u);
  for (unsigned int b = 0; b < kMaxBands; ++b) {
    const std::string prefix = "band" + std::to_string(b) + ".";
    descriptors.push_back({prefix + "frequencyHz", b * 3u + 0u});
    descriptors.push_back({prefix + "gainDb", b * 3u + 1u});
    descriptors.push_back({prefix + "q", b * 3u + 2u});
  }
  return descriptors;
}

EqBand MinimumPhaseEq::natural_band(EqBand band) {
  band.coeff_mode = BiquadCoeffMode::Vicanek;
  band.phase = PhaseMode::NaturalPhase;
  return band;
}

}  // namespace sonare::mastering::eq
