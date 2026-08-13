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
  update_latency_mirrors();
  // Publish the initial value so the audio thread can adopt it on the first
  // process_block() call even if set_config() is never invoked.
  config_cell_.store(config_);
  config_version_.fetch_add(1, std::memory_order_release);
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
  // Report the resolved (effective) retune grain through config() rather than
  // the requested value: grain size is structural and fixed at prepare() time
  // (0 means "derive from sample rate"), so latency_samples() already uses the
  // resolved grain. Mirror it into config_ so the two never disagree.
  sync_effective_grain_size();
  update_latency_mirrors();
  reset();
  // Re-publish so the audio thread sees the same (post-prepare) config and
  // does NOT re-apply coefficients on its first block (they are already
  // up-to-date from the loop above). adopt_snapshot_for_block() detects this
  // via the applied_config_version_ guard, which we set to match the version
  // we are about to publish.
  active_config_ = config_;
  applied_isp_limiter_active_ = config_.limiter.enable_isp_limiter;
  config_cell_.store(config_);
  applied_config_version_ = config_version_.fetch_add(1, std::memory_order_release) + 1;
}

void RealtimeVoiceChanger::reset() {
  for (auto& channel : channels_) reset_channel(channel);
}

void RealtimeVoiceChanger::set_config(const RealtimeVoiceChangerConfig& config) {
  // Writer side: normalize, update the visible mirror used by config(), and
  // hand the value off to the audio thread via the lock-free seqlock cell.
  // We deliberately DO NOT touch derived scalars (input_gain_, gate_attack_,
  // ...) or per-channel BiquadState here — those are written by the audio
  // thread inside adopt_snapshot_for_block() so set_config() can race safely
  // with process_block(). Storing into config_cell_ and bumping
  // config_version_ never allocates, locks, or throws, so this whole function
  // is realtime-safe to call from the audio thread itself (see the class doc
  // comment on set_config for why WASM needs that).
  config_ = normalize_realtime_voice_changer_config(config);
  // Keep config() reporting the effective grain: grain size is structural and
  // cannot change after prepare(), so a newly-requested grain in `config` has
  // no effect until the next prepare(). Overwrite it with the resolved value so
  // config() never advertises a size that is not actually in use.
  sync_effective_grain_size();
  update_latency_mirrors();
  config_cell_.store(config_);
  config_version_.fetch_add(1, std::memory_order_release);
}

void RealtimeVoiceChanger::update_latency_mirrors() noexcept {
  latency_isp_enabled_.store(config_.limiter.enable_isp_limiter, std::memory_order_relaxed);
}

void RealtimeVoiceChanger::sync_effective_grain_size() noexcept {
  // Only meaningful once prepare() has allocated the per-channel retune stages;
  // before that the requested value stands. All channels share the same
  // resolved grain, so channel 0 is representative.
  if (!channels_.empty()) {
    config_.retune.grain_size = channels_[0].retune.grain_size();
  }
}

