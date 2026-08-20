#pragma once

/// @file true_peak_limiter.h
/// @brief Ceiling limiter with true-peak style post guard.

#include <vector>

#include "mastering/dynamics/brickwall_limiter.h"
#include "rt/lookahead_buffer.h"
#include "rt/oversampler.h"
#include "rt/sliding_max.h"
#include "rt/true_peak_filter.h"

namespace sonare::mastering::maximizer {

struct TruePeakLimiterConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
  int oversample_factor = 4;
  /// @brief Detect-only mode: when true the gain envelope is computed at the
  ///        oversampled rate but applied to the base-rate signal (the per-base
  ///        sample gain is the minimum over the oversampled subsamples of that
  ///        base sample, so any inter-sample over forces the base sample down).
  ///        This is a BEST-EFFORT mode: it does NOT guarantee the true (inter-
  ///        sample) peak ceiling because the limited signal is never
  ///        re-synthesised at the oversampled rate. Leave false (the default)
  ///        to use the sample-accurate polyphase path that does guarantee the
  ///        true-peak ceiling.
  bool apply_gain_at_input_rate = false;
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
  int latency_samples() const noexcept override;

  // Parameters:
  //   0 = ceiling_db (clamped <= 0; not audio-thread safe, rejected by mixer automation)
  //   1 = release_ms (clamped >= 0; in-place time-constant recompute)
  // lookahead_ms, oversample_factor and apply_gain_at_input_rate are NOT
  // automatable (they resize buffers or switch processing modes).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=ceilingDb, 1=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

 private:
  static void validate_config(const TruePeakLimiterConfig& config);
  void prepare_buffers(int num_channels);
  void update_time_constants();
  /// @brief Sample rate the gain-smoother loop actually advances at.
  /// @details The fast/slow attack, release and crest envelopes are updated once
  ///          per OVERSAMPLED sample, so their millisecond time constants must be
  ///          converted to one-pole coefficients at the oversampled rate
  ///          (base rate * oversample factor). Converting at the base rate makes
  ///          every envelope run a factor of `oversample_factor` too fast.
  double smoother_sample_rate() const noexcept;
  float adaptive_release_coeff(float linked_peak);
  void process_polyphase(float* const* channels, int num_channels, int num_samples);
  void process_polyphase_detect_only(float* const* channels, int num_channels, int num_samples);

  TruePeakLimiterConfig config_{};
  dynamics::BrickwallLimiter limiter_;
  sonare::rt::TruePeakFilter true_peak_filter_;
  sonare::rt::Oversampler downsampler_{4};
  std::vector<sonare::rt::LookaheadBuffer> lookahead_;
  std::vector<sonare::rt::LookaheadBuffer> oversampled_lookahead_;
  std::vector<std::vector<float>> true_peak_history_;
  std::vector<const float*> input_ptrs_;
  std::vector<float*> oversampled_ptrs_;
  std::vector<std::vector<float>> oversampled_buffers_;
  std::vector<std::vector<float>> limited_oversampled_buffers_;
  std::vector<std::vector<float>> true_peak_scratch_;
  std::vector<sonare::rt::Oversampler::StreamingState> downsampler_states_;
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
};

}  // namespace sonare::mastering::maximizer
