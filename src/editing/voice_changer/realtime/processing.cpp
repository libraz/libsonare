#include <algorithm>
#include <cassert>
#include <cmath>

#include "editing/voice_changer/realtime.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::editing::voice_changer {
namespace {

float db_to_gain(float db) noexcept { return sonare::db_to_linear(db); }

float amp_to_db(float amp) noexcept {
  return sonare::linear_to_db(std::max(std::abs(amp), sonare::constants::kAmpEpsilon));
}

inline float smooth_attack_release(float& state, float target, float attack_alpha,
                                   float release_alpha, bool attack_when_decreasing) noexcept {
  const bool use_attack = attack_when_decreasing ? target < state : target > state;
  const float alpha = use_attack ? attack_alpha : release_alpha;
  state += alpha * (target - state);
  return state;
}

}  // namespace

float RealtimeVoiceChanger::process_input_stage(ChannelState& state, float input,
                                                bool control_update) noexcept {
  // Coefficients and derived detector controls change at an absolute
  // 32-sample cadence. The parameter smoothers below still advance every
  // sample, so a block boundary cannot restart or skip a transition.
  constexpr float kFilterEpsilon = 1.0e-4f;
  const float highpass_hz = state.eq_highpass_hz.process();
  state.gate_threshold_db.process();
  state.gate_attack_ms.process();
  state.gate_release_ms.process();
  state.gate_range_db.process();
  if (control_update) {
    if (std::abs(highpass_hz - state.filter_highpass_hz) > kFilterEpsilon) {
      state.filter_highpass_hz = highpass_hz;
      update_input_filter(state);
    }

    // Cache the gate's expensive dB-derived controls. The detector and gate
    // gain continue to run at the audio sample rate below.
    state.gate_threshold_linear = db_to_gain(state.gate_threshold_db.current());
    state.gate_range_linear = db_to_gain(-state.gate_range_db.current());
    state.gate_attack_alpha =
        rt::one_pole_alpha_from_time_ms(state.gate_attack_ms.current(), sample_rate_);
    state.gate_release_alpha =
        rt::one_pole_alpha_from_time_ms(state.gate_release_ms.current(), sample_rate_);
  }

  // Apply input gain then a 2nd-order HPF. The HPF (highpass_hz >= 20) already
  // removes DC, so no separate DC blocker is needed.
  float x = state.hpf.process(input * state.input_gain.process());

  // Noise gate.
  //   1. A fixed fast detector (~0.8 ms) follows |x| so the level estimate
  //      tracks transients without being delayed by the user's A/R settings.
  //   2. The gate gain itself is exponentially smoothed using the
  //      user-configured attack (when opening) / release (when closing).
  //      Smoothing the *gain* — not just the detector — is what eliminates
  //      the zipper noise that a hard threshold-cross produces.
  const float env_in = std::abs(x);
  state.gate_env += fast_det_alpha_ * (env_in - state.gate_env);
  const float gate_target =
      state.gate_env < state.gate_threshold_linear ? state.gate_range_linear : 1.0f;
  smooth_attack_release(state.gate_gain, gate_target, state.gate_attack_alpha,
                        state.gate_release_alpha, /*attack_when_decreasing=*/false);
  x *= state.gate_gain;
  return x;
}

