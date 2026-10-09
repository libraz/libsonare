#include "effects/modulation/wah.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

using constants::kDefaultDawSampleRate;

Wah::Wah(WahConfig config) : config_(config) {
  config_.rate_hz = std::max(0.0f, config_.rate_hz);
  config_.resonance = std::max(0.5f, config_.resonance);
}

void Wah::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
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
  // The sweep range is the filter's own cutoff ceiling, so it never claims a frequency the
  // filter would silently reduce.
  const float max_cutoff = SvfBandpass::max_cutoff_hz(sample_rate_);
  const float lo = std::clamp(std::min(config_.min_hz, config_.max_hz), 10.0f, max_cutoff);
  const float hi = std::clamp(std::max(config_.min_hz, config_.max_hz), lo, max_cutoff);
  const float q = std::max(0.5f, config_.resonance);
  const bool lowpass = config_.filter_type == WahFilterType::kLowpass;
  const WahSweepLaw law = config_.sweep_law;
  // Stereo-pair processor: one bandpass filter per plane is allocated for two
  // planes only, so planes beyond the pair pass through dry (see the registry's
  // stereoPairOnly classification).
  const int active = std::min(num_channels, static_cast<int>(filters_.size()));
  for (int i = 0; i < num_samples; ++i) {
    // LFO in [-1, 1] -> a [0, 1] sweep position -> centre frequency.
    const float sweep = 0.5f * (lfo_.process() + 1.0f);
    const float fc = wah_sweep_hz(law, lo, hi, sweep);
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * filters_[ch].process(in, fc, q, lowpass);
    }
  }
  bool discarded = false;
  for (auto& filter : filters_) discarded |= filter.discard_non_finite();
  if (discarded) note_non_finite_discard();
}

int Wah::tail_samples() const noexcept {
  rt::TailBudget tail;
  // The sweep's lowest corner rings longest at a given resonance.
  if (std::clamp(config_.dry_wet, 0.0f, 1.0f) > 0.0f) {
    tail = SvfBandpass::ring(std::min(config_.min_hz, config_.max_hz), config_.resonance,
                             sample_rate_);
  }
  return tail.samples();
}

void Wah::reset() {
  lfo_.reset(0.0);
  for (auto& filter : filters_) {
    filter.reset();
  }
}

bool Wah::set_parameter_impl(unsigned int param_id, float value) {
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
    case 5:
    case 6: {
      // An unnamed value is refused rather than rounded onto a neighbour.
      const int count = param_id == 5 ? kWahFilterTypeCount : kWahSweepLawCount;
      if (value < 0.0f || value != std::floor(value) || value >= static_cast<float>(count)) {
        return false;
      }
      if (param_id == 5) {
        config_.filter_type = static_cast<WahFilterType>(static_cast<int>(value));
      } else {
        config_.sweep_law = static_cast<WahSweepLaw>(static_cast<int>(value));
      }
      return true;
    }
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Wah::parameter_descriptors() const {
  return {{"rateHz", 0}, {"minHz", 1},      {"maxHz", 2},   {"resonance", 3},
          {"dryWet", 4}, {"filterType", 5}, {"sweepLaw", 6}};
}

}  // namespace sonare::effects::modulation
