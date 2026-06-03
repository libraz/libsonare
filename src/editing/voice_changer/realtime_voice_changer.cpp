#include <algorithm>
#include <cmath>

#include "editing/voice_changer/realtime.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

// The C ABI ↔ C++ ABI version consistency check (kVoiceChangerAbiVersion ==
// SONARE_VOICE_CHANGER_ABI_VERSION) lives in src/sonare_c_daw.cpp. Keeping it
// there preserves the layer rule "editing/ must not depend on the public C
// API header sonare_c.h" while still failing the build the moment the two
// constants drift.

namespace sonare::editing::voice_changer {
namespace {

constexpr float kDeessGainSmoothingHz = 200.0f;
constexpr float kDeessEnvelopeHz = 100.0f;
constexpr float kFastDetectorHz = 200.0f;
constexpr float kLimiterAttackMs = 0.1f;

float db_to_gain(float db) noexcept { return sonare::db_to_linear(db); }

}  // namespace

RealtimeVoiceChanger::RealtimeVoiceChanger(RealtimeVoiceChangerConfig config)
    : config_(normalize_realtime_voice_changer_config(config)) {
  // Derive sample-rate-independent gains/mixes so that config() observers see
  // consistent state even before prepare() is called. The sample-rate-dependent
  // branch inside update_derived() is guarded and will be re-run by prepare().
  update_derived(config_);
  // Publish the initial snapshot so the audio thread can adopt it on the first
  // process_block() call even if set_config() is never invoked.
  config_publisher_.publish(std::make_shared<const RealtimeVoiceChangerConfig>(config_));
}

void RealtimeVoiceChanger::prepare(double sample_rate, int max_block_size, int num_channels) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  if (num_channels < 1 || num_channels > 2)
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be 1 or 2");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  num_channels_ = num_channels;
  channels_.resize(static_cast<std::size_t>(num_channels_));
  scratch_.assign(static_cast<std::size_t>(std::max(1, max_block_size_)), 0.0f);
  // Allocation phase: this is the only place buffers may be (re)sized.
  for (auto& channel : channels_) allocate_channel(channel);
  update_derived(config_);
  // Configuration phase: realtime-safe coefficient/state updates. Safe to do
  // here from the control thread because prepare() is called before any audio
  // thread runs against this instance.
  for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
    apply_channel_config(channels_[ch], static_cast<int>(ch), config_);
  }
  reset();
  // Re-publish so the audio thread sees the same (post-prepare) snapshot and
  // does NOT re-apply coefficients on its first block (they are already
  // up-to-date from the loop above). adopt_snapshot_for_block() detects this
  // via the applied_snapshot_ pointer guard.
  auto fresh = std::make_shared<const RealtimeVoiceChangerConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_.publish(std::move(fresh));
  // Force the audio-thread current pointer to match applied_snapshot_ now,
  // so the first process_block() does not redundantly re-apply coefficients.
  config_publisher_.acquire();
}

void RealtimeVoiceChanger::reset() {
  for (auto& channel : channels_) reset_channel(channel);
}

void RealtimeVoiceChanger::set_config(const RealtimeVoiceChangerConfig& config) {
  // Control-thread side: normalize, update the visible mirror used by config(),
  // and hand the snapshot off to the audio thread via the lock-free publisher.
  // We deliberately DO NOT touch derived scalars (input_gain_, gate_attack_,
  // ...) or per-channel BiquadState here — those are written by the audio
  // thread inside adopt_snapshot_for_block() so set_config() can race safely
  // with process_block().
  config_ = normalize_realtime_voice_changer_config(config);
  config_publisher_.publish(std::make_shared<const RealtimeVoiceChangerConfig>(config_));
}

