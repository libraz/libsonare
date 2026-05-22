#pragma once

/// @file waveshaper.h
/// @brief Configurable static waveshaper.

#include <vector>

#include "mastering/common/adaa.h"
#include "mastering/common/aliasing_control.h"
#include "mastering/common/nonlinearities.h"
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
  common::AliasingControl aliasing = common::AliasingControl::None;
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
  void ensure_state(int num_channels);
  float shape_sample(float sample, int channel);

  WaveshaperConfig config_{};
  bool prepared_ = false;
  std::vector<common::Adaa1<common::TanhNonlinearity>> tanh_adaa_;
  std::vector<common::Adaa1<common::ArctanNonlinearity>> arctan_adaa_;
};

}  // namespace sonare::mastering::saturation
