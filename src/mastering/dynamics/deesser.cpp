#include "mastering/dynamics/deesser.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

DeEsser::DeEsser(DeEsserConfig config) : config_(config) { validate_config(config_); }

void DeEsser::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  update_filter_coeff();
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  reset();
}

void DeEsser::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("DeEsser must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  ensure_state(num_channels);
  float max_reduction = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& low_state = lowpass_state_[static_cast<size_t>(ch)];
    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float input = channels[ch][i];
      low_state = lowpass_coeff_ * low_state + (1.0f - lowpass_coeff_) * input;
      const float high = input - low_state;
      const float envelope = follower.process(high);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), config_);
      channels[ch][i] = low_state + high * db_to_linear(reduction_db);
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void DeEsser::reset() {
  std::fill(lowpass_state_.begin(), lowpass_state_.end(), 0.0f);
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

void DeEsser::set_config(const DeEsserConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_filter_coeff();
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

void DeEsser::validate_config(const DeEsserConfig& config) {
  if (!(config.frequency_hz > 0.0f) || !(config.ratio >= 1.0f) || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f || config.range_db < 0.0f) {
    throw std::invalid_argument("invalid de-esser configuration");
  }
}

float DeEsser::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

float DeEsser::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float DeEsser::gain_reduction_db(float input_db, const DeEsserConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  const float reduction = over_db * (1.0f - 1.0f / config.ratio);
  return -std::min(config.range_db, reduction);
}

void DeEsser::ensure_state(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  lowpass_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

void DeEsser::update_filter_coeff() {
  const float nyquist = static_cast<float>(sample_rate_ * 0.5);
  const float cutoff = std::clamp(config_.frequency_hz, 10.0f, nyquist * 0.98f);
  lowpass_coeff_ = static_cast<float>(std::exp(-2.0 * kPi * cutoff / sample_rate_));
}

}  // namespace sonare::mastering::dynamics
