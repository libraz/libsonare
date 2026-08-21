#pragma once

#include <vector>

#include "mastering/maximizer/true_peak_limiter.h"

namespace sonare::mastering::maximizer {

/// @brief Limiter with crest-factor based adaptive release, per design Appendix C.3.
///
/// release_ms tracks the short-term crest factor of the input (peak / RMS).
/// High crest (transient material) -> short release, low crest (sustained
/// material) -> long release. This avoids product-specific limiter behavior
/// while keeping the mapping deterministic.
///
/// The crest, RMS and release-smoothing envelopes advance one sample at a time
/// with coefficients derived from @c sample_rate and the configured
/// milliseconds alone, and the resulting release is handed to the inner limiter
/// on a fixed control grid anchored to the stream position. Neither depends on
/// @c num_samples, so a host that renders in 512-sample callbacks and one that
/// renders a whole file in a single block get the same output.
struct AdaptiveReleaseConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float min_release_ms = 20.0f;
  float max_release_ms = 250.0f;
  float crest_window_ms = 30.0f;
  float crest_low = 2.0f;    // crest factor at/below which release is at minimum
  float crest_high = 10.0f;  // crest factor at/above which release is at maximum
  float release_smoothing_ms = 20.0f;
};

class AdaptiveRelease : public rt::ProcessorBase {
 public:
  explicit AdaptiveRelease(AdaptiveReleaseConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const AdaptiveReleaseConfig& config);
  const AdaptiveReleaseConfig& config() const { return config_; }
  float current_release_ms() const { return current_release_ms_; }
  float current_crest_factor() const { return current_crest_factor_; }
  /// @brief Most negative gain reduction over the whole of the last process()
  ///        call, aggregated across its internal control chunks.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }
  int latency_samples() const noexcept override { return limiter_.latency_samples(); }

  // Automatable parameters (RT-safe, no allocation). Most are read directly by
  // the per-sample adaptive-release mapping, so writing config_ is sufficient;
  // the two that set an envelope speed (3, 6) also refresh their cached
  // one-pole coefficient, which is plain arithmetic:
  //   0 = ceiling_db (clamped <= 0; forwarded in-place to the inner limiter)
  //   1 = min_release_ms (clamped to >= 0)
  //   2 = max_release_ms (clamped to >= min_release_ms)
  //   3 = crest_window_ms (clamped to a small positive minimum)
  //   4 = crest_low (clamped to a small positive minimum)
  //   5 = crest_high (clamped to > crest_low)
  //   6 = release_smoothing_ms (clamped to >= 0)
  // lookahead_ms is NOT automatable (it resizes the inner lookahead buffers).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=ceilingDb, 1=minReleaseMs, 2=maxReleaseMs,
  // 3=crestWindowMs, 4=crestLow, 5=crestHigh, 6=releaseSmoothingMs.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const AdaptiveReleaseConfig& config);
  void configure_limiter();
  void update_envelope_coefficients() noexcept;
  /// @brief Advances the crest/RMS/release envelopes over @p count input samples
  ///        starting at @p offset. Must run before the limiter overwrites the
  ///        buffer in place.
  void advance_envelopes(float* const* channels, int num_channels, int offset, int count) noexcept;

  /// @brief Interval, in input samples, between release updates handed to the
  ///        inner limiter.
  /// @details The release itself is smoothed per sample; this is only how often
  ///          the smoothed value is published, and it is what keeps the control
  ///          rate independent of the caller's block size. 64 samples is 1.3 ms
  ///          at 48 kHz — an order of magnitude faster than the release
  ///          smoothing this stage is configured with — while keeping the
  ///          number of inner limiter calls bounded on an offline render.
  static constexpr int kControlIntervalSamples = 64;

  AdaptiveReleaseConfig config_{};
  TruePeakLimiter limiter_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  float current_release_ms_ = 20.0f;
  float current_crest_factor_ = 0.0f;
  float peak_envelope_ = 0.0f;
  float rms_square_envelope_ = 0.0f;
  float crest_coeff_ = 0.0f;              ///< Per-sample crest/RMS envelope rate.
  float release_smoothing_coeff_ = 1.0f;  ///< Per-sample release smoothing rate.
  int control_phase_ = 0;                 ///< Position within the control grid.
  float last_gain_reduction_db_ = 0.0f;   ///< Most negative GR over the last call.
  /// Channel pointers offset to the current control chunk. Sized in prepare()
  /// so the audio thread never allocates.
  std::vector<float*> chunk_channels_;
};

}  // namespace sonare::mastering::maximizer