void RealtimeVoiceChanger::update_derived(const RealtimeVoiceChangerConfig& config) {
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
  // Seed the retune config BEFORE prepare() so the requested grain size is the
  // one actually resolved/allocated. StreamingRetune::prepare() reads its own
  // config_.grain_size to size the grain/ring buffers, and its set_config()
  // treats grain size as structural (kept once prepared) — so if we relied on
  // the post-prepare apply_channel_config() to deliver the grain, prepare()
  // would have already locked in the default (auto) grain and the request would
  // be silently ignored.
  state.retune.set_config(config_.retune);
  state.retune.prepare(sample_rate_, max_block_size_);
  state.dry_delay.assign(static_cast<std::size_t>(state.retune.latency_samples()), 0.0f);
  state.dry_delay_pos = 0;
  state.formant.prepare(sample_rate_, max_block_size_);
  state.reverb.prepare(sample_rate_, max_block_size_);
  state.input_gain.prepare(sample_rate_, 10.0f);
  state.output_gain.prepare(sample_rate_, 10.0f);
  state.wet_mix.prepare(sample_rate_, 10.0f);
  state.eq_highpass_hz.prepare(sample_rate_, 12.0f);
  state.eq_body_db.prepare(sample_rate_, 12.0f);
  state.eq_presence_db.prepare(sample_rate_, 12.0f);
  state.eq_air_db.prepare(sample_rate_, 12.0f);
  state.gate_threshold_db.prepare(sample_rate_, 12.0f);
  state.gate_attack_ms.prepare(sample_rate_, 12.0f);
  state.gate_release_ms.prepare(sample_rate_, 12.0f);
  state.gate_range_db.prepare(sample_rate_, 12.0f);
  state.comp_threshold_db.prepare(sample_rate_, 12.0f);
  state.comp_ratio.prepare(sample_rate_, 12.0f);
  state.comp_attack_ms.prepare(sample_rate_, 12.0f);
  state.comp_release_ms.prepare(sample_rate_, 12.0f);
  state.comp_makeup_db.prepare(sample_rate_, 12.0f);
  state.deess_frequency_hz.prepare(sample_rate_, 12.0f);
  state.deess_threshold_db.prepare(sample_rate_, 12.0f);
  state.deess_ratio.prepare(sample_rate_, 12.0f);
  state.deess_range_db.prepare(sample_rate_, 12.0f);
  state.limiter_ceiling_db.prepare(sample_rate_, 12.0f);
  state.limiter_release_ms.prepare(sample_rate_, 12.0f);
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
  state.input_gain.set_target(db_to_gain(config.input_gain_db));
  state.output_gain.set_target(db_to_gain(config.output_gain_db));
  state.wet_mix.set_target(std::clamp(config.wet_mix, 0.0f, 1.0f));
  state.eq_highpass_hz.set_target(config.eq.highpass_hz);
  state.eq_body_db.set_target(config.eq.body_db);
  state.eq_presence_db.set_target(config.eq.presence_db);
  state.eq_air_db.set_target(config.eq.air_db);
  state.gate_threshold_db.set_target(config.gate.threshold_db);
  state.gate_attack_ms.set_target(config.gate.attack_ms);
  state.gate_release_ms.set_target(config.gate.release_ms);
  state.gate_range_db.set_target(config.gate.range_db);
  state.comp_threshold_db.set_target(config.compressor.threshold_db);
  state.comp_ratio.set_target(config.compressor.ratio);
  state.comp_attack_ms.set_target(config.compressor.attack_ms);
  state.comp_release_ms.set_target(config.compressor.release_ms);
  state.comp_makeup_db.set_target(config.compressor.makeup_gain_db);
  state.deess_frequency_hz.set_target(config.deesser.frequency_hz);
  state.deess_threshold_db.set_target(config.deesser.threshold_db);
  state.deess_ratio.set_target(config.deesser.ratio);
  state.deess_range_db.set_target(config.deesser.range_db);
  state.limiter_ceiling_db.set_target(config.limiter.ceiling_db);
  state.limiter_release_ms.set_target(config.limiter.release_ms);

  // ISP limiter config updates are RT-safe (no allocation / no re-prepare).
  // The enable flag is read at block dispatch time in process_block; this only
  // mirrors the time-constant changes.
  state.isp_limiter.set_config({config.limiter.isp_ceiling_dbtp, config.limiter.release_ms});
}

void RealtimeVoiceChanger::update_channel_filters(ChannelState& state) noexcept {
  state.hpf.set(rt::rbj_highpass(rt::frequency_to_w0(state.filter_highpass_hz, sample_rate_),
                                 sonare::constants::kButterworthQ));
  state.body.set(
      rt::rbj_peak(rt::frequency_to_w0(180.0f, sample_rate_), 0.85f, state.filter_body_db));
  state.presence.set(
      rt::rbj_peak(rt::frequency_to_w0(3600.0f, sample_rate_), 0.9f, state.filter_presence_db));
  state.air.set(
      rt::rbj_high_shelf(rt::frequency_to_w0(9500.0f, sample_rate_), 0.75f, state.filter_air_db));
  state.deess_band.set(
      rt::rbj_bandpass(rt::frequency_to_w0(state.filter_deess_frequency_hz, sample_rate_), 2.2f));
}

