#pragma once

/// @file upward_compressor.h
/// @brief Upward compressor that raises quieter material below a threshold.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct UpwardCompressorConfig {
  float threshold_db = -30.0f;
  float ratio = 2.0f;
  float attack_ms = 10.0f;
  float release_ms = 100.0f;
  float range_db = 12.0f;
};

class UpwardCompressor : public common::ProcessorBase {
 public:
  explicit UpwardCompressor(UpwardCompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const UpwardCompressorConfig& config);
  const UpwardCompressorConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

 private:
  static void validate_config(const UpwardCompressorConfig& config);
  static float gain_db(float input_db, const UpwardCompressorConfig& config);
  void ensure_followers(int num_channels);

  UpwardCompressorConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
