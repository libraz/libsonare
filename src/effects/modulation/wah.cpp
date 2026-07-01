#include "effects/modulation/wah.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"

namespace sonare::effects::modulation {

Wah::Wah(WahConfig config) : config_(config) {
  config_.rate_hz = std::max(0.0f, config_.rate_hz);
  config_.resonance = std::max(0.5f, config_.resonance);
}

void Wah::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  lfo_.prepare(sample_rate_);
  lfo_.set_rate_hz(config_.rate_hz);
  for (auto& filter : filters_) {
    filter.prepare(sample_rate_);
  }
  reset();
}

void Wah::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float lo = std::min(config_.min_hz, config_.max_hz);
  const float hi = std::max(config_.min_hz, config_.max_hz);
  const float q = std::max(0.5f, config_.resonance);
  const int active = std::min(num_channels, static_cast<int>(filters_.size()));
  for (int i = 0; i < num_samples; ++i) {
    // LFO in [-1, 1] -> a [0, 1] sweep position -> centre frequency.
    const float sweep = 0.5f * (lfo_.process() + 1.0f);
    const float fc = lo + (hi - lo) * sweep;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * filters_[ch].process(in, fc, q);
    }
  }
}

void Wah::reset() {
  lfo_.reset(0.0);
  for (auto& filter : filters_) {
    filter.reset();
  }
}

bool Wah::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      lfo_.set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      config_.min_hz = value;
      return true;
    case 2:
      config_.max_hz = value;
      return true;
    case 3:
      config_.resonance = std::max(0.5f, value);
      return true;
    case 4:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Wah::parameter_descriptors() const {
  return {{"rateHz", 0}, {"minHz", 1}, {"maxHz", 2}, {"resonance", 3}, {"dryWet", 4}};
}

}  // namespace sonare::effects::modulation
