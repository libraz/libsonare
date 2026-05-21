#pragma once

#include "mastering/maximizer/true_peak_limiter.h"

namespace sonare::mastering::maximizer {

/// @brief Limiter with crest-factor based adaptive release, per design Appendix C.3.
///
/// release_ms is recomputed each block from the short-term crest factor of the
/// input (peak / RMS). High crest (transient material) -> short release, low crest
/// (sustained material) -> long release. This is the patent-safe alternative to
/// iZotope IRC.
struct AdaptiveReleaseConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float min_release_ms = 20.0f;
  float max_release_ms = 250.0f;
  float crest_window_ms = 30.0f;
  float crest_low = 2.0f;    // crest factor at/below which release is at minimum
  float crest_high = 10.0f;  // crest factor at/above which release is at maximum
};

class AdaptiveRelease : public common::ProcessorBase {
 public:
  explicit AdaptiveRelease(AdaptiveReleaseConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const AdaptiveReleaseConfig& config);
  const AdaptiveReleaseConfig& config() const { return config_; }
  float current_release_ms() const { return current_release_ms_; }
  float current_crest_factor() const { return current_crest_factor_; }
  float last_gain_reduction_db() const { return limiter_.last_gain_reduction_db(); }

 private:
  static void validate_config(const AdaptiveReleaseConfig& config);
  void configure_limiter();
  float compute_crest_factor(float* const* channels, int num_channels, int num_samples) const;

  AdaptiveReleaseConfig config_{};
  TruePeakLimiter limiter_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  float current_release_ms_ = 20.0f;
  float current_crest_factor_ = 0.0f;
};

}  // namespace sonare::mastering::maximizer
