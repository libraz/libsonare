#pragma once

/// @file waveshaper.h
/// @brief Configurable static waveshaper.

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

enum class WaveshaperCurve {
  Tanh,
  Arctan,
  Asymmetric,
};

struct WaveshaperConfig {
  float drive_db = 0.0f;
  float mix = 1.0f;
  float output_gain_db = 0.0f;
  float bias = 0.0f;
  WaveshaperCurve curve = WaveshaperCurve::Tanh;
};

class Waveshaper : public common::ProcessorBase {
 public:
  explicit Waveshaper(WaveshaperConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const WaveshaperConfig& config);
  const WaveshaperConfig& config() const { return config_; }

  static float db_to_linear(float db);
  static float shape(float sample, const WaveshaperConfig& config);

 private:
  static void validate_config(const WaveshaperConfig& config);

  WaveshaperConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::saturation
