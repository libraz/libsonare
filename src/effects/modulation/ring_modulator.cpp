#include "effects/modulation/ring_modulator.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

RingModulator::RingModulator(RingModulatorConfig config) : config_(config) {
  config_.carrier_hz = std::max(0.0f, config_.carrier_hz);
}

void RingModulator::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  reset();
}

void RingModulator::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const double increment = static_cast<double>(config_.carrier_hz) / sample_rate_;
  double phase = phase_;
  for (int i = 0; i < num_samples; ++i) {
    const float carrier = static_cast<float>(std::sin(phase * ::sonare::constants::kTwoPiD));
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * in * carrier;
    }
    phase += increment;
    phase -= std::floor(phase);
  }
  phase_ = phase;
}

void RingModulator::reset() { phase_ = 0.0; }

bool RingModulator::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.carrier_hz = std::max(0.0f, value);
      return true;
    case 1:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> RingModulator::parameter_descriptors() const {
  return {{"carrierHz", 0}, {"dryWet", 1}};
}

}  // namespace sonare::effects::modulation
