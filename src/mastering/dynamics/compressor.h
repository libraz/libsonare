#pragma once

/// @file compressor.h
/// @brief Feed-forward compressor with soft knee and makeup gain.

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

enum class DetectorMode {
  Peak,
  Rms,
  LogRms,
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
  bool sidechain_hpf_enabled = false;
  float sidechain_hpf_hz = 100.0f;
  float pdr_time_ms = 0.0f;
  float pdr_release_scale = 1.0f;
};

class Compressor : public common::ProcessorBase {
 public:
  explicit Compressor(CompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const CompressorConfig& config);
  const CompressorConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

 private:
  static void validate_config(const CompressorConfig& config);
  static float gain_reduction_db(float input_db, const CompressorConfig& config);
  void update_coefficients();

  CompressorConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  // RMS pre-smoothing state (for Rms / LogRms detectors). Rms = 10 ms window,
  // LogRms = 50 ms window for sustained-level estimation.
  float rms_state_ = 0.0f;
  float rms_coeff_ = 0.0f;
  float log_rms_coeff_ = 0.0f;
  float hpf_b0_ = 1.0f;
  float hpf_a1_ = 0.0f;
  float hpf_x1_ = 0.0f;
  float hpf_y1_ = 0.0f;
  float pdr_state_db_ = 0.0f;
  float pdr_coeff_ = 0.0f;
  // Log-domain attack/release smoothing on the gain-reduction signal (in dB).
  common::EnvelopeFollower reduction_smoother_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