const RealtimeVoiceChangerConfig& RealtimeVoiceChanger::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry point. config_version_ is bumped (release) strictly
  // after the corresponding config_cell_.store() completes, so observing a
  // new version here (acquire) means the cell holds at least that value.
  // try_load_into() never allocates, locks, blocks, or spins. Unlike
  // try_load(), it reports a conflict (a concurrent set_config() caught
  // mid-write, or a torn read) instead of silently substituting a stale
  // cached value — so on a conflict we deliberately do NOT advance
  // applied_config_version_. The version we captured above stays unmatched,
  // so a LATER block's version check will still see a mismatch and retry the
  // read for real: no update is ever permanently dropped, only delayed by
  // (at most) one block. If a new value was adopted, re-derive the scalar
  // coefficients and re-apply per-channel DSP coefficients — both write to
  // members that the per-sample loop reads, but the loop has not started yet
  // for this block, so no race.
  const std::uint32_t version = config_version_.load(std::memory_order_acquire);
  if (version != applied_config_version_) {
    RealtimeVoiceChangerConfig candidate;
    if (config_cell_.try_load_into(&candidate)) {
      active_config_ = candidate;
      const bool next_isp_limiter_active = active_config_.limiter.enable_isp_limiter;
      update_derived(active_config_);
      for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
        apply_channel_config(channels_[ch], static_cast<int>(ch), active_config_);
        if (next_isp_limiter_active != applied_isp_limiter_active_) {
          // A disabled interval skips process_block(), leaving the lookahead
          // queue frozen. Reset on either enabled-state edge so re-enabling
          // cannot emit samples from before the interval.
          channels_[ch].isp_limiter.reset();
        }
      }
      applied_isp_limiter_active_ = next_isp_limiter_active;
      // version, not config_version_.load(): the value we already validated
      // against the candidate we just adopted. If a newer publish raced in
      // since, this block simply re-derives again on the next call — the
      // "worst case one redundant re-derive" the class doc already accepts.
      applied_config_version_ = version;
    }
    // Conflict: applied_config_version_ is left untouched (stale), so this
    // whole branch is retried on the next block instead of being skipped.
  }
  return active_config_;
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
  // reset() is an explicit state boundary, unlike set_config(): snap ramps so
  // a newly prepared/reset processor never inherits an old transition.
  state.input_gain.reset(state.input_gain.target());
  state.output_gain.reset(state.output_gain.target());
  state.wet_mix.reset(state.wet_mix.target());
  state.eq_highpass_hz.reset(state.eq_highpass_hz.target());
  state.eq_body_db.reset(state.eq_body_db.target());
  state.eq_presence_db.reset(state.eq_presence_db.target());
  state.eq_air_db.reset(state.eq_air_db.target());
  state.gate_threshold_db.reset(state.gate_threshold_db.target());
  state.gate_attack_ms.reset(state.gate_attack_ms.target());
  state.gate_release_ms.reset(state.gate_release_ms.target());
  state.gate_range_db.reset(state.gate_range_db.target());
  state.comp_threshold_db.reset(state.comp_threshold_db.target());
  state.comp_ratio.reset(state.comp_ratio.target());
  state.comp_attack_ms.reset(state.comp_attack_ms.target());
  state.comp_release_ms.reset(state.comp_release_ms.target());
  state.comp_makeup_db.reset(state.comp_makeup_db.target());
  state.deess_frequency_hz.reset(state.deess_frequency_hz.target());
  state.deess_threshold_db.reset(state.deess_threshold_db.target());
  state.deess_ratio.reset(state.deess_ratio.target());
  state.deess_range_db.reset(state.deess_range_db.target());
  state.limiter_ceiling_db.reset(state.limiter_ceiling_db.target());
  state.limiter_release_ms.reset(state.limiter_release_ms.target());
  state.filter_highpass_hz = state.eq_highpass_hz.current();
  state.filter_body_db = state.eq_body_db.current();
  state.filter_presence_db = state.eq_presence_db.current();
  state.filter_air_db = state.eq_air_db.current();
  state.filter_deess_frequency_hz = state.deess_frequency_hz.current();
  state.filter_update_countdown = 0;
  update_channel_filters(state);
  state.reverb.reset();
  std::fill(state.dry_delay.begin(), state.dry_delay.end(), 0.0f);
  state.dry_delay_pos = 0;
  state.isp_limiter.reset();
}

}  // namespace sonare::editing::voice_changer
