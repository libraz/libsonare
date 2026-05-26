#pragma once

/// @file deesser.h
/// @brief Split-band de-esser for attenuating sibilant high-frequency energy.

#include <vector>

#include "mastering/common/biquad.h"
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
  float bandpass_q = 1.5f;
};

class DeEsser : public common::ProcessorBase {
 public:
  explicit DeEsser(DeEsserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const DeEsserConfig& config);
  const DeEsserConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = frequency_hz (clamped to > 0)
  //   1 = threshold_db
  //   2 = ratio (clamped to >= 1)
  //   3 = attack_ms (clamped to >= 0)
  //   4 = release_ms (clamped to >= 0)
  //   5 = range_db (clamped to >= 0)
  //   6 = bandpass_q (clamped to > 0)
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const DeEsserConfig& config);
  static float gain_reduction_db(float input_db, const DeEsserConfig& config);
  void ensure_state(int num_channels);
  void update_filter_coeff();

  using Biquad = common::Biquad;

  static constexpr size_t kPreparedChannels = 2;
  DeEsserConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  Biquad filter_coeffs_;
  std::vector<Biquad> bandpass_;
  std::vector<Biquad> bandpass2_;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
