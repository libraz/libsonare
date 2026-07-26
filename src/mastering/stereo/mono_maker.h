#pragma once

/// @file mono_maker.h
/// @brief Stereo to mono low-frequency utility.

#include <array>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

struct MonoMakerConfig {
  float amount = 1.0f;
  float frequency_hz = 120.0f;
};

class MonoMaker : public rt::ProcessorBase {
 public:
  explicit MonoMaker(MonoMakerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MonoMakerConfig& config);
  const MonoMakerConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = amount (clamped to [0, 1])
  //   1 = crossover frequency in Hz
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=amount
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const MonoMakerConfig& config);
  void update_coefficient() noexcept;

  MonoMakerConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  std::array<float, 4> highpass_input_{};
  std::array<float, 4> highpass_output_{};
  float coefficient_ = 0.0f;
};

}  // namespace sonare::mastering::stereo
