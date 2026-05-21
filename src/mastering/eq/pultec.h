#pragma once

/// @file pultec.h
/// @brief Pultec EQP-1A inspired program equalizer.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

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
  void clear();

  float low_frequency() const { return low_frequency_hz_; }
  float low_boost() const { return low_boost_; }
  float low_attenuation() const { return low_attenuation_; }
  float high_boost_frequency() const { return high_boost_frequency_hz_; }
  float high_boost() const { return high_boost_; }
  float high_attenuation_frequency() const { return high_attenuation_frequency_hz_; }
  float high_attenuation() const { return high_attenuation_; }

 private:
  void rebuild();
  static float clamp_amount(float amount);
  static float validate_frequency(float frequency_hz);

  ParametricEq eq_;
  float low_frequency_hz_ = 60.0f;
  float low_boost_ = 0.0f;
  float low_attenuation_ = 0.0f;
  float high_boost_frequency_hz_ = 8000.0f;
  float high_boost_ = 0.0f;
  float high_bandwidth_ = 0.5f;
  float high_attenuation_frequency_hz_ = 10000.0f;
  float high_attenuation_ = 0.0f;
};

}  // namespace sonare::mastering::eq
