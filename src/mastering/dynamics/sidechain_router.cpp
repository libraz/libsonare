#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#include "mastering/common/biquad_design.h"
#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"

namespace sonare::mastering::dynamics {

namespace {
// The library targets mono/stereo only; preallocate detector state for two
// channels so the audio thread never resizes the HPF state.
constexpr int kMaxChannels = 2;
}  // namespace

SidechainRouter::SidechainRouter(SidechainRouterConfig config)
    : config_(config),
      config_publisher_(std::make_unique<rt::RtPublisher<SidechainRouterConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const SidechainRouterConfig>(config_));
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
  for (auto& lookahead : lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
  for (auto& lookahead : gain_lookahead_) {
    lookahead.prepare(static_cast<size_t>(lookahead_samples_));
  }
  update_coefficients(config_);
  // Preallocate detector HPF state for the maximum supported channel count so
  // it never has to grow on the audio thread.
  ensure_hpf_state(kMaxChannels);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const SidechainRouterConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const SidechainRouterConfig* SidechainRouter::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const SidechainRouterConfig* current = config_publisher_->current();
  if (current && current != applied_snapshot_) {
    update_coefficients(*current);
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen on a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by prepared_ so this path stays defined.
  return current ? current : &config_;
}

void SidechainRouter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SidechainRouter");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const SidechainRouterConfig& cfg = *adopt_snapshot_for_block();

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
      const float detector = detector_sample(channels, ch, i, cfg);
      if (cfg.key_listen) {
        channels[ch][i] = detector;
        continue;
      }
      const float envelope = follower.process(detector);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), cfg);
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
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const SidechainRouterConfig>(config_));
}

bool SidechainRouter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      break;
    case 1:
      config_.ratio = std::max(1.0f, value);
      break;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      break;
    case 4:
      config_.range_db = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const SidechainRouterConfig>(config_));
  return true;
}

void SidechainRouter::validate_config(const SidechainRouterConfig& config) {
  if (!(config.ratio >= 1.0f) || config.attack_ms < 0.0f || config.release_ms < 0.0f ||
      config.range_db < 0.0f || config.lookahead_ms < 0.0f ||
      (config.sidechain_hpf_enabled && config.sidechain_hpf_hz <= 0.0f)) {
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

void SidechainRouter::update_coefficients(const SidechainRouterConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
  const auto hpf =
      common::onepole_highpass_coeffs(static_cast<double>(config.sidechain_hpf_hz), sample_rate_);
  hpf_b0_ = hpf.b0;
  hpf_a1_ = hpf.a1;
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

float SidechainRouter::detector_sample(float* const* channels, int channel, int sample,
                                       const SidechainRouterConfig& cfg) {
  if (sidechain_channels_ == nullptr || sidechain_num_channels_ == 0 ||
      sidechain_num_samples_ == 0) {
    return channels[channel][sample];
  }

  if (sample >= sidechain_num_samples_) {
    return 0.0f;
  }
  float detector = 0.0f;
  int detector_channel = std::min(channel, sidechain_num_channels_ - 1);
  if (cfg.mono_summing) {
    for (int ch = 0; ch < sidechain_num_channels_; ++ch)
      detector += sidechain_channels_[ch][sample];
    detector /= static_cast<float>(sidechain_num_channels_);
    detector_channel = 0;
  } else {
    detector = sidechain_channels_[detector_channel][sample];
  }
  if (cfg.sidechain_hpf_enabled && detector_channel < static_cast<int>(hpf_x1_.size())) {
    auto idx = static_cast<size_t>(detector_channel);
    const float y = hpf_b0_ * (detector - hpf_x1_[idx]) + hpf_a1_ * hpf_y1_[idx];
    hpf_x1_[idx] = detector;
    hpf_y1_[idx] = y;
    detector = y;
  }
  return detector;
}

}  // namespace sonare::mastering::dynamics
