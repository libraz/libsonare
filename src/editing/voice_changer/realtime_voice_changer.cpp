#include <algorithm>
#include <cmath>
#include <string>

#include "editing/voice_changer/realtime.h"
#include "streaming/realtime_limits.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

// kVoiceChangerAbiVersion is checked against the C macro in
// src/c_api/sonare_c_voice_changer.cpp, which is the bridge layer that may
// include sonare_c.h; this one must not.

namespace sonare::editing::voice_changer {
namespace {

constexpr float kDeessGainSmoothingHz = 200.0f;
constexpr float kDeessEnvelopeHz = 100.0f;
constexpr float kFastDetectorHz = 200.0f;
constexpr float kLimiterAttackMs = 0.1f;
/// Glide time of the dry/wet mix.
constexpr float kWetMixGlideMs = 10.0f;
/// Glide time of a voice's gain when the voice count or its level changes.
constexpr float kVoiceGainGlideMs = 10.0f;
/// Interpolator margin past the longest harmony delay.
constexpr std::size_t kHarmonyRingMargin = 8;
/// The shifter's history reach past its latency: grains read up to eight of the longest periods
/// back, plus four unvoiced grain spacings of 5 ms.
constexpr double kShifterHistoryPeriods = 8.0;
constexpr double kShifterHistoryUnvoicedS = 0.020;
/// The vocoder's band filters keep ringing for a few of their own group delays.
constexpr double kVocoderRingGroupDelays = 8.0;
/// Bound on how long any character type rings (the 4th-order radio corners are the longest).
constexpr double kCharacterTailS = 0.05;

float db_to_gain(float db) noexcept { return sonare::db_to_linear(db); }

rt::BiquadCoeffs input_highpass(double sample_rate, float corner_hz) {
  return rt::rbj_highpass(rt::frequency_to_w0(corner_hz, sample_rate),
                          sonare::constants::kButterworthQ);
}

rt::BiquadCoeffs body_peak(double sample_rate, float gain_db) {
  return rt::rbj_peak(rt::frequency_to_w0(180.0f, sample_rate), 0.85f, gain_db);
}

rt::BiquadCoeffs presence_peak(double sample_rate, float gain_db) {
  return rt::rbj_peak(rt::frequency_to_w0(3600.0f, sample_rate), 0.9f, gain_db);
}

rt::BiquadCoeffs air_shelf(double sample_rate, float gain_db) {
  return rt::rbj_high_shelf(rt::frequency_to_w0(9500.0f, sample_rate), 0.75f, gain_db);
}

// Returns @p config when it is valid; the failure text is built only on the refusing path.
const RealtimeVoiceChangerConfig& checked_config(const RealtimeVoiceChangerConfig& config) {
  if (validate_realtime_voice_changer_config(config, nullptr, nullptr)) return config;
  std::string error;
  validate_realtime_voice_changer_config(config, nullptr, &error);
  throw SonareException(ErrorCode::InvalidParameter, error);
}

}  // namespace

RealtimeVoiceChanger::RealtimeVoiceChanger(RealtimeVoiceChangerConfig config)
    : config_(checked_config(config)) {
  // The constructed config IS the first request; nothing has resolved a grain
  // yet, so config_ still carries what the caller asked for.
  requested_retune_grain_size_ = config_.retune.grain_size;
  // Derive sample-rate-independent gains/mixes so that config() observers see
  // consistent state even before prepare() is called. The sample-rate-dependent
  // branch inside update_derived() is guarded and will be re-run by prepare().
  update_derived();
  update_latency_mirrors();
  note_queue_.reserve(static_cast<std::size_t>(kNoteQueueCapacity));
  // Publish the initial value so the audio thread can adopt it on the first
  // process_block() call even if set_config() is never invoked.
  config_cell_.store(config_);
  config_version_.fetch_add(1, std::memory_order_release);
}

const char* prepare_arguments_error(double sample_rate, int max_block_size,
                                    int num_channels) noexcept {
  if (!is_valid_realtime_sample_rate(sample_rate))
    return "sample_rate must be finite and within the supported range";
  if (!is_valid_realtime_block_size(max_block_size))
    return "max_block_size must be positive and within the supported maximum";
  if (num_channels < 1 || num_channels > 2) return "num_channels must be 1 or 2";
  return nullptr;
}

void RealtimeVoiceChanger::prepare(double sample_rate, int max_block_size, int num_channels) {
  if (const char* refused = prepare_arguments_error(sample_rate, max_block_size, num_channels))
    throw SonareException(ErrorCode::InvalidParameter, refused);

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  num_channels_ = num_channels;
  prepared_formant_mode_ = config_.formant_mode;
  prepared_engine_ = config_.voice_engine.engine;
  prepared_f0_min_hz_ = config_.voice_engine.f0_min_hz;
  prepared_vocoder_bands_ = config_.vocoder.bands;
  // Events queued for the previous preparation do not carry over.
  note_queue_.reserve(static_cast<std::size_t>(kNoteQueueCapacity));
  notes_pushed_.store(0, std::memory_order_relaxed);
  notes_applied_ = 0;
  all_notes_off_pending_.store(false, std::memory_order_relaxed);
  all_notes_off_at_.store(0, std::memory_order_relaxed);
  channels_.resize(static_cast<std::size_t>(num_channels_));
  scratch_.assign(static_cast<std::size_t>(max_block_size_), 0.0f);
  // Allocation phase: this is the only place buffers may be (re)sized.
  for (auto& channel : channels_) allocate_channel(channel);
  update_derived();
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
  non_finite_discard_count_.reset();
  reset_channels();
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
  // Latest published scalars, so the snapped smoothers match a fresh prepare().
  if (!channels_.empty()) adopt_snapshot_for_block();
  reset_channels();
}

void RealtimeVoiceChanger::reset_channels() {
  for (auto& channel : channels_) {
    reset_channel(channel);
    // Already at rest, so the next block that includes it must not reset it again.
    channel.gate.admit(true, [] {});
  }
  last_gain_reduction_db_.store(0.0f, std::memory_order_relaxed);
}

void RealtimeVoiceChanger::set_config(const RealtimeVoiceChangerConfig& config) {
  // Writer side: validate, update the visible mirror used by config(), and
  // hand the value off to the audio thread via the lock-free seqlock cell.
  // We deliberately DO NOT touch derived scalars or per-channel BiquadState
  // here — those are written by the audio
  // thread inside adopt_snapshot_for_block() so set_config() can race safely
  // with process_block(). Storing into config_cell_ and bumping
  // config_version_ never allocates or locks, so this whole function is
  // realtime-safe to call from the audio thread itself (see the class doc
  // comment on set_config for why WASM needs that); the refusal paths throw.
  const RealtimeVoiceChangerConfig& accepted = checked_config(config);
  if (!channels_.empty() && accepted.formant_mode != prepared_formant_mode_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "formant mode is fixed at prepare(); it cannot change on a live chain");
  }
  if (!channels_.empty() && (accepted.voice_engine.engine != prepared_engine_ ||
                             accepted.voice_engine.f0_min_hz != prepared_f0_min_hz_ ||
                             accepted.vocoder.bands != prepared_vocoder_bands_)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "voiceEngine.engine, voiceEngine.f0MinHz and vocoder.bands are fixed at "
                          "prepare(); they cannot change on a live chain");
  }
  config_ = accepted;
  // Record the request before the effective-value mirror below lands on the
  // same field; prepare() seeds the retune stages from this, not from config_.
  // A grain equal to the effective one is config() read back, not a new request.
  if (channels_.empty() || config_.retune.grain_size != channels_[0].retune.grain_size()) {
    requested_retune_grain_size_ = config_.retune.grain_size;
  }
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

