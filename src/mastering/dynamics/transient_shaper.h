#pragma once

/// @file transient_shaper.h
/// @brief Envelope-difference transient shaper for attack and sustain control.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct TransientShaperConfig {
  float attack_gain_db = 3.0f;
  float sustain_gain_db = 0.0f;
  float fast_attack_ms = 0.0f;
  float fast_release_ms = 20.0f;
  float slow_attack_ms = 15.0f;
  float slow_release_ms = 200.0f;
  float sensitivity = 1.0f;
  float max_gain_db = 12.0f;
  float gain_smoothing_ms = 0.0f;
  float lookahead_ms = 0.0f;
};

class TransientShaper : public common::ProcessorBase {
 public:
  explicit TransientShaper(TransientShaperConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const TransientShaperConfig& config);
  const TransientShaperConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = attack_gain_db
  //   1 = sustain_gain_db
  //   2 = fast_attack_ms (clamped to >= 0)
  //   3 = fast_release_ms (clamped to >= 0)
  //   4 = slow_attack_ms (clamped to >= 0)
  //   5 = slow_release_ms (clamped to >= 0)
  //   6 = sensitivity (clamped to >= 0)
  //   7 = max_gain_db (clamped to >= 0)
  //   8 = gain_smoothing_ms (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const TransientShaperConfig& config);
  static float coeff(double sample_rate, float ms);
  void ensure_followers(int num_channels);

  TransientShaperConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> fast_followers_;
  std::vector<common::EnvelopeFollower> slow_followers_;
  std::vector<float> gain_state_db_;
  std::vector<std::vector<float>> lookahead_;
  std::vector<size_t> lookahead_index_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
