#pragma once

/// @file true_peak_limiter.h
/// @brief Ceiling limiter with true-peak style post guard.

#include <cstdint>
#include <vector>

#include "mastering/dynamics/brickwall_limiter.h"
#include "rt/lookahead_buffer.h"
#include "rt/overflow_counter.h"
#include "rt/oversampler.h"
#include "rt/polyphase_fir.h"
#include "rt/sliding_max.h"

namespace sonare::mastering::maximizer {

struct TruePeakLimiterConfig {
  /// @brief Output true-peak ceiling in dBTP.
  /// @details Holds as metering::true_peak reads the output at @ref
  ///   oversample_factor, to within float rounding of the final correction: the
  ///   last stage (@ref TruePeakOutputGuard) reconstructs the output with that
  ///   meter's own interpolation filter and scales any stencil whose
  ///   interpolated value exceeds the ceiling. A meter running at a HIGHER
  ///   oversampling interpolates points the limiter never evaluated and can
  ///   read above the ceiling: measured for the default 4x limiter read by a
  ///   16x meter at +0.12 dB on full-scale white noise and below 0.001 dB on
  ///   transient program material. Raise @ref oversample_factor to push that
  ///   down, or match the meter's factor to eliminate it. An offline render
  ///   trimmed to its input length drops the pre-ring ahead of its first
  ///   sample, so a meter reading that render with silence in its place can
  ///   exceed the ceiling within the first stencil of a program that starts at
  ///   full level: measured +0.05 dB on a full-level step at sample 0, none
  ///   once six samples of silence precede it.
  ///
  ///   The ceiling is applied sample by sample and never as a whole-block
  ///   rescale: a correction derived from a block's own maximum would make the
  ///   output depend on the host's buffer size, so an offline render and a
  ///   streaming render of the same material would disagree.
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
  int oversample_factor = 4;
  /// @brief Apply the gain at the base rate instead of reconstructing the
  ///        limited signal at @ref oversample_factor and decimating it back.
  /// @details The gain envelope is computed at the oversampled rate either way.
  ///   Here each base sample takes the minimum gain over its own subsamples and
  ///   the decimation stage is skipped, which shortens the latency by one
  ///   decimation group delay. Both modes end in the same output guard, so the
  ///   ceiling holds identically in each.
  bool apply_gain_at_input_rate = false;
};

/// @brief Final ceiling guard, evaluated with the interpolation a meter uses.
/// @details Reconstructs the base-rate output with the canonical true-peak FIR
///   for the limiter's factor (the one metering::true_peak reads with) and,
///   where an interpolated value exceeds the ceiling, scales every base sample
///   of that value's stencil, channel-linked, so it lands on the ceiling.
///   Positions are visited in stream order and each reads every earlier
///   correction, and a correction rechecks the earlier positions its scaling
///   can have raised. The output is delayed by one stencil reach, so no sample
///   leaves before every interpolation reading it has been evaluated, plus the
///   recheck window's reach, so a recheck only touches samples not yet emitted.
///   The block size plays no part in the result.
class TruePeakOutputGuard {
 public:
  void prepare(int factor, int max_channels, int max_block_size);
  void reset() noexcept;
  int latency_samples() const noexcept { return latency_; }
  /// @brief Bounds @p channels in place; returns the smallest gain it applied.
  /// @param excluded_channel Channel left out of the linked detection (still
  ///        scaled), or negative for none.
  float process(float* const* channels, int num_channels, int num_samples, int excluded_channel,
                float ceiling);

 private:
  // Recheck window, in stencil reaches. One reach leaves the positions a recheck
  // itself raises outside the window: measured 12 of 2000 random bursts over the
  // ceiling (+0.0017 dB worst); two reached none in 20000.
  static constexpr int kRecheckReaches = 2;
  // Bounds the rechecks after one correction; each pass only scales down.
  static constexpr int kMaxRecheckPasses = 8;

