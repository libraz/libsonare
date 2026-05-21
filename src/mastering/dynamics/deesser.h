#pragma once

/// @file deesser.h
/// @brief Split-band de-esser for attenuating sibilant high-frequency energy.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct DeEsserConfig {
  float frequency_hz = 6000.0f;
  float threshold_db = -24.0f;
  float ratio = 4.0f;
  float attack_ms = 1.0f;
  float release_ms = 60.0f;
  float range_db = 12.0f;
};

class DeEsser : public common::ProcessorBase {
 public:
  explicit DeEsser(DeEsserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const DeEsserConfig& config);
  const DeEsserConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const DeEsserConfig& config);
  static float linear_to_db(float value);
  static float db_to_linear(float db);
  static float gain_reduction_db(float input_db, const DeEsserConfig& config);
  void ensure_state(int num_channels);
  void update_filter_coeff();

  DeEsserConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  float lowpass_coeff_ = 0.0f;
  std::vector<float> lowpass_state_;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
