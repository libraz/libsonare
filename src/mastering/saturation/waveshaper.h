#pragma once

/// @file waveshaper.h
/// @brief Configurable static waveshaper.

#include <vector>

#include "rt/adaa.h"
#include "rt/aliasing_control.h"
#include "rt/nonlinearities.h"
#include "rt/processor_base.h"

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
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class Waveshaper : public rt::ProcessorBase {
 public:
  explicit Waveshaper(WaveshaperConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const WaveshaperConfig& config);
  const WaveshaperConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). All are
  // read per sample by shape()/shape_sample() with no precomputed coefficients:
  //   0 = drive_db
  //   1 = mix (clamped to [0, 1])
  //   2 = output_gain_db
  // bias is NOT automatable: it shifts the ADAA operating point, which would
  // require clearing the antiderivative history. curve/aliasing are enums.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=driveDb, 1=mix, 2=outputGainDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  static float db_to_linear(float db);
  static float shape(float sample, const WaveshaperConfig& config);

 private:
  static void validate_config(const WaveshaperConfig& config);
  void ensure_state(int num_channels);
  float shape_sample(float sample, int channel);

  WaveshaperConfig config_{};
  bool prepared_ = false;
  std::vector<sonare::rt::Adaa1<sonare::rt::TanhNonlinearity>> tanh_adaa_;
  std::vector<sonare::rt::Adaa1<sonare::rt::ArctanNonlinearity>> arctan_adaa_;
};

}  // namespace sonare::mastering::saturation