void RealtimeVoiceChanger::update_derived(const RealtimeVoiceChangerConfig& config) {
  input_gain_ = db_to_gain(config.input_gain_db);
  output_gain_ = db_to_gain(config.output_gain_db);
  wet_mix_ = std::clamp(config.wet_mix, 0.0f, 1.0f);
  if (sample_rate_ > 0.0) {
    fast_det_alpha_ = rt::one_pole_lowpass_alpha_matched(kFastDetectorHz, sample_rate_);
    gate_attack_ = rt::one_pole_alpha_from_time_ms(config.gate.attack_ms, sample_rate_);
    gate_release_ = rt::one_pole_alpha_from_time_ms(config.gate.release_ms, sample_rate_);
    comp_attack_ = rt::one_pole_alpha_from_time_ms(config.compressor.attack_ms, sample_rate_);
    comp_release_ = rt::one_pole_alpha_from_time_ms(config.compressor.release_ms, sample_rate_);
    limiter_attack_ = rt::one_pole_alpha_from_time_ms(kLimiterAttackMs, sample_rate_);
    limiter_release_ = rt::one_pole_alpha_from_time_ms(config.limiter.release_ms, sample_rate_);
    deess_alpha_ = rt::one_pole_lowpass_alpha_matched(kDeessEnvelopeHz, sample_rate_);
    deess_gain_alpha_ = rt::one_pole_lowpass_alpha_matched(kDeessGainSmoothingHz, sample_rate_);
  }
}

void RealtimeVoiceChanger::allocate_channel(ChannelState& state) {
  // Sub-component allocations: streaming retune/formant/reverb own their
  // internal buffers and resize them inside their own prepare() entry points.
  state.retune.prepare(sample_rate_, max_block_size_);
  state.formant.prepare(sample_rate_, max_block_size_);
  state.reverb.prepare(sample_rate_, max_block_size_);
  // ISP limiter: prepared unconditionally so toggling
  // LimiterConfig::enable_isp_limiter at runtime never triggers an allocation
  // from the audio thread. Cost is small (one TruePeakFilter history vector +
  // a sliding-max ring, both proportional to max_block_size_).
  state.isp_limiter.prepare(sample_rate_, max_block_size_);
}

void RealtimeVoiceChanger::apply_channel_config(ChannelState& state, int channel_index,
                                                const RealtimeVoiceChangerConfig& config) {
  // Sub-component coefficient updates (no buffer resizing).
  state.retune.set_config(config.retune);
  state.formant.set_config(config.formant);
  state.reverb.set_config(config.reverb, channel_index);

  // Biquad coefficient updates: pure scalar math, RT-safe.
  state.hpf.set(rt::rbj_highpass(rt::frequency_to_w0(config.eq.highpass_hz, sample_rate_),
                                 sonare::constants::kButterworthQ));
  state.body.set(rt::rbj_peak(rt::frequency_to_w0(180.0f, sample_rate_), 0.85f, config.eq.body_db));
  state.presence.set(
      rt::rbj_peak(rt::frequency_to_w0(3600.0f, sample_rate_), 0.9f, config.eq.presence_db));
  state.air.set(
      rt::rbj_high_shelf(rt::frequency_to_w0(9500.0f, sample_rate_), 0.75f, config.eq.air_db));
  state.deess_band.set(
      rt::rbj_bandpass(rt::frequency_to_w0(config.deesser.frequency_hz, sample_rate_), 2.2f));

  // ISP limiter config updates are RT-safe (no allocation / no re-prepare).
  // The enable flag is read at block dispatch time in process_block; this only
  // mirrors the time-constant changes.
  state.isp_limiter.set_config({config.limiter.isp_ceiling_dbtp, config.limiter.release_ms});
}

const RealtimeVoiceChangerConfig& RealtimeVoiceChanger::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry point. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients and re-apply per-channel DSP coefficients
  // — both write to members that the per-sample loop reads, but the loop has
  // not started yet for this block, so no race.
  config_publisher_.acquire();
  const RealtimeVoiceChangerConfig* current = config_publisher_.current();
  if (current && current != applied_snapshot_) {
    update_derived(*current);
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
      apply_channel_config(channels_[ch], static_cast<int>(ch), *current);
    }
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen for a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by sample_rate_ > 0.0 so this path stays
  // defined even with default-initialised members.
  return current ? *current : config_;
}

void RealtimeVoiceChanger::reset_channel(ChannelState& state) {
  state.retune.reset();
  state.formant.reset();
  state.hpf.reset();
  state.body.reset();
  state.presence.reset();
  state.air.reset();
  state.deess_band.reset();
  state.gate_env = 0.0f;
  state.gate_gain = 1.0f;
  state.comp_env = 0.0f;
  state.comp_gain = 1.0f;
  state.deess_env = 0.0f;
  state.deess_gain = 1.0f;
  state.limiter_gain = 1.0f;
  state.reverb.reset();
  state.isp_limiter.reset();
}

}  // namespace sonare::editing::voice_changer
