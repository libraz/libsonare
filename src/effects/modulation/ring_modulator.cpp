#include "effects/modulation/ring_modulator.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"

namespace sonare::effects::modulation {

namespace {

constexpr double kDegreesPerTurn = 360.0;
constexpr float kMaxPhaseDeg = 360.0f;
constexpr double kAntiphaseTurns = 0.5;

}  // namespace

RingModulator::RingModulator(RingModulatorConfig config) : config_(config) {
  config_.carrier_hz = std::max(0.0f, config_.carrier_hz);
  config_.phase_deg = std::clamp(config_.phase_deg, 0.0f, kMaxPhaseDeg);
  config_.stereo_spread = std::clamp(config_.stereo_spread, 0.0f, 1.0f);
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
  const LfoShape shape = config_.shape;
  // Right carrier's lead over the left, in turns; zero keeps one shared carrier.
  const double offset =
      static_cast<double>(config_.phase_deg) / kDegreesPerTurn +
      kAntiphaseTurns * static_cast<double>(std::clamp(config_.stereo_spread, 0.0f, 1.0f));
  double phase = phase_;
  for (int i = 0; i < num_samples; ++i) {
    const float carrier = lfo_shape_value(shape, phase);
    const float carrier_right = offset != 0.0 ? lfo_shape_value(shape, phase + offset) : carrier;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * in * (ch == 0 ? carrier : carrier_right);
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
    case 2:
      // An unnamed value is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kLfoShapeCount)) {
        return false;
      }
      config_.shape = static_cast<LfoShape>(static_cast<int>(value));
      return true;
    case 3:
      config_.phase_deg = std::clamp(value, 0.0f, kMaxPhaseDeg);
      return true;
    case 4:
      config_.stereo_spread = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> RingModulator::parameter_descriptors() const {
  return {{"carrierHz", 0}, {"dryWet", 1}, {"shape", 2}, {"phaseDeg", 3}, {"stereoSpread", 4}};
}

}  // namespace sonare::effects::modulation
