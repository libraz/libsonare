#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::spectral {

struct PresenceEnhancerConfig {
  float amount = 0.2f;
  float drive = 2.0f;
  float center_frequency_hz = 3200.0f;
  float q = 1.2f;
};

class PresenceEnhancer : public common::ProcessorBase {
 public:
  explicit PresenceEnhancer(PresenceEnhancerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const PresenceEnhancerConfig& config);

  struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
    float process(float x);
    void reset();
  };

 private:
  static void validate_config(const PresenceEnhancerConfig& config);
  void ensure_state(int num_channels);

  PresenceEnhancerConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  std::vector<Biquad> bandpass_;
};

}  // namespace sonare::mastering::spectral
