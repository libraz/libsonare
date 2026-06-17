#pragma once

#include <vector>

#include "mastering/maximizer/maximizer.h"

namespace sonare::mastering::maximizer {

struct SoftKneeMaxConfig {
  float input_gain_db = 0.0f;
  float ceiling_db = -1.0f;
  float knee_db = 6.0f;
  float release_ms = 50.0f;
};

class SoftKneeMax : public rt::ProcessorBase {
 public:
  explicit SoftKneeMax(SoftKneeMaxConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftKneeMaxConfig& config);
  const SoftKneeMaxConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return maximizer_.last_gain_reduction_db(); }

  // Parameters:
  //   0 = input_gain_db (applied per block, no coefficients)
  //   1 = ceiling_db (clamped <= 0; not audio-thread safe, rejected by mixer automation)
  //   2 = knee_db (clamped >= 0; applied per block, no coefficients)
  //   3 = release_ms (clamped >= 0; in-place via inner maximizer)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=inputGainDb, 1=ceilingDb, 2=kneeDb, 3=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

 private:
  static void validate_config(const SoftKneeMaxConfig& config);

  SoftKneeMaxConfig config_{};
  Maximizer maximizer_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::maximizer
