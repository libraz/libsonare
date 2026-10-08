#include "effects/modulation/auto_wah.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/dsp_primitives.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

AutoWah::AutoWah(AutoWahConfig config) : config_(config) {
  config_.resonance = std::max(0.5f, config_.resonance);
  config_.lfo_rate_hz = std::max(0.0f, config_.lfo_rate_hz);
  config_.lfo_depth = std::clamp(config_.lfo_depth, 0.0f, 1.0f);
}

void AutoWah::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  for (auto& filter : filters_) {
    filter.prepare(sample_rate_);
  }
  lfo_.prepare(sample_rate_);
  lfo_.set_rate_hz(config_.lfo_rate_hz);
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
  const float max_cutoff = SvfBandpass::max_cutoff_hz(sample_rate_);
  const float lo = std::clamp(std::min(config_.min_hz, config_.max_hz), 10.0f, max_cutoff);
  const float hi = std::clamp(std::max(config_.min_hz, config_.max_hz), lo, max_cutoff);
  const float q = std::max(0.5f, config_.resonance);
  const float sens = std::max(0.0f, config_.sensitivity);
  const float lfo_depth = config_.lfo_depth;
  const bool lowpass = config_.filter_type == WahFilterType::kLowpass;
  const bool down = config_.direction == AutoWahDirection::kDown;
  const WahSweepLaw law = config_.sweep_law;
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
    float position = envelope_ * sens;
    // The LFO adds a unipolar 0..depth swing to the envelope's position.
    if (lfo_depth > 0.0f) position += lfo_depth * 0.5f * (lfo_.process() + 1.0f);
    float open = std::clamp(position, 0.0f, 1.0f);
    if (down) open = 1.0f - open;
    const float fc = wah_sweep_hz(law, lo, hi, open);
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * filters_[ch].process(in, fc, q, lowpass);
    }
  }
  discard_non_finite();
}

void AutoWah::discard_non_finite() noexcept {
  // The follower rests at silence: it is a rectified level, and a sweep opened
  // from anything else would be a filter position no input asked for.
  bool discarded = discard_if_non_finite(envelope_, 0.0f);
  for (auto& filter : filters_) discarded |= filter.discard_non_finite();
  if (discarded) note_non_finite_discard();
}

int AutoWah::tail_samples() const noexcept {
  rt::TailBudget tail;
  // The sweep's lowest corner rings longest at a given resonance.
  if (std::clamp(config_.dry_wet, 0.0f, 1.0f) > 0.0f) {
    tail = SvfBandpass::ring(std::min(config_.min_hz, config_.max_hz), config_.resonance,
                             sample_rate_);
  }
  return tail.samples();
}

void AutoWah::reset() {
  envelope_ = 0.0f;
  lfo_.reset(0.0);
  for (auto& filter : filters_) {
    filter.reset();
  }
}

bool AutoWah::set_parameter_impl(unsigned int param_id, float value) {
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
    case 5:
      config_.lfo_rate_hz = std::max(0.0f, value);
      lfo_.set_rate_hz(config_.lfo_rate_hz);
      return true;
    case 6:
      config_.lfo_depth = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 7:
    case 8:
    case 9: {
      // An unnamed value is refused rather than rounded onto a neighbour.
      const int count = param_id == 7   ? kWahFilterTypeCount
                        : param_id == 8 ? kAutoWahDirectionCount
                                        : kWahSweepLawCount;
      if (value < 0.0f || value != std::floor(value) || value >= static_cast<float>(count)) {
        return false;
      }
      const int choice = static_cast<int>(value);
      if (param_id == 7) {
        config_.filter_type = static_cast<WahFilterType>(choice);
      } else if (param_id == 8) {
        config_.direction = static_cast<AutoWahDirection>(choice);
      } else {
        config_.sweep_law = static_cast<WahSweepLaw>(choice);
      }
      return true;
    }
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> AutoWah::parameter_descriptors() const {
  return {{"sensitivity", 0}, {"minHz", 1},     {"maxHz", 2},    {"resonance", 3},
          {"dryWet", 4},      {"lfoRateHz", 5}, {"lfoDepth", 6}, {"filterType", 7},
          {"direction", 8},   {"sweepLaw", 9}};
}

}  // namespace sonare::effects::modulation
