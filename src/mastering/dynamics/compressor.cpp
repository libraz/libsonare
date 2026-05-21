#include "mastering/dynamics/compressor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

Compressor::Compressor(CompressorConfig config) : config_(config) { validate_config(config_); }

void Compressor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  reset();
}

void Compressor::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Compressor must be prepared before processing");
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

  ensure_followers(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  float max_reduction = 0.0f;
  const float makeup_db =
      config_.makeup_gain_db + (config_.auto_makeup ? std::max(0.0f, -config_.threshold_db) *
                                                          (1.0f - 1.0f / config_.ratio) * 0.5f
                                                    : 0.0f);

  for (int i = 0; i < num_samples; ++i) {
    for (int ch = 0; ch < num_channels; ++ch) {
      const float detector_input = config_.detector == DetectorMode::Rms
                                       ? channels[ch][i] * channels[ch][i]
                                       : std::abs(channels[ch][i]);
      const float envelope = followers_[static_cast<size_t>(ch)].process(detector_input);
      const float level = config_.detector == DetectorMode::Rms ? std::sqrt(envelope) : envelope;
      const float reduction_db = gain_reduction_db(linear_to_db(level), config_);
      const float gain = db_to_linear(reduction_db + makeup_db);
      channels[ch][i] *= gain;
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void Compressor::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

void Compressor::set_config(const CompressorConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

void Compressor::validate_config(const CompressorConfig& config) {
  if (!(config.ratio >= 1.0f)) {
    throw std::invalid_argument("compressor ratio must be at least 1");
  }
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.knee_db < 0.0f) {
    throw std::invalid_argument("compressor timing and knee values must be non-negative");
  }
}

float Compressor::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

float Compressor::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float Compressor::gain_reduction_db(float input_db, const CompressorConfig& config) {
  if (config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  float compressed_over_db = 0.0f;
  if (config.knee_db > 0.0f) {
    const float half_knee = config.knee_db * 0.5f;
    if (over_db <= -half_knee) {
      compressed_over_db = 0.0f;
    } else if (over_db >= half_knee) {
      compressed_over_db = over_db * (1.0f - 1.0f / config.ratio);
    } else {
      const float x = over_db + half_knee;
      compressed_over_db = (1.0f - 1.0f / config.ratio) * x * x / (2.0f * config.knee_db);
    }
  } else if (over_db > 0.0f) {
    compressed_over_db = over_db * (1.0f - 1.0f / config.ratio);
  }

  return -compressed_over_db;
}

void Compressor::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