void RealtimeVoiceChanger::update_derived() {
  if (sample_rate_ > 0.0) {
    fast_det_alpha_ = rt::one_pole_lowpass_alpha_matched(kFastDetectorHz, sample_rate_);
    limiter_attack_ = rt::one_pole_alpha_from_time_ms(kLimiterAttackMs, sample_rate_);
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
  StreamingRetuneConfig retune_config = config_.retune;
  retune_config.grain_size = requested_retune_grain_size_;
  state.retune.set_config(retune_config);
  state.retune.prepare(sample_rate_, max_block_size_);
  int aligned_latency = state.retune.latency_samples();
  if (prepared_engine_ == PitchEngine::Psola) {
    aligned_latency = allocate_transform(state);
  } else if (prepared_formant_mode_ == FormantMode::Absolute) {
    const int rate = static_cast<int>(std::lround(sample_rate_));
    state.warp.prepare(formant_warp_frame_size(rate), formant_warp_lpc_order(rate));
    aligned_latency += state.warp.latency_samples();
  }
  state.character.prepare(sample_rate_);
  state.space.prepare(sample_rate_);
  state.dry_delay.assign(static_cast<std::size_t>(aligned_latency), 0.0f);
  state.dry_delay_pos = 0;
  state.formant.prepare(sample_rate_, max_block_size_);
  state.reverb.prepare(sample_rate_, max_block_size_);
  state.input_gain.prepare(sample_rate_, 10.0f);
  state.output_gain.prepare(sample_rate_, 10.0f);
  state.wet_mix.prepare(sample_rate_, kWetMixGlideMs);
  state.eq_highpass_hz.prepare(sample_rate_, kParamSmoothingMs);
  state.eq_body_db.prepare(sample_rate_, kParamSmoothingMs);
  state.eq_presence_db.prepare(sample_rate_, kParamSmoothingMs);
  state.eq_air_db.prepare(sample_rate_, kParamSmoothingMs);
  state.gate_threshold_db.prepare(sample_rate_, kParamSmoothingMs);
  state.gate_attack_ms.prepare(sample_rate_, kParamSmoothingMs);
  state.gate_release_ms.prepare(sample_rate_, kParamSmoothingMs);
  state.gate_range_db.prepare(sample_rate_, kParamSmoothingMs);
  state.comp_threshold_db.prepare(sample_rate_, kParamSmoothingMs);
  state.comp_ratio.prepare(sample_rate_, kParamSmoothingMs);
  state.comp_attack_ms.prepare(sample_rate_, kParamSmoothingMs);
  state.comp_release_ms.prepare(sample_rate_, kParamSmoothingMs);
  state.comp_makeup_db.prepare(sample_rate_, kParamSmoothingMs);
  state.deess_frequency_hz.prepare(sample_rate_, kParamSmoothingMs);
  state.deess_threshold_db.prepare(sample_rate_, kParamSmoothingMs);
  state.deess_ratio.prepare(sample_rate_, kParamSmoothingMs);
  state.deess_range_db.prepare(sample_rate_, kParamSmoothingMs);
  state.limiter_ceiling_db.prepare(sample_rate_, kParamSmoothingMs);
  state.limiter_release_ms.prepare(sample_rate_, kParamSmoothingMs);
  // ISP limiter: prepared unconditionally so toggling
  // LimiterConfig::enable_isp_limiter at runtime never triggers an allocation
  // from the audio thread. Cost is small (one TruePeakFilter history vector +
  // a sliding-max ring, both proportional to max_block_size_).
  state.isp_limiter.prepare(sample_rate_, max_block_size_);
}

int RealtimeVoiceChanger::allocate_transform(ChannelState& state) {
  const int block = max_block_size_;
  state.tracker.prepare(sample_rate_, block, prepared_f0_min_hz_);
  state.shifter.prepare(sample_rate_, block, prepared_f0_min_hz_);
  state.targets.prepare(sample_rate_);
  state.vocoder.prepare(sample_rate_, block, prepared_vocoder_bands_);
  const int hop = std::max(1, state.tracker.hop_input_samples());
  state.estimates.assign(static_cast<std::size_t>(block / hop + 2), F0Estimate{});
  for (auto& voice : state.voice_out) voice.assign(static_cast<std::size_t>(block), 0.0f);
  state.vocoder_out.assign(static_cast<std::size_t>(block), 0.0f);
  const auto ring = static_cast<std::size_t>(
                        std::ceil(static_cast<double>(kHarmonyMaxDelayMs) * 1e-3 * sample_rate_)) +
                    kHarmonyRingMargin;
  for (auto& delay : state.harmony_delay) {
    delay.line.assign(ring, 0.0f);
    delay.write = 0;
  }
  for (auto& delay : state.harmony_delay_samples) delay.prepare(sample_rate_, kParamSmoothingMs);
  for (auto& gain : state.voice_gain) gain.prepare(sample_rate_, kVoiceGainGlideMs);
  state.shift_mix.prepare(sample_rate_, kParamSmoothingMs);
  // Both paths land on the longer of the two delays, so switching the vocoder keeps the latency.
  const int shifter = state.shifter.latency_samples();
  const int vocoder = state.vocoder.group_delay_samples();
  state.unshifted.line.assign(static_cast<std::size_t>(shifter), 0.0f);
  state.voice_compensation.line.assign(static_cast<std::size_t>(std::max(0, vocoder - shifter)),
                                       0.0f);
  state.vocoder_compensation.line.assign(static_cast<std::size_t>(std::max(0, shifter - vocoder)),
                                         0.0f);
  state.unshifted.pos = 0;
  state.voice_compensation.pos = 0;
  state.vocoder_compensation.pos = 0;
  transform_latency_ = std::max(shifter, vocoder);
  return transform_latency_;
}

void RealtimeVoiceChanger::apply_channel_config(ChannelState& state, int channel_index,
                                                const RealtimeVoiceChangerConfig& config) {
  // Sub-component coefficient updates (no buffer resizing). The grain is structural and the
  // request lives in requested_retune_grain_size_, so the stage only ever sees its own size.
  StreamingRetuneConfig retune = config.retune;
  retune.grain_size = state.retune.grain_size();
  state.retune.set_config(retune);
  if (prepared_engine_ == PitchEngine::Psola) {
    // The grain resampling carries the formant factor, so the colour stage keeps only its tones.
    StreamingFormantConfig colour = config.formant;
    colour.factor = 1.0f;
    colour.amount = 1.0f;
    state.formant.set_config(colour);
    const float factor =
        config.formant_mode == FormantMode::Absolute
            ? config.formant.factor
            : effective_formant_factor(config.formant.factor, config.formant.amount);
    state.targets.set_config(config.pitch_correction, config.harmony, config.midi,
                             config.retune.semitones, factor, config.formant_mode);
    state.vocoder.set_config(config.vocoder, factor);
    state.shift_mix.set_target(std::clamp(config.retune.mix, 0.0f, 1.0f));
    const float delay =
        static_cast<float>(static_cast<double>(config.harmony.delay_ms) * 1e-3 * sample_rate_);
    for (auto& smoother : state.harmony_delay_samples) smoother.set_target(delay);
  } else if (prepared_formant_mode_ == FormantMode::Absolute) {
    // The warp carries the factor, so the colour stage stays neutral; amount does not apply.
    StreamingFormantConfig colour = config.formant;
    colour.factor = 1.0f;
    colour.amount = 1.0f;
    state.formant.set_config(colour);
    // The retune shifts only its wet share, so the warp undoes pitch_ratio^mix.
    const double pitch_ratio = std::exp2(static_cast<double>(config.retune.semitones) *
                                         static_cast<double>(config.retune.mix) / 12.0);
    state.warp.set_factor(
        static_cast<float>(static_cast<double>(config.formant.factor) / pitch_ratio));
  } else {
    state.formant.set_config(config.formant);
  }
  state.reverb.set_config(config.reverb, channel_index);
  // A stage that sat out while the other variation ran resumes from rest, not from its old tail.
  const ReverbVariation variation = config.reverb_variation.variation;
  if (variation != state.variation) {
    if (variation == ReverbVariation::Reverb) {
      state.reverb.reset();
    } else if (state.variation == ReverbVariation::Reverb) {
      state.space.reset();
    }
    state.variation = variation;
  }
  state.space.set_config(config.reverb, config.reverb_variation);
  state.character.set_config(config.character);
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

void RealtimeVoiceChanger::update_input_filter(ChannelState& state) noexcept {
  state.hpf.set(input_highpass(sample_rate_, state.filter_highpass_hz));
}

void RealtimeVoiceChanger::update_output_filters(ChannelState& state) noexcept {
  state.body.set(body_peak(sample_rate_, state.filter_body_db));
  state.presence.set(presence_peak(sample_rate_, state.filter_presence_db));
  state.air.set(air_shelf(sample_rate_, state.filter_air_db));
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
    if (config_reader_.try_load_into(&candidate)) {
      active_config_ = candidate;
      const bool next_isp_limiter_active = active_config_.limiter.enable_isp_limiter;
      update_derived();
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

rt::TailBudget RealtimeVoiceChanger::tail_budget() const noexcept {
  rt::TailBudget wet;
  if (channels_.empty() || !(sample_rate_ > 0.0)) return wet;
  // Gains around the filters scale every residual; the limiters only ever reduce it.
  const double scale =
      db_to_linear(static_cast<double>(config_.input_gain_db)) *
      db_to_linear(static_cast<double>(std::max(0.0f, config_.compressor.makeup_gain_db))) *
      db_to_linear(static_cast<double>(config_.output_gain_db));
  const auto ring = [scale](const rt::BiquadCoeffs& c) {
    rt::TailBudget section;
    return section.biquad(
        scale * (std::abs(static_cast<double>(c.b0)) + std::abs(static_cast<double>(c.b1)) +
                 std::abs(static_cast<double>(c.b2))),
        static_cast<double>(c.a1), static_cast<double>(c.a2));
  };
  // The high-pass corner glides down to the lowest accepted one, where it rings longest.
  wet.then(ring(input_highpass(sample_rate_, kVoiceHighpassMinHz)));
  const ChannelState& channel = channels_[0];
  if (prepared_engine_ == PitchEngine::Psola) {
    rt::TailBudget voice;
    voice.delay(kShifterHistoryPeriods * channel.shifter.latency_samples() +
                kShifterHistoryUnvoicedS * sample_rate_);
    voice.delay(static_cast<double>(kHarmonyMaxDelayMs) * 1e-3 * sample_rate_);
    if (config_.vocoder.enabled) {
      // Each band envelope falls at its release once the modulator stops.
      rt::TailBudget vocoded;
      vocoded.delay(kVocoderRingGroupDelays * channel.vocoder.group_delay_samples());
      vocoded.decay(1.0 - time_to_attack_release_rate(sample_rate_, config_.vocoder.release_ms));
      voice.alongside(vocoded);
    }
    wet.then(voice);
  } else {
    if (prepared_formant_mode_ == FormantMode::Absolute) {
      wet.delay(static_cast<double>(channel.warp.latency_samples()));
    }
    wet.then(channel.retune.tail());
  }
  wet.then(StreamingFormant::tail(sample_rate_));
  if (config_.character.type != CharacterType::None) {
    rt::TailBudget character;
    wet.then(character.seconds(kCharacterTailS, sample_rate_));
  }
  rt::TailBudget body;
  rt::TailBudget presence;
  rt::TailBudget air;
  for (const float gain_db : {-kVoiceEqGainLimitDb, kVoiceEqGainLimitDb}) {
    body.alongside(ring(body_peak(sample_rate_, gain_db)));
    presence.alongside(ring(presence_peak(sample_rate_, gain_db)));
    air.alongside(ring(air_shelf(sample_rate_, gain_db)));
  }
  wet.then(body).then(presence).then(air);
  if (config_.reverb_variation.variation == ReverbVariation::Reverb) {
    wet.then(StreamingReverb::tail(config_.reverb, sample_rate_, num_channels_, scale));
  } else {
    // Each pass of the echo loop is scaled by the feedback; the first is heard at the mix.
    const double delay =
        static_cast<double>(space_delay_ms(config_.reverb_variation)) * 1e-3 * sample_rate_;
    rt::TailBudget echo;
    echo.delay(delay).recirculation(delay, static_cast<double>(config_.reverb_variation.feedback),
                                    scale * static_cast<double>(config_.reverb.mix));
    wet.then(echo);
  }
  if (config_.wet_mix > 0.0f) return wet;
  // A mix gliding to zero still lets the wet path through until it lands.
  rt::TailBudget glide;
  glide.decay(1.0 - time_to_attack_release_rate(sample_rate_, kWetMixGlideMs), scale);
  return rt::TailBudget::reported(std::min(wet.samples(), glide.samples()));
}

void RealtimeVoiceChanger::reset_channel(ChannelState& state) {
  state.retune.reset();
  state.warp.reset();
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
  state.comp_reduction_gain = 1.0f;
  state.deess_env = 0.0f;
  state.deess_gain = 1.0f;
  state.limiter_gain = 1.0f;
  state.output_limiter_substituted = false;
  state.control_cadence.reset();
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
  state.gate_threshold_linear = db_to_gain(state.gate_threshold_db.current());
  state.gate_range_linear = db_to_gain(-state.gate_range_db.current());
  state.gate_attack_alpha =
      rt::one_pole_alpha_from_time_ms(state.gate_attack_ms.current(), sample_rate_);
  state.gate_release_alpha =
      rt::one_pole_alpha_from_time_ms(state.gate_release_ms.current(), sample_rate_);
  state.comp_attack_alpha =
      rt::one_pole_alpha_from_time_ms(state.comp_attack_ms.current(), sample_rate_);
  state.comp_release_alpha =
      rt::one_pole_alpha_from_time_ms(state.comp_release_ms.current(), sample_rate_);
  state.limiter_ceiling_linear = db_to_gain(state.limiter_ceiling_db.current());
  state.limiter_release_alpha =
      rt::one_pole_alpha_from_time_ms(state.limiter_release_ms.current(), sample_rate_);
  update_input_filter(state);
  update_output_filters(state);
  state.reverb.reset();
  std::fill(state.dry_delay.begin(), state.dry_delay.end(), 0.0f);
  state.dry_delay_pos = 0;
  state.isp_limiter.reset();
  state.character.reset();
  state.space.reset();
  state.tracker.reset();
  state.shifter.reset();
  state.targets.reset();
  state.vocoder.reset();
  for (auto& delay : state.harmony_delay) {
    std::fill(delay.line.begin(), delay.line.end(), 0.0f);
    delay.write = 0;
  }
  for (auto& delay : state.harmony_delay_samples) delay.reset(delay.target());
  // Voices enter from silence; the first block's targets raise the gains.
  for (auto& gain : state.voice_gain) gain.reset(0.0f);
  state.shift_mix.reset(state.shift_mix.target());
  state.unshifted.clear();
  state.voice_compensation.clear();
  state.vocoder_compensation.clear();
  state.vocoder_running = false;
  state.control_phase = 0;
  state.held_voice = {};
  state.transform_substituted = false;
  state.voice_input_substituted = false;
}

bool RealtimeVoiceChanger::push_note(NoteEvent event) noexcept {
  if (channels_.empty() || prepared_engine_ != PitchEngine::Psola) return false;
  if (!note_queue_.push(event)) return false;
  notes_pushed_.fetch_add(1, std::memory_order_release);
  return true;
}

bool RealtimeVoiceChanger::note_on(int note, float velocity) noexcept {
  if (!is_valid_note(note) || !is_valid_velocity(velocity)) return false;
  return push_note(NoteEvent{NoteEvent::Kind::On, note, velocity});
}

bool RealtimeVoiceChanger::note_off(int note) noexcept {
  if (!is_valid_note(note)) return false;
  return push_note(NoteEvent{NoteEvent::Kind::Off, note, 0.0f});
}

void RealtimeVoiceChanger::all_notes_off() noexcept {
  // Stamped with the events pushed so far: those apply first, later ones after the release.
  all_notes_off_at_.store(notes_pushed_.load(std::memory_order_acquire), std::memory_order_release);
  all_notes_off_pending_.store(true, std::memory_order_release);
}

void RealtimeVoiceChanger::drain_note_events() noexcept {
  const bool release_pending = all_notes_off_pending_.exchange(false, std::memory_order_acq_rel);
  const std::uint64_t release_at =
      release_pending ? all_notes_off_at_.load(std::memory_order_acquire) : 0;
  bool released = !release_pending;
  const auto release_all = [this] {
    for (auto& channel : channels_) channel.targets.all_notes_off();
  };
  NoteEvent event{};
  while (note_queue_.pop(event)) {
    if (!released && notes_applied_ >= release_at) {
      release_all();
      released = true;
    }
    for (auto& channel : channels_) channel.targets.apply_note(event);
    ++notes_applied_;
  }
  if (!released) release_all();
}

}  // namespace sonare::editing::voice_changer