float RealtimeVoiceChanger::process_output_stage(ChannelState& state, float input,
                                                 bool control_update) noexcept {
  // These controls are consumed by this stage, so advance their smoothers
  // immediately before the first sample that uses them. Keeping this work out
  // of process_input_stage prevents a large caller block from making its tail
  // coefficients affect the block's leading output samples.
  constexpr float kFilterEpsilon = 1.0e-4f;
  const float body_db = state.eq_body_db.process();
  const float presence_db = state.eq_presence_db.process();
  const float air_db = state.eq_air_db.process();
  const float deess_frequency_hz = state.deess_frequency_hz.process();
  if (control_update) {
    if (std::abs(body_db - state.filter_body_db) > kFilterEpsilon ||
        std::abs(presence_db - state.filter_presence_db) > kFilterEpsilon ||
        std::abs(air_db - state.filter_air_db) > kFilterEpsilon ||
        std::abs(deess_frequency_hz - state.filter_deess_frequency_hz) > kFilterEpsilon) {
      state.filter_body_db = body_db;
      state.filter_presence_db = presence_db;
      state.filter_air_db = air_db;
      state.filter_deess_frequency_hz = deess_frequency_hz;
      update_output_filters(state);
    }
  }

  float x = input;
  x = state.body.process(x);
  x = state.presence.process(x);
  x = state.air.process(x);

  // Compressor: feed-forward with ratio-based reduction.
  //   Detection uses the same fast follower as the gate so the user's
  //   attack/release apply to the *gain* only — a single-stage LP. The
  //   earlier double-smoothing (detector A/R + gain A/R with the same
  //   coefficients) stretched the effective time constant ~2x and made
  //   the user-set attack feel sluggish.
  const float comp_env_in = std::abs(x);
  state.comp_env += fast_det_alpha_ * (comp_env_in - state.comp_env);
  const float comp_threshold_db = state.comp_threshold_db.process();
  const float comp_ratio = state.comp_ratio.process();
  const float comp_makeup_db = state.comp_makeup_db.process();
  const float comp_attack_ms = state.comp_attack_ms.process();
  const float comp_release_ms = state.comp_release_ms.process();
  if (control_update) {
    state.comp_attack_alpha = rt::one_pole_alpha_from_time_ms(comp_attack_ms, sample_rate_);
    state.comp_release_alpha = rt::one_pole_alpha_from_time_ms(comp_release_ms, sample_rate_);
  }
  // The envelope changes at the audio rate. Keep its gain target on that same
  // path so a static configuration remains numerically identical to the
  // established compressor recurrence; only config-derived A/R transforms are
  // refreshed at the bounded control cadence above.
  const float over = amp_to_db(state.comp_env) - comp_threshold_db;
  float comp_target = 1.0f;
  if (over > 0.0f) {
    const float reduction_db = over - over / comp_ratio;
    comp_target = db_to_gain(-reduction_db + comp_makeup_db);
  } else {
    comp_target = db_to_gain(comp_makeup_db);
  }
  smooth_attack_release(state.comp_gain, comp_target, state.comp_attack_alpha,
                        state.comp_release_alpha, /*attack_when_decreasing=*/true);
  x *= state.comp_gain;

  // De-esser: ratio-based broadband reduction triggered by the sibilance
  // band-pass. The kDeessEnvelopeHz LP gives a fast (~1.6 ms) detector, and
  // kDeessGainSmoothingHz smooths the gain itself (~0.8 ms) so the two stages
  // serve distinct purposes — detector tracking vs gain dezippering.
  const float ess = std::abs(state.deess_band.process(x));
  state.deess_env += deess_alpha_ * (ess - state.deess_env);
  const float deess_threshold_db = state.deess_threshold_db.process();
  const float deess_ratio = state.deess_ratio.process();
  const float deess_range_db = state.deess_range_db.process();
  // As for compression, detector output is signal-dependent and must not be
  // held between control ticks. The gain smoother coefficient is already
  // precomputed once in prepare(), matching the original sample recurrence.
  const float ess_over = amp_to_db(state.deess_env) - deess_threshold_db;
  float deess_target = 1.0f;
  if (ess_over > 0.0f) {
    const float reduction_db = std::min(deess_range_db, ess_over - ess_over / deess_ratio);
    deess_target = db_to_gain(-reduction_db);
  }
  state.deess_gain += deess_gain_alpha_ * (deess_target - state.deess_gain);
  x *= state.deess_gain;

  // Reverb: variable-length Schroeder reverb (2 combs + 1 series allpass).
  // Implementation lives in streaming_reverb.{h,cpp}; the helper handles
  // wet/dry mix internally and returns the mixed signal.
  x = state.reverb.process_sample(x);

  // Output gain + simple peak limiter. Not a true-peak (inter-sample) limiter
  // — the schema cap on ceilingDb keeps typical material safe without 4x
  // oversampling. Attack is sub-millisecond (kLimiterAttackMs) so transient
  // bursts taper across ~5 samples instead of a single-sample step (audibly
  // clicks); the final clamp absorbs the residual peak over that taper.
  x *= state.output_gain.process();
  const float limiter_ceiling_db = state.limiter_ceiling_db.process();
  const float limiter_release_ms = state.limiter_release_ms.process();
  if (control_update) {
    state.limiter_ceiling_linear = db_to_gain(limiter_ceiling_db);
    state.limiter_release_alpha = rt::one_pole_alpha_from_time_ms(limiter_release_ms, sample_rate_);
  }
  const float ceiling = state.limiter_ceiling_linear;
  const float abs_x = std::abs(x);
  const float limit_target =
      abs_x > ceiling ? ceiling / std::max(abs_x, sonare::constants::kAmpEpsilon) : 1.0f;
  smooth_attack_release(state.limiter_gain, limit_target, limiter_attack_,
                        state.limiter_release_alpha,
                        /*attack_when_decreasing=*/true);
  return std::clamp(x * state.limiter_gain, -ceiling, ceiling);
}

void RealtimeVoiceChanger::ensure_scratch(int num_samples) noexcept {
  // RT-safe: prepare() always allocates max_block_size_ samples up front, so
  // the scratch buffer is guaranteed to be large enough whenever process_block
  // accepts the request. The caller MUST have already validated
  // num_samples <= max_block_size_ — we never resize here (an audio-thread
  // resize would risk priority inversion).
  assert(num_samples <= max_block_size_);
  (void)num_samples;
}

