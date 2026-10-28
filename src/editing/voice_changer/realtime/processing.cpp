#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "editing/voice_changer/realtime.h"
#include "rt/fractional_delay.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/non_finite_sample.h"
#include "util/non_finite_state.h"

namespace sonare::editing::voice_changer {
namespace {

using sonare::discard_group_if_non_finite;
using sonare::discard_if_non_finite;

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

  // Character colouration ahead of the EQ. A non-finite value leaving it marks its state for a
  // reset; the value itself continues to the limiter, which handles it as before.
  float x = state.character.process_sample(input);
  if (!std::isfinite(x)) state.transform_substituted = true;
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
  float comp_target = db_to_gain(comp_makeup_db);
  float comp_reduction_target = 1.0f;
  if (over > 0.0f) {
    const float reduction_db = over - over / comp_ratio;
    comp_target = db_to_gain(-reduction_db + comp_makeup_db);
    comp_reduction_target = db_to_gain(-reduction_db);
  }
  smooth_attack_release(state.comp_gain, comp_target, state.comp_attack_alpha,
                        state.comp_release_alpha, /*attack_when_decreasing=*/true);
  // Same smoothing without the make-up, so the meter never reads the make-up ramp.
  smooth_attack_release(state.comp_reduction_gain, comp_reduction_target, state.comp_attack_alpha,
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
  // wet/dry mix internally and returns the mixed signal. The echo-family
  // variations run in the space stage instead, with the same mix.
  if (state.variation == ReverbVariation::Reverb) {
    x = state.reverb.process_sample(x);
  } else {
    x = state.space.process_sample(x);
    if (!std::isfinite(x)) state.transform_substituted = true;
  }

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
  // std::clamp folds an infinity onto the ceiling, which is a sample this stage
  // could legitimately have produced; a NaN answers false to both comparisons
  // and propagates instead. Record the fold so the block's discard report covers
  // this stage by construction, rather than by the recurrence that produced the
  // value happening to be caught as well.
  const float limited = x * state.limiter_gain;
  if (std::isinf(limited)) state.output_limiter_substituted = true;
  return std::clamp(limited, -ceiling, ceiling);
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

void RealtimeVoiceChanger::process_transform(ChannelState& state,
                                             const RealtimeVoiceChangerConfig& config,
                                             const float* carrier, int num_samples) noexcept {
  float* x = scratch_.data();
  // The tracker, the shifter and the vocoder all keep history; keep a non-finite value out. The
  // substitution is reported for the block, and the stages need no reset: none of them saw it.
  for (int i = 0; i < num_samples; ++i) {
    if (!std::isfinite(x[i])) {
      x[i] = 0.0f;
      state.voice_input_substituted = true;
    }
  }
  const int estimates = state.tracker.process(x, num_samples, state.estimates.data(),
                                              static_cast<int>(state.estimates.size()));
  state.shifter.push_estimates(state.estimates.data(), estimates);
  const VoiceTargets targets = state.targets.update(state.estimates.data(), estimates, num_samples);
  float* voice_out[kMaxVoices];
  for (int v = 0; v < kMaxVoices; ++v)
    voice_out[v] = state.voice_out[static_cast<size_t>(v)].data();
  state.shifter.process(x, targets, voice_out, num_samples);

  const bool vocoder_on = config.vocoder.enabled;
  if (vocoder_on && !state.vocoder_running) {
    // Switched on: the bank and its alignment line start from rest.
    state.vocoder.reset();
    state.vocoder_compensation.clear();
  }
  state.vocoder_running = vocoder_on;
  if (vocoder_on) {
    const float* external =
        config.vocoder.carrier_source == VocoderCarrierSource::External ? carrier : nullptr;
    state.vocoder.process(x, external, targets, state.vocoder_out.data(), num_samples);
  }

  const int count = std::clamp(targets.count, 0, kMaxVoices);
  for (int v = 0; v < kMaxVoices; ++v) {
    const VoiceTarget& target = targets.voices[v];
    state.voice_gain[static_cast<size_t>(v)].set_target(v < count && target.active ? target.gain
                                                                                   : 0.0f);
  }
  for (int h = 0; h < kMaxHarmonyVoices; ++h) {
    if (h + 1 < count) {
      state.harmony_delay_samples[static_cast<size_t>(h)].set_target(static_cast<float>(
          static_cast<double>(targets.voices[h + 1].delay_ms) * 1e-3 * sample_rate_));
    }
  }

  for (int k = 0; k < num_samples; ++k) {
    float sum = state.voice_gain[0].process() * (count > 0 ? voice_out[0][k] : 0.0f);
    for (int h = 0; h < kMaxHarmonyVoices; ++h) {
      DelayRing& ring = state.harmony_delay[static_cast<size_t>(h)];
      // A voice beyond the count feeds silence, so its delayed tail fades with its gain.
      ring.line[ring.write] = h + 1 < count ? voice_out[h + 1][k] : 0.0f;
      const float delay = state.harmony_delay_samples[static_cast<size_t>(h)].process();
      const float delayed = rt::lagrange3_read(ring.line.data(), ring.line.size(), ring.write,
                                               static_cast<int>(std::lround(delay * 256.0f)));
      ring.write = ring.write + 1 == ring.line.size() ? 0 : ring.write + 1;
      sum += state.voice_gain[static_cast<size_t>(h + 1)].process() * delayed;
    }
    const float mix = state.shift_mix.process();
    const float unshifted = state.unshifted.process(x[k]);
    const float voice = state.voice_compensation.process(mix * sum + (1.0f - mix) * unshifted);
    float y = vocoder_on ? state.vocoder_compensation.process(state.vocoder_out[k]) : voice;
    if (!std::isfinite(y)) {
      y = 0.0f;
      state.transform_substituted = true;
    }
    x[k] = y;
  }
}

bool RealtimeVoiceChanger::process_block(const float* input, float* output,
                                         int num_samples) noexcept {
  return process_block(input, nullptr, output, num_samples);
}

bool RealtimeVoiceChanger::process_block(const float* input, const float* carrier, float* output,
                                         int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops to keep this RT-safe (no throw,
  // no allocation). When sample_rate_ is unset we still zero-fill the output
  // so callers observe a defined buffer state rather than uninitialised memory.
  if (num_samples == 0) return true;
  if (num_samples < 0) return false;
  if (sample_rate_ <= 0.0) {
    if (output != nullptr) std::fill_n(output, num_samples, 0.0f);
    return false;
  }
  if (num_samples > max_block_size_) return false;
  if (input == nullptr || output == nullptr) return false;
  const RealtimeVoiceChangerConfig& config = adopt_snapshot_for_block();
  // Refused before the output is touched: the caller's buffer keeps what it held.
  if (takes_external_carrier(config) && carrier == nullptr) return false;
  // Reuse the multi-channel path with channels=1 by staging the dry input in
  // the output buffer and passing that as the channel pointer. The
  // multi-channel path uses its own internal scratch_, so reading the
  // "dry" sample back from channels[0][i] still observes the original input
  // (the wet/dry mix would otherwise read the input-stage-processed signal).
  if (input != output) {
    std::copy_n(input, num_samples, output);
  }
  float* channel_ptr = output;
  run_block(&channel_ptr, 1, carrier, num_samples, config);
  return true;
}

bool RealtimeVoiceChanger::process_block(float* const* channels, int num_channels,
                                         int num_samples) noexcept {
  return process_block(channels, num_channels, nullptr, num_samples);
}

bool RealtimeVoiceChanger::process_block(float* const* channels, int num_channels,
                                         const float* carrier, int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops; caller-owned planar buffers
  // are left untouched (we do not know their channel layout to safely zero).
  if (num_samples == 0) return true;
  if (num_samples < 0) return false;
  if (sample_rate_ <= 0.0) return false;
  if (channels == nullptr) return false;
  if (num_channels < 1 || num_channels > num_channels_) return false;
  if (num_samples > max_block_size_) return false;
  // Adopt the latest published configuration snapshot exactly once at block
  // start. After this point the per-sample loop reads from a stable const
  // reference; the control thread cannot mutate any field the loop touches
  // because set_config() only writes to config_ + publishes a NEW snapshot
  // (the audio thread keeps owning the previously-adopted one).
  const RealtimeVoiceChangerConfig& config = adopt_snapshot_for_block();
  if (takes_external_carrier(config) && carrier == nullptr) return false;
  run_block(channels, num_channels, carrier, num_samples, config);
  return true;
}

void RealtimeVoiceChanger::run_block(float* const* channels, int num_channels, const float* carrier,
                                     int num_samples,
                                     const RealtimeVoiceChangerConfig& config) noexcept {
  ensure_scratch(num_samples);
  const bool psola = prepared_engine_ == PitchEngine::Psola;
  bool discarded = false;
  float deepest_reduction_linear = 1.0f;
  // A channel this block leaves out re-enters from rest instead of resuming its old history.
  for (int ch = 0; ch < num_channels_; ++ch) {
    auto& channel = channels_[static_cast<std::size_t>(ch)];
    const bool active = ch < num_channels && channels[ch] != nullptr;
    channel.gate.admit(active, [&] { reset_channel(channel); });
  }
  // Note events take effect at the block boundary, after a re-entering channel was reset.
  if (psola) drain_note_events();
  for (int ch = 0; ch < num_channels; ++ch) {
    // Skip null channel pointers (caller's responsibility) rather than aborting
    // the whole block: a null right pointer must not leave the left output
    // buffer untouched / undefined.
    if (channels[ch] == nullptr) continue;
    auto& channel = channels_[static_cast<std::size_t>(ch)];
    const std::uint64_t block_start = channel.control_cadence.sample_position();
    // The substitution below succeeds, so the non-finite value never reaches a
    // state cell and discard_non_finite_state() cannot see it. The flag is what
    // carries it to the block's report instead.
    bool input_substituted = false;
    for (int i = 0; i < num_samples; ++i) {
      // Non-finite input is flushed to silence rather than refused: every stage
      // below recirculates its own output, so one NaN would hold the HPF/EQ
      // biquads, the retune ring, the formant filter and the reverb until
      // reset(), and this runs where a throw is not available. Written back in
      // place so the dry read in the mix loop stays finite too. It keeps a
      // non-finite sample out of the state and does NOT bound what the state
      // can become -- a finite sample large enough leaves float range inside the
      // highpass recurrence on the next line. discard_non_finite_state is what
      // catches that, one block later.
      float clean = channels[ch][i];
      if (resolve_non_finite(SampleDestination::kRecursiveState, clean)) input_substituted = true;
      channels[ch][i] = clean;
      const bool control_update = channel.control_cadence.advance();
      scratch_[i] = process_input_stage(channel, clean, control_update);
    }
    if (psola) {
      process_transform(channel, config, carrier, num_samples);
    } else {
      if (prepared_formant_mode_ == FormantMode::Absolute) {
        channel.warp.process(scratch_.data(), scratch_.data(), num_samples);
      }
      channel.retune.process_block(scratch_.data(), scratch_.data(), num_samples);
    }
    // The vocoder replaces the voice stage, formant colour included.
    if (!(psola && config.vocoder.enabled)) {
      channel.formant.process_block(scratch_.data(), scratch_.data(), num_samples);
    }
    float lowest_dynamics_gain = std::numeric_limits<float>::infinity();
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
      lowest_dynamics_gain =
          std::min(lowest_dynamics_gain,
                   channel.comp_reduction_gain * channel.deess_gain * channel.limiter_gain);
      const float wet_mix = channel.wet_mix.process();
      channels[ch][i] = delayed_dry * (1.0f - wet_mix) + wet * wet_mix;
    }
    deepest_reduction_linear =
        std::min(deepest_reduction_linear, std::min(1.0f, lowest_dynamics_gain));
    // Final inter-sample-peak limiter — applied after the aligned dry/wet mix.
    // It stays active at wet_mix == 0 so toggling the mix cannot introduce a
    // second latency discontinuity in addition to the deliberately aligned
    // retune paths.
    if (config.limiter.enable_isp_limiter) {
      channel.isp_limiter.process_block(channels[ch], num_samples);
    }
    // Accumulated across the channels rather than bumped inside the loop: the
    // channel count is a property of the buffer the caller passed, not of the
    // work they asked for.
    const bool state_discarded = discard_non_finite_state(channel);
    discarded |= state_discarded || input_substituted;
  }
  if (discarded) non_finite_discard_count_.bump();
  const float reduction_db =
      std::isfinite(deepest_reduction_linear) ? amp_to_db(deepest_reduction_linear) : 0.0f;
  last_gain_reduction_db_.store(std::min(0.0f, reduction_db), std::memory_order_relaxed);
}

bool RealtimeVoiceChanger::discard_non_finite_state(ChannelState& state) noexcept {
  // Seventeen floats per channel, once per block. The retune ring and the OLA
  // accumulators are read at an offset rather than fed back, so they flush on
  // their own; the formant and ISP stages own their cells and clear them there.
  // Every call runs: | rather than || so no cell is skipped once one reports.
  bool discarded = discard_group_if_non_finite(state.hpf.z1, state.hpf.z2);
  discarded |= discard_group_if_non_finite(state.body.z1, state.body.z2);
  discarded |= discard_group_if_non_finite(state.presence.z1, state.presence.z2);
  discarded |= discard_group_if_non_finite(state.air.z1, state.air.z2);
  discarded |= discard_group_if_non_finite(state.deess_band.z1, state.deess_band.z2);
  // A detector and the gain it drives recover together: the gain a discarded
  // detector produced is not usable either, and unity is where each rests.
  if (discard_if_non_finite(state.gate_env, 0.0f)) {
    state.gate_gain = 1.0f;
    discarded = true;
  }
  discarded |= discard_if_non_finite(state.gate_gain, 1.0f);
  if (discard_if_non_finite(state.comp_env, 0.0f)) {
    state.comp_gain = 1.0f;
    state.comp_reduction_gain = 1.0f;
    discarded = true;
  }
  discarded |= discard_if_non_finite(state.comp_gain, 1.0f);
  discarded |= discard_if_non_finite(state.comp_reduction_gain, 1.0f);
  if (discard_if_non_finite(state.deess_env, 0.0f)) {
    state.deess_gain = 1.0f;
    discarded = true;
  }
  discarded |= discard_if_non_finite(state.deess_gain, 1.0f);
  // The output limiter folded an infinity onto its ceiling, so the gain it drove
  // to was derived from a magnitude the input cannot explain.
  if (state.output_limiter_substituted) {
    state.output_limiter_substituted = false;
    state.limiter_gain = 1.0f;
    discarded = true;
  }
  discarded |= discard_if_non_finite(state.limiter_gain, 1.0f);
  discarded |= state.formant.discard_non_finite();
  discarded |= state.isp_limiter.discard_non_finite();
  // The reverb's bound is one comb delay period rather than this block; see
  // StreamingReverb::discard_non_finite.
  discarded |= state.reverb.discard_non_finite();
  if (state.voice_input_substituted) {
    state.voice_input_substituted = false;
    discarded = true;
  }
  // The transformer stages own their history without a discard of their own, so a non-finite
  // value that one of them produced returns all of them to rest.
  if (state.transform_substituted) {
    state.transform_substituted = false;
    state.character.reset();
    state.space.reset();
    state.tracker.reset();
    state.shifter.reset();
    state.vocoder.reset();
    discarded = true;
  }
  return discarded;
}

int RealtimeVoiceChanger::latency_samples() const noexcept {
  if (channels_.empty()) return 0;
  // Both retune and whole-chain dry paths are aligned to the OLA latency, so
  // hosts see one fixed delay regardless of either mix control. Biquad/formant
  // group delays are intentionally omitted (<= 8 samples combined). The psola
  // voice stage and the vocoder are aligned to the longer of their two delays.
  int latency = prepared_engine_ == PitchEngine::Psola
                    ? transform_latency_
                    : channels_[0].retune.latency_samples() + channels_[0].warp.latency_samples();
  if (latency_isp_enabled_.load(std::memory_order_relaxed)) {
    latency += channels_[0].isp_limiter.latency_samples();
  }
  return latency;
}

}  // namespace sonare::editing::voice_changer
