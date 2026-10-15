#include "effects/modulation/ring_modulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::effects::modulation {

using constants::kDefaultDawSampleRate;
using constants::kDegreesPerTurnD;

namespace {

constexpr float kMaxPhaseDeg = 360.0f;
constexpr double kAntiphaseTurns = 0.5;
constexpr float kInf = std::numeric_limits<float>::infinity();

/// One validated field: its automation id and the closed interval a value must lie in.
struct FieldDomain {
  const char* name;
  float RingModulatorConfig::*member;
  unsigned int param_id;
  float lo;
  float hi;
};

// The single definition of each field's domain, shared by validate_config and live automation.
constexpr FieldDomain kFields[] = {
    {"carrierHz", &RingModulatorConfig::carrier_hz, 0, 0.0f, kInf},
    {"dryWet", &RingModulatorConfig::dry_wet, 1, 0.0f, 1.0f},
    {"phaseDeg", &RingModulatorConfig::phase_deg, 3, 0.0f, kMaxPhaseDeg},
    {"stereoSpread", &RingModulatorConfig::stereo_spread, 4, 0.0f, 1.0f},
};

bool in_domain(const FieldDomain& field, float value) noexcept {
  return std::isfinite(value) && value >= field.lo && value <= field.hi;
}

const FieldDomain* find_field(unsigned int param_id) noexcept {
  for (const FieldDomain& field : kFields) {
    if (field.param_id == param_id) return &field;
  }
  return nullptr;
}

}  // namespace

RingModulator::RingModulator(RingModulatorConfig config) : config_(config) {
  validate_config(config_);
}

void RingModulator::validate_config(const RingModulatorConfig& config) {
  for (const FieldDomain& field : kFields) {
    const float value = config.*field.member;
    if (in_domain(field, value)) continue;
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("ring modulator: ") + field.name + " out of range [" +
                              util::to_text(field.lo) + ", " + util::to_text(field.hi) + "], got " +
                              util::to_text(value));
  }
}

void RingModulator::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  reset();
}

void RingModulator::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const common::MixGains mix =
      common::mix_gains(config_.mix_law, std::clamp(config_.dry_wet, 0.0f, 1.0f));
  const float wet = mix.wet;
  const float dry = mix.dry;
  const double increment = static_cast<double>(config_.carrier_hz) / sample_rate_;
  const LfoShape shape = config_.shape;
  // Right carrier's lead over the left, in turns; zero keeps one shared carrier.
  const double offset =
      static_cast<double>(config_.phase_deg) / kDegreesPerTurnD +
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

bool RingModulator::set_parameter_impl(unsigned int param_id, float value) {
  const FieldDomain* field = find_field(param_id);
  if (field != nullptr && !in_domain(*field, value)) return false;
  switch (param_id) {
    case 0:
      config_.carrier_hz = value;
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
      config_.phase_deg = value;
      return true;
    case 4:
      config_.stereo_spread = value;
      return true;
    case 5:
      return common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> RingModulator::parameter_descriptors() const {
  return {{"carrierHz", 0}, {"dryWet", 1},       {"shape", 2},
          {"phaseDeg", 3},  {"stereoSpread", 4}, {"mixLaw", 5}};
}

}  // namespace sonare::effects::modulation
