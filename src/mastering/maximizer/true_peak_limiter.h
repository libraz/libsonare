#pragma once

/// @file true_peak_limiter.h
/// @brief Ceiling limiter with true-peak style post guard.

#include <vector>

#include "mastering/common/lookahead_buffer.h"
#include "mastering/common/oversampler.h"
#include "mastering/common/sliding_max.h"
#include "mastering/common/true_peak_filter.h"
#include "mastering/dynamics/brickwall_limiter.h"

namespace sonare::mastering::maximizer {

struct TruePeakLimiterConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
  int oversample_factor = 4;
  bool apply_gain_at_input_rate = false;
};

class TruePeakLimiter : public common::ProcessorBase {
 public:
  explicit TruePeakLimiter(TruePeakLimiterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TruePeakLimiterConfig& config);
  void set_release_ms(float release_ms);
  const TruePeakLimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }
  int latency_samples() const noexcept override;

 private:
  static void validate_config(const TruePeakLimiterConfig& config);
  void prepare_buffers(int num_channels);
  void update_time_constants();
  float adaptive_release_coeff(float linked_peak);
  void process_polyphase(float* const* channels, int num_channels, int num_samples);
  void process_polyphase_detect_only(float* const* channels, int num_channels, int num_samples);
  void process_fallback(float* const* channels, int num_channels, int num_samples);

  TruePeakLimiterConfig config_{};
  dynamics::BrickwallLimiter limiter_;
  common::TruePeakFilter true_peak_filter_;
  common::Oversampler downsampler_{4};
  std::vector<common::LookaheadBuffer> lookahead_;
  std::vector<common::LookaheadBuffer> oversampled_lookahead_;
  std::vector<std::vector<float>> true_peak_history_;
  std::vector<const float*> input_ptrs_;
  std::vector<float*> oversampled_ptrs_;
  std::vector<std::vector<float>> oversampled_buffers_;
  std::vector<std::vector<float>> limited_oversampled_buffers_;
  std::vector<float> linked_abs_;
  std::vector<float> input_rate_gain_;
  common::SlidingMax<float> oversampled_peak_window_{1};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
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
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::maximizer
