#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

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
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  lookahead_samples_ = static_cast<int>(
      std::round(std::clamp(config_.lookahead_ms, 0.0f, 1000.0f) * 0.001f * sample_rate_));
  prepared_ = true;
  // Preallocate per-channel main delay lines, the single shared gain delay line,
  // and the per-source-channel HPF state up front so the audio-thread process()
  // path never resizes (which would malloc). A block (or sidechain) requesting
  // more than kRealtimePreparedChannels channels throws in ensure_capacity().
  const auto delay = static_cast<size_t>(std::max(lookahead_samples_, 0));
  lookahead_.assign(kRealtimePreparedChannels, {});
  for (auto& lookahead : lookahead_) {
    lookahead.prepare(delay);
  }
  gain_lookahead_.prepare(delay);
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  update_coefficients(config_);
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
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SidechainRouter");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const SidechainRouterConfig& cfg = *adopt_snapshot_for_block();

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }
  // Per-channel main delay lines and per-source-channel HPF state are
  // preallocated in prepare(); verify the block (and the external sidechain, if
  // any) fit the prepared state instead of resizing on the audio thread.
  ensure_capacity(num_channels);
  if (sidechain_channels_ != nullptr) {
    ensure_capacity(sidechain_num_channels_);
  }

  float max_reduction = 0.0f;
  // Sample-major loop with a single linked detector and a single shared
  // envelope follower: every output channel receives the same gain (preserves
  // the stereo image) and the sidechain HPF runs exactly once per source
  // channel per sample (no double-filtering across output channels).
  for (int i = 0; i < num_samples; ++i) {
    const float detector = detector_sample(channels, num_channels, i, cfg);
    if (cfg.key_listen) {
      for (int ch = 0; ch < num_channels; ++ch) {
        channels[ch][i] = detector;
      }
      continue;
    }
    const float envelope = follower_.process(detector);
    const float reduction_db = gain_reduction_db(linear_to_db(envelope), cfg);
    const float reduction_gain = db_to_linear(reduction_db);
    const float delayed_gain =
        lookahead_samples_ > 0 ? gain_lookahead_.process(reduction_gain) : reduction_gain;
    for (int ch = 0; ch < num_channels; ++ch) {
      const float main_sample = lookahead_samples_ > 0
                                    ? lookahead_[static_cast<size_t>(ch)].process(channels[ch][i])
                                    : channels[ch][i];
      channels[ch][i] = main_sample * delayed_gain;
    }
    max_reduction = std::min(max_reduction, reduction_db);
  }

  last_gain_reduction_db_ = max_reduction;
}

void SidechainRouter::reset() {
  follower_.reset();
  for (auto& lookahead : lookahead_) {
    lookahead.reset();
  }
  gain_lookahead_.reset();
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  last_gain_reduction_db_ = 0.0f;
}

void SidechainRouter::set_sidechain(const float* const* channels, int num_channels,
                                    int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    clear_sidechain();
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "sidechain channel buffer must not be null");
    }
  }

  // The detector HPF state is preallocated to kRealtimePreparedChannels in
  // prepare(); an external sidechain wider than that is rejected here (on the
  // non-RT control thread) rather than triggering an audio-thread resize.
  if (prepared_ && static_cast<size_t>(num_channels) > hpf_x1_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sidechain channels exceed prepared SidechainRouter state");
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
    throw SonareException(ErrorCode::InvalidParameter, "invalid sidechain router configuration");
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
  follower_.prepare(sample_rate_, config.attack_ms, config.release_ms);
  const auto hpf = sonare::rt::onepole_highpass_coeffs(static_cast<double>(config.sidechain_hpf_hz),
                                                       sample_rate_);
  hpf_b0_ = hpf.b0;
  hpf_a1_ = hpf.a1;
}

void SidechainRouter::ensure_capacity(int num_channels) const {
  // RT-safe: never resizes on the audio thread. The per-channel main delay
  // lines and per-source-channel HPF state are preallocated to
  // kRealtimePreparedChannels in prepare(); a wider block throws instead of
  // allocating (matches Limiter::prepare_buffers).
  if (static_cast<size_t>(num_channels) <= lookahead_.size() &&
      static_cast<size_t>(num_channels) <= hpf_x1_.size()) {
    return;
  }
  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared SidechainRouter state");
}

float SidechainRouter::detector_sample(float* const* channels, int num_channels, int sample,
                                       const SidechainRouterConfig& cfg) {
  // Choose the detector source: the external sidechain when set, otherwise the
  // main channels (self / internal detection). The sidechain HPF is honored in
  // both modes.
  const bool use_external =
      sidechain_channels_ != nullptr && sidechain_num_channels_ > 0 && sidechain_num_samples_ > 0;
  if (use_external && sample >= sidechain_num_samples_) {
    return 0.0f;
  }
  const int source_channels = use_external ? sidechain_num_channels_ : num_channels;

  // Fold the source down to a single linked detector value, applying the
  // per-channel HPF once per source channel per sample. Because the one-pole
  // HPF is LTI, filtering each source channel and averaging is identical to
  // averaging then filtering, so mono summing keeps its meaning while never
  // sharing HPF state across output channels.
  float abs_sum = 0.0f;
  float peak = 0.0f;
  for (int ch = 0; ch < source_channels; ++ch) {
    float s = use_external ? sidechain_channels_[ch][sample] : channels[ch][sample];
    if (cfg.sidechain_hpf_enabled) {
      const auto idx = static_cast<size_t>(ch);
      const float y = hpf_b0_ * (s - hpf_x1_[idx]) + hpf_a1_ * hpf_y1_[idx];
      hpf_x1_[idx] = s;
      hpf_y1_[idx] = y;
      s = y;
    }
    // Sum magnitudes, not signed amplitudes: a signed sum collapses toward zero
    // for an anti-correlated (out-of-phase) stereo key, making a loud key read
    // as silence and the ducking fail to trigger. The mean of |s| stays
    // representative regardless of inter-channel phase.
    abs_sum += std::abs(s);
    peak = std::max(peak, std::abs(s));
  }
  if (cfg.mono_summing) {
    return abs_sum / static_cast<float>(std::max(source_channels, 1));
  }
  return peak;
}

}  // namespace sonare::mastering::dynamics
