#include "mastering/spectral/spectral_shaper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::constants::kPiD;

float one_pole_alpha(float frequency_hz, double sample_rate) {
  return std::clamp(
      static_cast<float>(2.0 * kPiD * frequency_hz / (2.0 * kPiD * frequency_hz + sample_rate)),
      0.0f, 1.0f);
}

float smoothing_coeff(double sample_rate, float time_ms) {
  if (time_ms <= 0.0f) return 1.0f;
  if (sample_rate <= 0.0) return 0.0f;
  return 1.0f - time_to_coefficient(sample_rate, time_ms);
}

}  // namespace

SpectralShaper::SpectralShaper(SpectralShaperConfig config) : config_(config) {
  validate_config(config_);
}

void SpectralShaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  for (auto& envelope : envelopes_)
    envelope.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  prepared_ = true;
  reset();
}

void SpectralShaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) throw std::logic_error("SpectralShaper must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  if (low_state_.size() != static_cast<size_t>(num_channels)) {
    low_state_.assign(static_cast<size_t>(num_channels), 0.0f);
    band_low_state_.assign(static_cast<size_t>(num_channels), 0.0f);
    gain_state_.assign(static_cast<size_t>(num_channels), 1.0f);
    envelopes_.assign(static_cast<size_t>(num_channels), {});
    for (auto& envelope : envelopes_) {
      envelope.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }

  const float low_alpha = one_pole_alpha(config_.frequency_hz, sample_rate_);
  const float high_alpha = one_pole_alpha(config_.high_frequency_hz, sample_rate_);
  const float attack_coeff = smoothing_coeff(sample_rate_, config_.attack_ms);
  const float release_coeff = smoothing_coeff(sample_rate_, config_.release_ms);
  float min_gain = 1.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    float low = low_state_[static_cast<size_t>(ch)];
    float band_low = band_low_state_[static_cast<size_t>(ch)];
    float gain_state = gain_state_[static_cast<size_t>(ch)];
    auto& envelope = envelopes_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float dry = channels[ch][i];
      low += low_alpha * (dry - low);
      const float high_passed = dry - low;
      band_low += high_alpha * (high_passed - band_low);
      const float target_band = band_low;
      const float remainder = dry - target_band;

      const float level = envelope.process(target_band);
      float target_gain = 1.0f;
      if (level > config_.threshold) {
        const float over = (level - config_.threshold) / std::max(level, 1e-6f);
        const float proportional_gain = std::clamp(1.0f - over * config_.amount, 0.0f, 1.0f);
        target_gain = std::max(proportional_gain, db_to_linear(-config_.range_db));
      }
      const float coeff = target_gain < gain_state ? attack_coeff : release_coeff;
      gain_state += coeff * (target_gain - gain_state);
      min_gain = std::min(min_gain, gain_state);
      channels[ch][i] = remainder + target_band * gain_state;
    }
    low_state_[static_cast<size_t>(ch)] = low;
    band_low_state_[static_cast<size_t>(ch)] = band_low;
    gain_state_[static_cast<size_t>(ch)] = gain_state;
  }
  last_reduction_db_ = linear_to_db(min_gain);
}

void SpectralShaper::reset() {
  std::fill(low_state_.begin(), low_state_.end(), 0.0f);
  std::fill(band_low_state_.begin(), band_low_state_.end(), 0.0f);
  std::fill(gain_state_.begin(), gain_state_.end(), 1.0f);
  for (auto& envelope : envelopes_) envelope.reset();
  last_reduction_db_ = 0.0f;
}

void SpectralShaper::set_config(const SpectralShaperConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& envelope : envelopes_) {
      envelope.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
  }
}

bool SpectralShaper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold = std::max(0.0f, value);
      return true;
    case 1:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      // Keep the validate_config invariant high_frequency_hz > frequency_hz.
      config_.frequency_hz =
          std::clamp(value, 1.0e-3f, std::nextafter(config_.high_frequency_hz, 0.0f));
      return true;
    case 3:
      config_.high_frequency_hz =
          std::max(value, std::nextafter(config_.frequency_hz, config_.frequency_hz + 1.0f));
      return true;
    case 4:
      config_.attack_ms = std::max(0.0f, value);
      // Re-prepare the envelope followers in place; preserves envelope state.
      if (prepared_) {
        for (auto& envelope : envelopes_) {
          envelope.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 5:
      config_.release_ms = std::max(0.0f, value);
      if (prepared_) {
        for (auto& envelope : envelopes_) {
          envelope.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 6:
      config_.range_db = std::max(0.0f, value);
      return true;
    default:
      return false;
  }
}

void SpectralShaper::validate_config(const SpectralShaperConfig& config) {
  if (!(config.threshold >= 0.0f) || !(config.amount >= 0.0f && config.amount <= 1.0f) ||
      !(config.frequency_hz > 0.0f) || !(config.high_frequency_hz > config.frequency_hz) ||
      config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db < 0.0f) {
    throw std::invalid_argument("invalid spectral shaper configuration");
  }
}

}  // namespace sonare::mastering::spectral
