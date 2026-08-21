#include "effects/modulation/auto_wah.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/dsp_primitives.h"

namespace sonare::effects::modulation {

AutoWah::AutoWah(AutoWahConfig config) : config_(config) {
  config_.resonance = std::max(0.5f, config_.resonance);
}

void AutoWah::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  for (auto& filter : filters_) {
    filter.prepare(sample_rate_);
  }
  update_coeffs();
  reset();
}

void AutoWah::update_coeffs() {
  attack_coeff_ = time_to_coefficient(sample_rate_, config_.attack_ms);
  release_coeff_ = time_to_coefficient(sample_rate_, config_.release_ms);
}

void AutoWah::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float nyquist = static_cast<float>(0.5 * sample_rate_);
  const float max_cutoff = 0.49f * nyquist;
  const float lo = std::clamp(std::min(config_.min_hz, config_.max_hz), 10.0f, max_cutoff);
  const float hi = std::clamp(std::max(config_.min_hz, config_.max_hz), lo, max_cutoff);
  const float q = std::max(0.5f, config_.resonance);
  const float sens = std::max(0.0f, config_.sensitivity);
  // Stereo-pair processor: only two per-plane filters exist, so planes beyond
  // the pair pass through dry (see the registry's stereoPairOnly classification).
  const int active = std::min(num_channels, static_cast<int>(filters_.size()));
  for (int i = 0; i < num_samples; ++i) {
    // Rectified peak across the active channels drives one shared envelope.
    float peak = 0.0f;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      peak = std::max(peak, std::fabs(channels[ch][i]));
    }
    const float coeff = peak > envelope_ ? attack_coeff_ : release_coeff_;
    envelope_ = peak + coeff * (envelope_ - peak);
    // Envelope (0..~1) scaled by sensitivity maps to the [lo, hi] sweep.
    const float open = std::clamp(envelope_ * sens, 0.0f, 1.0f);
    const float fc = lo + (hi - lo) * open;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * filters_[ch].process(in, fc, q);
    }
  }
}

void AutoWah::reset() {
  envelope_ = 0.0f;
  for (auto& filter : filters_) {
    filter.reset();
  }
}

bool AutoWah::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.sensitivity = value;
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

std::vector<rt::ParamDescriptor> AutoWah::parameter_descriptors() const {
  return {{"sensitivity", 0}, {"minHz", 1}, {"maxHz", 2}, {"resonance", 3}, {"dryWet", 4}};
}

}  // namespace sonare::effects::modulation