void RealtimeVoiceChanger::process_block(const float* input, float* output,
                                         int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops to keep this RT-safe (no throw,
  // no allocation). When sample_rate_ is unset we still zero-fill the output
  // so callers observe a defined buffer state rather than uninitialised memory.
  if (num_samples <= 0) return;
  if (sample_rate_ <= 0.0) {
    if (output != nullptr) std::fill_n(output, num_samples, 0.0f);
    return;
  }
  if (num_samples > max_block_size_) return;
  if (input == nullptr || output == nullptr) return;
  // Reuse the multi-channel path with channels=1 by staging the dry input in
  // the output buffer and passing that as the channel pointer. The
  // multi-channel path uses its own internal scratch_, so reading the
  // "dry" sample back from channels[0][i] still observes the original input
  // (the wet/dry mix would otherwise read the input-stage-processed signal).
  if (input != output) {
    std::copy_n(input, num_samples, output);
  }
  float* channel_ptr = output;
  process_block(&channel_ptr, 1, num_samples);
}

void RealtimeVoiceChanger::process_block(float* const* channels, int num_channels,
                                         int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops; caller-owned planar buffers
  // are left untouched (we do not know their channel layout to safely zero).
  if (num_samples <= 0) return;
  if (sample_rate_ <= 0.0) return;
  if (channels == nullptr) return;
  if (num_channels < 1 || num_channels > num_channels_) return;
  if (num_samples > max_block_size_) return;
  ensure_scratch(num_samples);
  // Adopt the latest published configuration snapshot exactly once at block
  // start. After this point the per-sample loop reads from a stable const
  // reference; the control thread cannot mutate any field the loop touches
  // because set_config() only writes to config_ + publishes a NEW snapshot
  // (the audio thread keeps owning the previously-adopted one).
  const RealtimeVoiceChangerConfig& config = adopt_snapshot_for_block();
  for (int ch = 0; ch < num_channels; ++ch) {
    // Skip null channel pointers (caller's responsibility) rather than aborting
    // the whole block: a null right pointer must not leave the left output
    // buffer untouched / undefined.
    if (channels[ch] == nullptr) continue;
    auto& channel = channels_[static_cast<std::size_t>(ch)];
    const std::uint64_t block_start = channel.control_cadence.sample_position();
    for (int i = 0; i < num_samples; ++i) {
      // Sanitize non-finite (NaN/Inf) input to silence before it can enter any
      // IIR state. A single upstream NaN would otherwise poison the HPF/EQ
      // biquads, the retune history ring, the formant filter, and the reverb
      // permanently (until reset()), because each recirculates its own output
      // as state. This must be RT-safe (no throw/alloc), so we flush the sample
      // to 0 rather than rejecting the block. The cleaned value is written back
      // in place so the dry read in the mix loop below stays finite too. Finite
      // samples pass through bit-identically (same value, same bits).
      const float raw = channels[ch][i];
      const float clean = std::isfinite(raw) ? raw : 0.0f;
      channels[ch][i] = clean;
      const bool control_update = channel.control_cadence.advance();
      scratch_[i] = process_input_stage(channel, clean, control_update);
    }
    channel.retune.process_block(scratch_.data(), scratch_.data(), num_samples);
    channel.formant.process_block(scratch_.data(), scratch_.data(), num_samples);
    for (int i = 0; i < num_samples; ++i) {
      float delayed_dry = channels[ch][i];
      if (!channel.dry_delay.empty()) {
        delayed_dry = channel.dry_delay[channel.dry_delay_pos];
        channel.dry_delay[channel.dry_delay_pos] = channels[ch][i];
        channel.dry_delay_pos = (channel.dry_delay_pos + 1) % channel.dry_delay.size();
      }
      const bool control_update =
          ControlCadence::is_due(block_start + static_cast<std::uint64_t>(i));
      const float wet = process_output_stage(channel, scratch_[i], control_update);
      const float wet_mix = channel.wet_mix.process();
      channels[ch][i] = delayed_dry * (1.0f - wet_mix) + wet * wet_mix;
    }
    // Final inter-sample-peak limiter — applied after the aligned dry/wet mix.
    // It stays active at wet_mix == 0 so toggling the mix cannot introduce a
    // second latency discontinuity in addition to the deliberately aligned
    // retune paths.
    if (config.limiter.enable_isp_limiter) {
      channel.isp_limiter.process_block(channels[ch], num_samples);
    }
  }
}

int RealtimeVoiceChanger::latency_samples() const noexcept {
  if (channels_.empty()) return 0;
  // Both retune and whole-chain dry paths are aligned to the OLA latency, so
  // hosts see one fixed delay regardless of either mix control. Biquad/formant
  // group delays are intentionally omitted (<= 8 samples combined).
  int latency = channels_[0].retune.latency_samples();
  if (latency_isp_enabled_.load(std::memory_order_relaxed)) {
    latency += channels_[0].isp_limiter.latency_samples();
  }
  return latency;
}

}  // namespace sonare::editing::voice_changer
