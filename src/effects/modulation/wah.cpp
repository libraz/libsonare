#include "effects/modulation/wah.h"

#include <algorithm>
#include <cmath>

#include "effects/common/field_domain.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

using common::FieldRange;
using common::in_domain;
using common::kNoUpperBound;
using common::require_in_domain;

using constants::kDefaultDawSampleRate;

namespace {
// Field domains shared by validate_config and live automation.
constexpr FieldRange kRateHz{0.0f, kNoUpperBound};
constexpr FieldRange kSweepHz{kWahMinSweepHz, kNoUpperBound};
constexpr FieldRange kResonance{kWahMinResonance, kNoUpperBound};
constexpr FieldRange kDryWet{0.0f, 1.0f};
}  // namespace

Wah::Wah(WahConfig config) : config_(config) { validate_config(config_); }

void Wah::validate_config(const WahConfig& config) {
  require_in_domain("wah", "rate_hz", config.rate_hz, kRateHz);
  require_in_domain("wah", "min_hz", config.min_hz, kSweepHz);
  require_in_domain("wah", "max_hz", config.max_hz, kSweepHz);
  require_in_domain("wah", "resonance", config.resonance, kResonance);
  require_in_domain("wah", "dry_wet", config.dry_wet, kDryWet);
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
  const float wet = config_.dry_wet;
  const float dry = 1.0f - wet;
  // The sweep range is the filter's own cutoff ceiling, so it never claims a frequency the
  // filter would silently reduce.
  const float max_cutoff = SvfBandpass::max_cutoff_hz(sample_rate_);
  const float lo = std::clamp(std::min(config_.min_hz, config_.max_hz), kWahMinSweepHz, max_cutoff);
  const float hi = std::clamp(std::max(config_.min_hz, config_.max_hz), lo, max_cutoff);
  const float q = config_.resonance;
  const bool lowpass = config_.filter_type == WahFilterType::kLowpass;
  const WahSweepLaw law = config_.sweep_law;
  // Stereo-pair processor: one bandpass filter per plane is allocated for two
  // planes only, so planes beyond the pair pass through dry (see the registry's
  // stereoPairOnly classification).
  const int active = std::min(num_channels, static_cast<int>(filters_.size()));
  for (size_t ch = 0; ch < filters_.size(); ++ch) {
    filter_gates_[ch].admit(static_cast<int>(ch) < active && channels[ch] != nullptr,
                            [this, ch] { filters_[ch].reset(); });
  }
  for (int i = 0; i < num_samples; ++i) {
    // LFO in [-1, 1] -> a [0, 1] sweep position -> centre frequency.
    const float sweep = 0.5f * (lfo_.process() + 1.0f);
    const float fc = wah_sweep_hz(law, lo, hi, sweep);
    const SvfBandpass::Coefficients coeffs = filters_[0].coefficients(fc, q);
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      channels[ch][i] = dry * in + wet * filters_[ch].tick(in, coeffs, lowpass);
    }
  }
  bool discarded = false;
  for (auto& filter : filters_) discarded |= filter.discard_non_finite();
  if (discarded) {
    for (auto& gate : filter_gates_) gate.close();
    note_non_finite_discard();
  }
}

int Wah::tail_samples() const noexcept {
  rt::TailBudget tail;
  // Near Nyquist the upper sweep endpoint can have the longer ring.
  if (config_.dry_wet > 0.0f) {
    tail = SvfBandpass::ring_range(config_.min_hz, config_.max_hz, config_.resonance, sample_rate_);
  }
  return tail.samples();
}

void Wah::reset() {
  lfo_.reset(0.0);
  for (auto& filter : filters_) {
    filter.reset();
  }
  for (auto& gate : filter_gates_) gate.close();
}

bool Wah::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!in_domain(value, kRateHz)) return false;
      config_.rate_hz = value;
      lfo_.set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      if (!in_domain(value, kSweepHz)) return false;
      config_.min_hz = value;
      return true;
    case 2:
      if (!in_domain(value, kSweepHz)) return false;
      config_.max_hz = value;
      return true;
    case 3:
      if (!in_domain(value, kResonance)) return false;
      config_.resonance = value;
      return true;
    case 4:
      if (!in_domain(value, kDryWet)) return false;
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
