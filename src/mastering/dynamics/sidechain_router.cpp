#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::dynamics {

namespace {
using sonare::constants::kTwoPi;
// The library targets mono/stereo only; preallocate detector state for two
// channels so the audio thread never resizes the HPF state.
constexpr int kMaxChannels = 2;
}  // namespace

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
  lookahead_samples_ = static_cast<int>(
      std::round(std::clamp(config_.lookahead_ms, 0.0f, 1000.0f) * 0.001f * sample_rate_));
  prepared_ = true;
  if (config_.sidechain_hpf_enabled) {
    const float cutoff =
        std::clamp(config_.sidechain_hpf_hz, 1.0f, static_cast<float>(sample_rate_ * 0.49));
    const float rc = 1.0f / (kTwoPi * cutoff);
    const float dt = 1.0f / static_cast<float>(sample_rate_);
    hpf_coeff_ = rc / (rc + dt);
  }
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  for (auto& lookahead : lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
  for (auto& lookahead : gain_lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
  // Preallocate detector HPF state for the maximum supported channel count so
  // it never has to grow on the audio thread.
  ensure_hpf_state(kMaxChannels);
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
  ensure_lookahead(num_channels);
  // HPF state is preallocated in prepare()/set_sidechain() for the maximum
  // supported channel count; no audio-thread reallocation here.
  float max_reduction = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float detector = detector_sample(channels, ch, i);
      if (config_.key_listen) {
        channels[ch][i] = detector;
        continue;
      }
      const float envelope = follower.process(detector);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), config_);
      const float reduction_gain = db_to_linear(reduction_db);
      const float main_sample = lookahead_samples_ > 0
                                    ? lookahead_[static_cast<size_t>(ch)].process(channels[ch][i])
                                    : channels[ch][i];
      const float delayed_gain =
          lookahead_samples_ > 0 ? gain_lookahead_[static_cast<size_t>(ch)].process(reduction_gain)
                                 : reduction_gain;
      channels[ch][i] = main_sample * delayed_gain;
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void SidechainRouter::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  for (auto& lookahead : lookahead_) {
    lookahead.reset();
  }
  for (auto& lookahead : gain_lookahead_) {
    lookahead.reset();
  }
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  last_gain_reduction_db_ = 0.0f;
}

void SidechainRouter::set_sidechain(const float* const* channels, int num_channels,
                                    int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("sidechain dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    clear_sidechain();
    return;
  }
  if (channels == nullptr) {
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
  // Grow detector HPF state if the external sidechain has more channels than
  // were preallocated. set_sidechain() is a non-RT setter, so resizing is safe
  // here (and avoids it on the audio thread).
  ensure_hpf_state(sidechain_num_channels_);
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
    lookahead_samples_ = static_cast<int>(
        std::round(std::clamp(config_.lookahead_ms, 0.0f, 1000.0f) * 0.001f * sample_rate_));
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    for (auto& lookahead : lookahead_) {
      lookahead.prepare(static_cast<size_t>(lookahead_samples_));
    }
    for (auto& lookahead : gain_lookahead_) {
      lookahead.prepare(static_cast<size_t>(lookahead_samples_));
    }
    reset();
  }
}

void SidechainRouter::validate_config(const SidechainRouterConfig& config) {
  if (!(config.ratio >= 1.0f) || config.attack_ms < 0.0f || config.release_ms < 0.0f ||
      config.range_db < 0.0f || config.sidechain_hpf_hz <= 0.0f || config.lookahead_ms < 0.0f) {
    throw std::invalid_argument("invalid sidechain router configuration");
  }
}

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

void SidechainRouter::ensure_lookahead(int num_channels) {
  if (lookahead_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  lookahead_.assign(static_cast<size_t>(num_channels), {});
  gain_lookahead_.assign(static_cast<size_t>(num_channels), {});
  for (auto& lookahead : lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
  for (auto& lookahead : gain_lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
}

void SidechainRouter::ensure_hpf_state(int num_channels) {
  const auto target = static_cast<size_t>(std::max(num_channels, 0));
  if (hpf_x1_.size() >= target) {
    return;
  }
  hpf_x1_.resize(target, 0.0f);
  hpf_y1_.resize(target, 0.0f);
}

float SidechainRouter::detector_sample(float* const* channels, int channel, int sample) {
  if (sidechain_channels_ == nullptr || sidechain_num_channels_ == 0 ||
      sidechain_num_samples_ == 0) {
    return channels[channel][sample];
  }

  if (sample >= sidechain_num_samples_) {
    return 0.0f;
  }
  float detector = 0.0f;
  int detector_channel = std::min(channel, sidechain_num_channels_ - 1);
  if (config_.mono_summing) {
    for (int ch = 0; ch < sidechain_num_channels_; ++ch)
      detector += sidechain_channels_[ch][sample];
    detector /= static_cast<float>(sidechain_num_channels_);
    detector_channel = 0;
  } else {
    detector = sidechain_channels_[detector_channel][sample];
  }
  if (config_.sidechain_hpf_enabled && detector_channel < static_cast<int>(hpf_x1_.size())) {
    auto idx = static_cast<size_t>(detector_channel);
    const float y = hpf_coeff_ * (hpf_y1_[idx] + detector - hpf_x1_[idx]);
    hpf_x1_[idx] = detector;
    hpf_y1_[idx] = y;
    detector = y;
  }
  return detector;
}

}  // namespace sonare::mastering::dynamics
