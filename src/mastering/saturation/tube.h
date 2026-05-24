#pragma once

#include <vector>

#include "mastering/common/oversampler.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct TubeConfig {
  float drive_db = 6.0f;
  float bias = 0.15f;
  float mix = 1.0f;
  int oversample_factor = 4;
  float bias_v = -1.6f;
  float harmonic_drive = 1.0f;
};

class Tube : public common::ProcessorBase {
 public:
  explicit Tube(TubeConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TubeConfig& config);
  const TubeConfig& tube_config() const { return tube_config_; }

 private:
  static void validate_config(const TubeConfig& config);
  static float process_model(float sample, const TubeConfig& config);
  void ensure_state(int num_channels);
  float apply_miller_filter(int channel, float sample);

  TubeConfig tube_config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  common::Oversampler oversampler_{4};
  std::vector<float> scratch_;
  std::vector<float> miller_state_;
};

}  // namespace sonare::mastering::saturation