  const rt::PolyphaseFir* fir_ = nullptr;
  std::vector<std::vector<float>> work_;
  int reach_ = 0;
  int latency_ = 0;
};

/// @brief Default depth (dB) the loudness stages may drive their post-gain
///        true-peak limiter to in order to reach the requested LUFS target.
/// @details The static normalization gain may exceed the peak headroom toward
///          the ceiling by this much; the limiter then brings the signal back
///          under the ceiling, which it does at every setting. The allowance
///          only ever permits gain up to `target - current`, so it cannot make a
///          master louder than the requested target — it decides how peaky an
///          input the stage will still try to normalize.
///
///          12 dB is the point at which the clamp stops being what limits any
///          built-in preset on peak-normalized program material (crest ~13.5 dB,
///          the worst case among the built-in test signals): every preset's
///          applied gain reaches its full `target - current` there, and larger
///          allowances measure identically. Smaller allowances reach the -14 and
///          -12 LUFS streaming targets but leave the loud club targets short by
///          enough that presets several LU apart deliver near-identical masters.
///
///          Every loudness path shares this default so the chain, the standalone
///          helper, and the named processor normalize alike; override per run
///          through `loudness.maxLimiterGainReductionDb`, where 0 restores a
///          strict headroom clamp.
inline constexpr float kDefaultLoudnessMaxLimiterGainReductionDb = 12.0f;

/// @brief Release time (ms) every loudness entry point runs its post-gain
///        true-peak limiter at unless the caller asks for another one.
/// @details Mirrors @ref TruePeakLimiterConfig::release_ms, which is the
///          limiter's own default rather than the loudness stage's; the two are
///          equal today and this name is what the public "0 selects the library
///          default" sentinel resolves to.
inline constexpr float kDefaultLoudnessReleaseMs = 50.0f;

/// @brief Builds the limiter config the loudness-normalization stage runs after
///        applying its static normalization gain.
/// @details The standalone @ref loudness_optimize helper, the per-processor
///          loudness stages, and the in-chain mono/stereo loudness stages all
///          run the same true-peak limiter. Routing every one through this
///          single constructor keeps them in lockstep on every field — a
///          standalone path that read only a subset (e.g. dropped @p release_ms
///          or @p apply_gain_at_input_rate) would limit differently from the
///          identical settings inside a chain.
inline TruePeakLimiterConfig loudness_limiter_config(float ceiling_db, int oversample_factor,
                                                     float release_ms,
                                                     bool apply_gain_at_input_rate) {
  TruePeakLimiterConfig config;
  config.ceiling_db = ceiling_db;
  config.oversample_factor = oversample_factor;
  config.release_ms = release_ms;
  config.apply_gain_at_input_rate = apply_gain_at_input_rate;
  return config;
}

class TruePeakLimiter : public rt::ProcessorBase {
 public:
  explicit TruePeakLimiter(TruePeakLimiterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // Control-thread only; must not race with process(). Changing lookahead,
  // oversample factor, or gain-application mode re-prepares and resets the
  // signal history and can change latency. Apply those changes between streams.
  // Ceiling and release changes retain the running signal history.
  void set_config(const TruePeakLimiterConfig& config);
  void set_release_ms(float release_ms);
  /// @brief Realtime-safe release update for per-block automation.
  /// @details Recomputes the scalar release time constant in place and forwards
  ///          to the inner brickwall limiter's in-place setter, without
  ///          publishing any configuration snapshot (no allocation). Safe to
  ///          call once per block from the audio thread. Uses the same
  ///          release-coefficient math as @ref set_release_ms; negative inputs
  ///          are clamped to zero rather than throwing.
  void set_release_ms_in_place(float release_ms) noexcept;
  const TruePeakLimiterConfig& config() const { return config_; }
  /// Instantaneous (most recently processed block) gain reduction for streaming
  /// meters. Offline hosts should use @ref minimum_gain_reduction_db instead.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }
  /// Most-negative gain reduction since the last prepare/reset. Offline hosts
  /// process a zero tail to drain latency, so this retains the program value.
  float minimum_gain_reduction_db() const noexcept { return minimum_gain_reduction_db_; }
  /// @brief Non-finite samples this stage replaced with a finite in-domain one.
  /// @details Monotonic since @ref prepare, which clears it; @ref reset does not.
  ///          Each becomes silence, at the input and again after the gain, so the
  ///          output stays finite, in range and free of any error while carrying
  ///          samples unrelated to the input. The count is per replacement, so a
  ///          sample replaced at both stages adds two; only zero versus non-zero
  ///          is a contract.
  std::uint32_t non_finite_substitution_count() const noexcept {
    return non_finite_substitution_count_.load();
  }
  int latency_samples() const noexcept override;
  int tail_samples() const noexcept override;

  // Parameters:
  //   0 = ceiling_db (clamped <= 0; realtime-safe, in-place)
  //   1 = release_ms (clamped >= 0; in-place time-constant recompute)
  // lookahead_ms, oversample_factor and apply_gain_at_input_rate are NOT
  // automatable (they resize buffers or switch processing modes).
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=ceilingDb, 1=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

  static void validate_config(const TruePeakLimiterConfig& config);

 private:
  void prepare_buffers(int num_channels);
  void update_time_constants();
  /// @brief Sample rate the RELEASE and crest-detector envelopes are converted
  ///        at; the fast/slow ATTACK envelopes deliberately are not (see
  ///        update_time_constants()).
  /// @details The gain-smoother loop advances once per OVERSAMPLED sample, so
  ///          the release and crest time constants must be converted to
  ///          one-pole coefficients at this rate (base rate * oversample
  ///          factor) or a nominal release runs a factor of `oversample_factor`
  ///          too fast. The attack coefficients are converted at the base rate
  ///          instead, on purpose: they are already near-instant peak clamps,
  ///          and running them `oversample_factor` times faster only tightens
  ///          limiting further, never loosens it.
  double smoother_sample_rate() const noexcept;
  float adaptive_release_coeff(float linked_peak);
  void process_polyphase(float* const* channels, int num_channels, int num_samples);
  void process_polyphase_detect_only(float* const* channels, int num_channels, int num_samples);
  /// @brief Returns the gain smoothers and the crest detector to rest when a
  ///        non-finite value has reached them. Runs once per block.
  void discard_non_finite_state() noexcept;

  TruePeakLimiterConfig config_{};
  rt::ChildProcessor<dynamics::BrickwallLimiter> limiter_;
  sonare::rt::Oversampler oversampler_{4};
  std::vector<sonare::rt::LookaheadBuffer> lookahead_;
  std::vector<sonare::rt::LookaheadBuffer> oversampled_lookahead_;
  std::vector<std::vector<float>> oversampled_buffers_;
  std::vector<std::vector<float>> limited_oversampled_buffers_;
  std::vector<sonare::rt::Oversampler::StreamingState> oversampler_states_;
  TruePeakOutputGuard output_guard_;
  std::vector<float> linked_abs_;
  std::vector<float> input_rate_gain_;
  std::vector<float> downsampled_;
  sonare::rt::SlidingMax<float> oversampled_peak_window_{1};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_working_channels_ = 0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  float fast_gain_ = 1.0f;
  float slow_gain_ = 1.0f;
  float crest_peak_ = 0.0f;
  float crest_rms_ = 0.0f;
  float fast_attack_coeff_ = 0.0f;
  float slow_attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  float crest_coeff_ = 0.0f;
  float adaptive_release_coeff_ = 0.0f;
  unsigned int adaptive_release_counter_ = 0;
  static constexpr unsigned int kReleaseControlInterval = 8;
  float last_gain_reduction_db_ = 0.0f;
  float minimum_gain_reduction_db_ = 0.0f;
  rt::OverflowCounter non_finite_substitution_count_{};
};

}  // namespace sonare::mastering::maximizer
