#include "mastering/spectral/low_end_focus.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "rt/biquad_design.h"

namespace sonare::mastering::spectral {

LowEndFocus::LowEndFocus(LowEndFocusConfig config) : config_(config) { validate_config(config_); }

void LowEndFocus::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void LowEndFocus::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) throw std::logic_error("LowEndFocus must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  if (low_state_.size() != static_cast<size_t>(num_channels)) {
    const auto size = static_cast<size_t>(num_channels);
    low_state_.assign(size, 0.0f);
    sub_state_.assign(size, 0.0f);
    transient_state_.assign(size, 0.0f);
    previous_low_.assign(size, 0.0f);
    divider_polarity_.assign(size, 1.0f);
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }

  const float low_alpha = rt::one_pole_lowpass_alpha(config_.cutoff_hz, sample_rate_);
  const float sub_alpha = rt::one_pole_lowpass_alpha(config_.cutoff_hz * 0.5f, sample_rate_);
  const float transient_alpha = rt::one_pole_lowpass_alpha(25.0f, sample_rate_);
  for (int i = 0; i < num_samples; ++i) {
    for (int ch = 0; ch < num_channels; ++ch) {
      const auto index = static_cast<size_t>(ch);
      low_state_[index] += low_alpha * (channels[ch][i] - low_state_[index]);
      if (previous_low_[index] < 0.0f && low_state_[index] >= 0.0f) {
        divider_polarity_[index] = -divider_polarity_[index];
      }
      previous_low_[index] = low_state_[index];

      const float divided = std::abs(low_state_[index]) * divider_polarity_[index];
      sub_state_[index] += sub_alpha * (divided - sub_state_[index]);
      transient_state_[index] += transient_alpha * (low_state_[index] - transient_state_[index]);
    }
    if (num_channels >= 2) {
      const float mono_low = 0.5f * (low_state_[0] + low_state_[1]);
      for (int ch = 0; ch < 2; ++ch) {
        const auto index = static_cast<size_t>(ch);
        const float low = mono_low + (low_state_[index] - mono_low) * config_.width;
        const float high = channels[ch][i] - low_state_[index];
        const float transient = low_state_[index] - transient_state_[index];
        channels[ch][i] = high + low + config_.subharmonic_amount * sub_state_[index] +
                          config_.transient_tightness * transient;
      }
    } else {
      const float high = channels[0][i] - low_state_[0];
      const float transient = low_state_[0] - transient_state_[0];
      channels[0][i] = high + low_state_[0] + config_.subharmonic_amount * sub_state_[0] +
                       config_.transient_tightness * transient;
    }
  }
}

void LowEndFocus::reset() {
  std::fill(low_state_.begin(), low_state_.end(), 0.0f);
  std::fill(sub_state_.begin(), sub_state_.end(), 0.0f);
  std::fill(transient_state_.begin(), transient_state_.end(), 0.0f);
  std::fill(previous_low_.begin(), previous_low_.end(), 0.0f);
  std::fill(divider_polarity_.begin(), divider_polarity_.end(), 1.0f);
}

void LowEndFocus::set_config(const LowEndFocusConfig& config) {
  validate_config(config);
  config_ = config;
}

bool LowEndFocus::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.cutoff_hz = std::max(value, 1.0e-3f);
      return true;
    case 1:
      config_.width = std::clamp(value, 0.0f, 2.0f);
      return true;
    case 2:
      config_.subharmonic_amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 3:
      config_.transient_tightness = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void LowEndFocus::validate_config(const LowEndFocusConfig& config) {
  if (!(config.cutoff_hz > 0.0f) || !(config.width >= 0.0f && config.width <= 2.0f) ||
      !(config.subharmonic_amount >= 0.0f && config.subharmonic_amount <= 1.0f) ||
      !(config.transient_tightness >= 0.0f && config.transient_tightness <= 1.0f)) {
    throw std::invalid_argument("invalid low end focus configuration");
  }
}

}  // namespace sonare::mastering::spectral
