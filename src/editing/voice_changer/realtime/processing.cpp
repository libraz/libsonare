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

float RealtimeVoiceChanger::process_input_stage(ChannelState& state,
                                                const RealtimeVoiceChangerConfig& config,
                                                float input) noexcept {
  // Apply input gain then a 2nd-order HPF. The HPF (highpass_hz >= 20) already
  // removes DC, so no separate DC blocker is needed.
  float x = state.hpf.process(input * input_gain_);

  // Noise gate.
  //   1. A fixed fast detector (~0.8 ms) follows |x| so the level estimate
  //      tracks transients without being delayed by the user's A/R settings.
  //   2. The gate gain itself is exponentially smoothed using the
  //      user-configured attack (when opening) / release (when closing).
  //      Smoothing the *gain* — not just the detector — is what eliminates
  //      the zipper noise that a hard threshold-cross produces.
  const float env_in = std::abs(x);
  state.gate_env += fast_det_alpha_ * (env_in - state.gate_env);
  const float gate_target = amp_to_db(state.gate_env) < config.gate.threshold_db
                                ? db_to_gain(-config.gate.range_db)
                                : 1.0f;
  smooth_attack_release(state.gate_gain, gate_target, gate_attack_, gate_release_,
                        /*attack_when_decreasing=*/false);
  x *= state.gate_gain;
  return x;
}

float RealtimeVoiceChanger::process_output_stage(ChannelState& state,
                                                 const RealtimeVoiceChangerConfig& config,
                                                 float input) noexcept {
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
  const float over = amp_to_db(state.comp_env) - config.compressor.threshold_db;
  float comp_target = 1.0f;
  if (over > 0.0f) {
    const float reduction_db = over - over / config.compressor.ratio;
    comp_target = db_to_gain(-reduction_db + config.compressor.makeup_gain_db);
  } else {
    comp_target = db_to_gain(config.compressor.makeup_gain_db);
  }
  smooth_attack_release(state.comp_gain, comp_target, comp_attack_, comp_release_,
                        /*attack_when_decreasing=*/true);
  x *= state.comp_gain;

  // De-esser: ratio-based broadband reduction triggered by the sibilance
  // band-pass. The kDeessEnvelopeHz LP gives a fast (~1.6 ms) detector, and
  // kDeessGainSmoothingHz smooths the gain itself (~0.8 ms) so the two stages
  // serve distinct purposes — detector tracking vs gain dezippering.
  const float ess = std::abs(state.deess_band.process(x));
  state.deess_env += deess_alpha_ * (ess - state.deess_env);
  const float ess_over = amp_to_db(state.deess_env) - config.deesser.threshold_db;
  float deess_target = 1.0f;
  if (ess_over > 0.0f) {
    const float reduction_db =
        std::min(config.deesser.range_db, ess_over - ess_over / config.deesser.ratio);
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
  x *= output_gain_;
  const float ceiling = db_to_gain(config.limiter.ceiling_db);
  const float abs_x = std::abs(x);
  const float limit_target =
      abs_x > ceiling ? ceiling / std::max(abs_x, sonare::constants::kAmpEpsilon) : 1.0f;
  smooth_attack_release(state.limiter_gain, limit_target, limiter_attack_, limiter_release_,
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
    for (int i = 0; i < num_samples; ++i) {
      scratch_[i] = process_input_stage(channel, config, channels[ch][i]);
    }
    channel.retune.process_block(scratch_.data(), scratch_.data(), num_samples);
    channel.formant.process_block(scratch_.data(), scratch_.data(), num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float input = channels[ch][i];
      const float wet = process_output_stage(channel, config, scratch_[i]);
      channels[ch][i] = input * (1.0f - wet_mix_) + wet * wet_mix_;
    }
    // Final inter-sample-peak limiter — applied after the dry/wet mix so the
    // sample-domain limiter inside process_output_stage cannot create new ISP
    // overshoots by clamping a transient. Skipped when wet_mix_ == 0 because
    // the output equals the dry input (no DSP modification by the voice
    // changer, so no new ISP overshoots are possible) and applying the limiter
    // would introduce its lookahead latency to a signal the caller expects to
    // pass through unchanged. Any ISP overshoots in the caller's own dry
    // signal are the caller's responsibility.
    if (config.limiter.enable_isp_limiter && wet_mix_ > 0.0f) {
      channel.isp_limiter.process_block(channels[ch], num_samples);
    }
  }
}

int RealtimeVoiceChanger::latency_samples() const noexcept {
  if (channels_.empty()) return 0;
  // Retune grain dominates; biquad / formant group delays (<= 8 samples
  // combined) are intentionally not added so this stays a stable,
  // host-compensable integer. See header for details.
  return channels_[0].retune.grain_size();
}

}  // namespace sonare::editing::voice_changer
