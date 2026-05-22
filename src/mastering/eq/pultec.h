#pragma once

/// @file pultec.h
/// @brief Pultec EQP-1A inspired program equalizer.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

enum class PultecComponentModel {
  CurveOnly,
  Eqp1aWdf,
};

class PultecEq : public common::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_low_frequency(float frequency_hz);
  void set_low_boost(float amount);
  void set_low_attenuation(float amount);
  void set_high_boost(float frequency_hz, float amount, float bandwidth = 0.5f);
  void set_high_attenuation(float frequency_hz, float amount);
  void set_component_model(PultecComponentModel model);
  void set_output_drive(float drive);
  void clear();

  float low_frequency() const { return low_frequency_hz_; }
  float low_boost() const { return low_boost_; }
  float low_attenuation() const { return low_attenuation_; }
  float high_boost_frequency() const { return high_boost_frequency_hz_; }
  float high_boost() const { return high_boost_; }
  float high_attenuation_frequency() const { return high_attenuation_frequency_hz_; }
  float high_attenuation() const { return high_attenuation_; }
  PultecComponentModel component_model() const { return component_model_; }
  float output_drive() const { return output_drive_; }

 private:
  void rebuild();
  static float clamp_amount(float amount);
  static float validate_frequency(float frequency_hz);
  void prepare_component_state(int num_channels);
  float process_component_sample(float input, int channel);

  ParametricEq eq_;
  struct ComponentState {
    float low_charge = 0.0f;
    float high_charge = 0.0f;
  };
  std::vector<ComponentState> component_state_;
  double sample_rate_ = 48000.0;
  float low_frequency_hz_ = 60.0f;
  float low_boost_ = 0.0f;
  float low_attenuation_ = 0.0f;
  float high_boost_frequency_hz_ = 8000.0f;
  float high_boost_ = 0.0f;
  float high_bandwidth_ = 0.5f;
  float high_attenuation_frequency_hz_ = 10000.0f;
  float high_attenuation_ = 0.0f;
  PultecComponentModel component_model_ = PultecComponentModel::CurveOnly;
  float output_drive_ = 0.0f;
};

}  // namespace sonare::mastering::eq
