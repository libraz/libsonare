#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

SidechainRouter::SidechainRouter(SidechainRouterConfig config) : config_(config) {
  validate_config(config_);
}

void SidechainRouter::prepare(double sample_rate, int max_block_size) {
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

void SidechainRouter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("SidechainRouter must be prepared before processing");
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
  float max_reduction = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float detector = detector_sample(channels, ch, i);
      const float envelope = follower.process(detector);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), config_);
      channels[ch][i] *= db_to_linear(reduction_db);
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void SidechainRouter::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

void SidechainRouter::set_sidechain(const float* const* channels, int num_channels,
                                    int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("sidechain dimensions must be non-negative");
  }
  if ((num_channels > 0 && channels == nullptr) || (num_samples > 0 && channels == nullptr)) {
    throw std::invalid_argument("sidechain channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("sidechain channel buffer must not be null");
    }
  }

  sidechain_channels_ = channels;
  sidechain_num_channels_ = num_channels;
  sidechain_num_samples_ = num_samples;
}

void SidechainRouter::clear_sidechain() {
  sidechain_channels_ = nullptr;
  sidechain_num_channels_ = 0;
  sidechain_num_samples_ = 0;
}

void SidechainRouter::set_config(const SidechainRouterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

void SidechainRouter::validate_config(const SidechainRouterConfig& config) {
  if (!(config.ratio >= 1.0f) || config.attack_ms < 0.0f || config.release_ms < 0.0f ||
      config.range_db < 0.0f) {
    throw std::invalid_argument("invalid sidechain router configuration");
  }
}

float SidechainRouter::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

float SidechainRouter::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float SidechainRouter::gain_reduction_db(float input_db, const SidechainRouterConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  const float reduction = over_db * (1.0f - 1.0f / config.ratio);
  return -std::min(config.range_db, reduction);
}

void SidechainRouter::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

float SidechainRouter::detector_sample(float* const* channels, int channel, int sample) const {
  if (sidechain_channels_ == nullptr || sidechain_num_channels_ == 0 ||
      sidechain_num_samples_ == 0) {
    return channels[channel][sample];
  }

  const int detector_channel = std::min(channel, sidechain_num_channels_ - 1);
  if (sample >= sidechain_num_samples_) {
    return 0.0f;
  }
  return sidechain_channels_[detector_channel][sample];
}

}  // namespace sonare::mastering::dynamics
