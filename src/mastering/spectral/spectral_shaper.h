#pragma once

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

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

class SpectralShaper : public common::ProcessorBase {
 public:
  explicit SpectralShaper(SpectralShaperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SpectralShaperConfig& config);
  float last_reduction_db() const { return last_reduction_db_; }

 private:
  static void validate_config(const SpectralShaperConfig& config);
  SpectralShaperConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<float> low_state_;
  std::vector<float> band_low_state_;
  std::vector<float> gain_state_;
  std::vector<common::EnvelopeFollower> envelopes_;
  float last_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::spectral
