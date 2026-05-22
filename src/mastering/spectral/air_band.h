#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::spectral {

struct AirBandConfig {
  float amount = 0.25f;
  float shelf_frequency_hz = 12000.0f;
  float dynamic_threshold_db = -36.0f;
  float dynamic_range_db = 3.0f;
};

class AirBand : public common::ProcessorBase {
 public:
  explicit AirBand(AirBandConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const AirBandConfig& config);

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
  static void validate_config(const AirBandConfig& config);
  void ensure_state(int num_channels);
  void rebuild_filters(int num_channels);

  AirBandConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  std::vector<float> previous_;
  std::vector<float> envelope_;
  std::vector<Biquad> shelf_;
  std::vector<Biquad> detector_;
};

}  // namespace sonare::mastering::spectral
