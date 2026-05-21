#pragma once

/// @file compressor.h
/// @brief Feed-forward compressor with soft knee and makeup gain.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

enum class DetectorMode {
  Peak,
  Rms,
};

struct CompressorConfig {
  float threshold_db = -18.0f;
  float ratio = 2.0f;
  float attack_ms = 10.0f;
  float release_ms = 100.0f;
  float knee_db = 0.0f;
  float makeup_gain_db = 0.0f;
  bool auto_makeup = false;
  DetectorMode detector = DetectorMode::Rms;
};

class Compressor : public common::ProcessorBase {
 public:
  explicit Compressor(CompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const CompressorConfig& config);
  const CompressorConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const CompressorConfig& config);
  static float linear_to_db(float value);
  static float db_to_linear(float db);
  static float gain_reduction_db(float input_db, const CompressorConfig& config);
  void ensure_followers(int num_channels);

  CompressorConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
