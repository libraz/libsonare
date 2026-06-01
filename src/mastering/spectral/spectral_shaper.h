#pragma once

#include <vector>

#include "rt/envelope_follower.h"
#include "rt/processor_base.h"

namespace sonare::mastering::spectral {

struct SpectralShaperConfig {
  float threshold = 0.25f;
  float amount = 0.5f;
  float frequency_hz = 3000.0f;
  float high_frequency_hz = 6000.0f;
  float attack_ms = 3.0f;
  float release_ms = 80.0f;
  float range_db = 12.0f;
};

class SpectralShaper : public rt::ProcessorBase {
 public:
  explicit SpectralShaper(SpectralShaperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SpectralShaperConfig& config);
  float last_reduction_db() const { return last_reduction_db_; }

  // Automatable parameters (RT-safe: updates config in place; per-block
  // coefficients are derived in process(), and attack/release re-prepare the
  // envelope followers without resetting their state). Ids follow the
  // SpectralShaperConfig declaration order:
  //   0 = threshold (clamped to >= 0)
  //   1 = amount (clamped to [0, 1])
  //   2 = frequency_hz (clamped to (0, high_frequency_hz))
  //   3 = high_frequency_hz (clamped to > frequency_hz)
  //   4 = attack_ms (clamped to >= 0; re-prepares envelope followers)
  //   5 = release_ms (clamped to >= 0; re-prepares envelope followers)
  //   6 = range_db (clamped to >= 0)
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const SpectralShaperConfig& config);
  SpectralShaperConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<float> low_state_;
  std::vector<float> band_low_state_;
  std::vector<float> gain_state_;
  std::vector<sonare::rt::EnvelopeFollower> envelopes_;
  float last_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::spectral
