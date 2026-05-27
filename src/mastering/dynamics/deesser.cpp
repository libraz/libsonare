#include "mastering/dynamics/deesser.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::dynamics {

namespace {

using sonare::constants::kPiD;

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
  if (bandpass_.size() < kRealtimePreparedChannels) {
    bandpass_.resize(kRealtimePreparedChannels, filter_coeffs_);
  }
  if (bandpass2_.size() < kRealtimePreparedChannels) {
    bandpass2_.resize(kRealtimePreparedChannels, filter_coeffs_);
  }
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  reset();
}

void DeEsser::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
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

    auto& filter = bandpass_[static_cast<size_t>(ch)];
    auto& filter2 = bandpass2_[static_cast<size_t>(ch)];
    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float input = channels[ch][i];
      const float sibilant = filter2.process(filter.process(input));
      const float envelope = follower.process(sibilant);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), config_);
      channels[ch][i] = input * db_to_linear(reduction_db);
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void DeEsser::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : bandpass2_) filter.reset();
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

bool DeEsser::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Keep frequency positive (validate_config invariant); update_filter_coeff
      // clamps the effective cutoff to a valid range and preserves filter state.
      config_.frequency_hz = std::max(value, 1.0f);
      if (prepared_) {
        update_filter_coeff();
      }
      return true;
    case 1:
      config_.threshold_db = value;
      return true;
    case 2:
      config_.ratio = std::max(1.0f, value);
      return true;
    case 3:
      config_.attack_ms = std::max(0.0f, value);
      // Recompute follower coefficients in place; preserves envelope state.
      if (prepared_) {
        for (auto& follower : followers_) {
          follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 4:
      config_.release_ms = std::max(0.0f, value);
      if (prepared_) {
        for (auto& follower : followers_) {
          follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 5:
      config_.range_db = std::max(0.0f, value);
      return true;
    case 6:
      config_.bandpass_q = std::max(1.0e-3f, value);
      if (prepared_) {
        update_filter_coeff();
      }
      return true;
    default:
      return false;
  }
}

void DeEsser::validate_config(const DeEsserConfig& config) {
  if (!(config.frequency_hz > 0.0f) || !(config.ratio >= 1.0f) || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f || config.range_db < 0.0f || !(config.bandpass_q > 0.0f)) {
    throw std::invalid_argument("invalid de-esser configuration");
  }
}

float DeEsser::gain_reduction_db(float input_db, const DeEsserConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  const float reduction = over_db * (1.0f - 1.0f / config.ratio);
  return -std::min(config.range_db, reduction);
}

void DeEsser::ensure_state(int num_channels) {
  const auto target_size = static_cast<size_t>(num_channels);
  if (followers_.size() >= target_size && bandpass_.size() >= target_size &&
      bandpass2_.size() >= target_size) {
    return;
  }

  throw std::invalid_argument("num_channels exceeds prepared DeEsser state");
}

void DeEsser::update_filter_coeff() {
  const float nyquist = static_cast<float>(sample_rate_ * 0.5);
  const float cutoff = std::clamp(config_.frequency_hz, 10.0f, nyquist * 0.98f);
  const float q = std::max(1.0e-3f, config_.bandpass_q);
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  const auto coeffs = rt::rbj_bandpass(w0, q);
  filter_coeffs_.b0 = coeffs.b0;
  filter_coeffs_.b1 = coeffs.b1;
  filter_coeffs_.b2 = coeffs.b2;
  filter_coeffs_.a1 = coeffs.a1;
  filter_coeffs_.a2 = coeffs.a2;
  for (auto& filter : bandpass_) {
    const float z1 = filter.z1;
    const float z2 = filter.z2;
    filter = filter_coeffs_;
    filter.z1 = z1;
    filter.z2 = z2;
  }
  for (auto& filter : bandpass2_) {
    const float z1 = filter.z1;
    const float z2 = filter.z2;
    filter = filter_coeffs_;
    filter.z1 = z1;
    filter.z2 = z2;
  }
}

}  // namespace sonare::mastering::dynamics
