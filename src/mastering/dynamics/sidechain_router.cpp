#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/dynamics/detector_level.h"
#include "mastering/dynamics/lookahead_validation.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::dynamics {

namespace {

// Field domains shared by validate_config and live automation.
bool valid_ratio(float v) noexcept { return std::isfinite(v) && v >= 1.0f; }
bool valid_non_negative_ms(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
SidechainRouter::SidechainRouter(SidechainRouterConfig config) : ConfigBase(std::move(config)) {}

void SidechainRouter::prepare(double sample_rate, int max_block_size) {
  if (!std::isfinite(sample_rate) || !(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be finite and positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  const int lookahead_samples = checked_lookahead_samples(sample_rate, config_.lookahead_ms);
  sample_rate_ = sample_rate;
  lookahead_samples_ = lookahead_samples;
  prepared_ = true;
  // Preallocate per-channel main delay lines and the per-source-channel HPF state up front so the
  // audio-thread process() path never resizes (which would malloc). A block (or sidechain)
  // requesting more than kRealtimePreparedChannels channels throws in ensure_capacity().
  const auto delay = static_cast<size_t>(std::max(lookahead_samples_, 0));
  lookahead_.assign(kRealtimePreparedChannels, {});
  for (auto& lookahead : lookahead_) {
    lookahead.prepare(delay);
  }
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_channel_gates_.assign(kRealtimePreparedChannels, rt::StageGate{});
  active_ = config_;
  update_coefficients(active_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void SidechainRouter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SidechainRouter");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const SidechainRouterConfig& cfg = *adopt_snapshot_for_block();

  // Per-channel main delay lines and per-source-channel HPF state are
  // preallocated in prepare(); verify the block (and the external sidechain, if
  // any) fit the prepared state instead of resizing on the audio thread.
  ensure_capacity(num_channels);
  if (sidechain_channels_ != nullptr) {
    ensure_capacity(sidechain_num_channels_);
  }

  // Listening and ducking share the main delay lines, so each mode enters from rest; listening
  // delays the key by the same reported latency the ducked signal carries.
  auto clear_lines = [this] {
    for (auto& lookahead : lookahead_) lookahead.reset();
  };
  const bool listening = listen_gate_.admit(cfg.key_listen, clear_lines);
  duck_gate_.admit(!cfg.key_listen, [this, &clear_lines] {
    follower_.reset();
    clear_lines();
  });

  // Bound is decided by the binding, not the remaining length: an exhausted key
  // (zero samples left) stays external and reads as silence.
  const bool use_external = sidechain_channels_ != nullptr && sidechain_num_channels_ > 0;
  const int source_channels = use_external ? sidechain_num_channels_ : num_channels;
  // An external key owns its own channel layout, so only the bus's internal
  // detector excludes its LFE plane. Applying the bus plane index to an
  // unrelated key would silently discard a valid sidechain channel.
  const int excluded_channel = use_external ? -1 : detector_excluded_channel(num_channels);
  auto restart_hpf = [this] {
    for (auto& gate : hpf_channel_gates_) gate.close();
  };
  key_source_gate_.admit(use_external, restart_hpf);
  internal_source_gate_.admit(!use_external, restart_hpf);
  const bool sidechain_hpf_enabled = sidechain_hpf_gate_.admit(cfg.sidechain_hpf_enabled, [this] {
    std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
    std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  });
  std::array<bool, kRealtimePreparedChannels> hpf_admitted{};
  for (size_t ch = 0; ch < hpf_channel_gates_.size(); ++ch) {
    const bool active = sidechain_hpf_enabled && ch < static_cast<size_t>(source_channels) &&
                        static_cast<int>(ch) != excluded_channel;
    hpf_admitted[ch] = hpf_channel_gates_[ch].admit(active, [this, ch] {
      hpf_x1_[ch] = 0.0f;
      hpf_y1_[ch] = 0.0f;
    });
  }

  float max_reduction = 0.0f;
  // Sample-major loop with a single linked detector and a single shared
  // envelope follower: every output channel receives the same gain (preserves
  // the stereo image) and the sidechain HPF runs exactly once per source
  // channel per sample (no double-filtering across output channels).
  for (int i = 0; i < num_samples; ++i) {
    const float detector = detector_sample(channels, num_channels, i, cfg, use_external,
                                           excluded_channel, hpf_admitted.data());
    if (listening) {
      for (int ch = 0; ch < num_channels; ++ch) {
        channels[ch][i] = lookahead_samples_ > 0
                              ? lookahead_[static_cast<size_t>(ch)].process(detector)
                              : detector;
      }
      continue;
    }
    const float envelope = follower_.process(detector);
    const float reduction_db = gain_reduction_db(detector_level_db(envelope), cfg);
    // Only the main signal is delayed: the gain is the key's current one, so it
    // moves lookahead_samples_ ahead of the transient it answers.
    const float reduction_gain = db_to_linear(reduction_db);
    for (int ch = 0; ch < num_channels; ++ch) {
      const float main_sample = lookahead_samples_ > 0
                                    ? lookahead_[static_cast<size_t>(ch)].process(channels[ch][i])
                                    : channels[ch][i];
      channels[ch][i] = main_sample * reduction_gain;
    }
    max_reduction = std::min(max_reduction, reduction_db);
  }

  // Scrub the HPF too: key-listen bypasses the follower, and a NaN HPF stops ducking for good.
  bool discarded = follower_.discard_if_non_finite();
  for (size_t ch = 0; ch < hpf_x1_.size(); ++ch) {
    discarded |= sonare::discard_group_if_non_finite(hpf_x1_[ch], hpf_y1_[ch]);
  }
  if (discarded) note_non_finite_discard();

  last_gain_reduction_db_ = max_reduction;
}

void SidechainRouter::reset() {
  follower_.reset();
  for (auto& lookahead : lookahead_) {
    lookahead.reset();
  }
  sidechain_hpf_gate_.close();
  for (auto& gate : hpf_channel_gates_) gate.close();
  key_source_gate_.close();
  internal_source_gate_.close();
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  last_gain_reduction_db_ = 0.0f;
}

void SidechainRouter::set_sidechain(const float* const* channels, int num_channels,
                                    int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain dimensions must be non-negative");
  }
  if (num_channels == 0) {
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

bool SidechainRouter::apply_parameter(SidechainRouterConfig& config, unsigned int param_id,
                                      float value) {
  switch (param_id) {
    case 0:
      config.threshold_db = value;
      break;
    case 1:
      if (!valid_ratio(value)) return false;
      config.ratio = value;
      break;
    case 2:
      if (!valid_non_negative_ms(value)) return false;
      config.attack_ms = value;
      break;
    case 3:
      if (!valid_non_negative_ms(value)) return false;
      config.release_ms = value;
      break;
    case 4:
      if (!valid_non_negative_ms(value)) return false;
      config.range_db = value;
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> SidechainRouter::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

void SidechainRouter::validate_config(const SidechainRouterConfig& config) {
  if (!std::isfinite(config.threshold_db) || !std::isfinite(config.ratio) ||
      !std::isfinite(config.attack_ms) || !std::isfinite(config.release_ms) ||
      !std::isfinite(config.range_db) || !std::isfinite(config.lookahead_ms) ||
      !std::isfinite(config.sidechain_hpf_hz) || !valid_ratio(config.ratio) ||
      !valid_non_negative_ms(config.attack_ms) || !valid_non_negative_ms(config.release_ms) ||
      !valid_non_negative_ms(config.range_db) || config.lookahead_ms < 0.0f ||
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
  require_prepared_channels(num_channels, std::min(lookahead_.size(), hpf_x1_.size()),
                            "SidechainRouter");
}

float SidechainRouter::detector_sample(float* const* channels, int num_channels, int sample,
                                       const SidechainRouterConfig& cfg, bool use_external,
                                       int excluded_channel, const bool* hpf_admitted) {
  // The detector source is the external sidechain when set, otherwise the main
  // channels (self / internal detection). The sidechain HPF is honored in both.
  // A key shorter than the block reads as zero-padded: the HPF still advances on silence, so its
  // state never goes stale across blocks.
  const bool key_present = !use_external || sample < sidechain_num_samples_;
  const int source_channels = use_external ? sidechain_num_channels_ : num_channels;

  // Fold the source down to a single linked detector value, applying the
  // per-channel HPF once per source channel per sample. Because the one-pole
  // HPF is LTI, filtering each source channel and averaging is identical to
  // averaging then filtering, so mono summing keeps its meaning while never
  // sharing HPF state across output channels.
  float abs_sum = 0.0f;
  float peak = 0.0f;
  int included_channels = 0;
  for (int ch = 0; ch < source_channels; ++ch) {
    if (ch == excluded_channel) continue;
    float s = !key_present
                  ? 0.0f
                  : (use_external ? sidechain_channels_[ch][sample] : channels[ch][sample]);
    if (hpf_admitted[ch]) {
      const auto idx = static_cast<size_t>(ch);
      s = rt::onepole_highpass_step(hpf_b0_, hpf_a1_, hpf_x1_[idx], hpf_y1_[idx], s);
    }
    // Sum magnitudes, not signed amplitudes: a signed sum collapses toward zero
    // for an anti-correlated (out-of-phase) stereo key, making a loud key read
    // as silence and the ducking fail to trigger. The mean of |s| stays
    // representative regardless of inter-channel phase.
    abs_sum += std::abs(s);
    peak = std::max(peak, std::abs(s));
    ++included_channels;
  }
  if (cfg.mono_summing) {
    return abs_sum / static_cast<float>(std::max(included_channels, 1));
  }
  return peak;
}

}  // namespace sonare::mastering::dynamics
