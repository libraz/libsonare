#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct ExciterConfig {
  float frequency_hz = 3000.0f;
  float drive_db = 6.0f;
  float amount = 0.25f;
  float q = 1.0f;
  float even_odd_mix = 0.5f;
};

class Exciter : public common::ProcessorBase {
 public:
  explicit Exciter(ExciterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const ExciterConfig& config);
  const ExciterConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = frequency_hz (clamped to > 0; recomputes biquad coeffs in place)
  //   1 = drive_db (read per sample)
  //   2 = amount (clamped to >= 0; read per sample)
  //   3 = q (clamped to > 0; recomputes biquad coeffs in place)
  //   4 = even_odd_mix (clamped to [0, 1]; read per sample)
  // Coefficient updates preserve the per-channel biquad delay state.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const ExciterConfig& config);
  void compute_coeffs();
  void update_coeff();
  void update_coeff_preserving_state();
  void ensure_state(int num_channels);
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

  ExciterConfig config_{};
  double sample_rate_ = 48000.0;
  Biquad bandpass_coeffs_;
  Biquad allpass_coeffs_;
  bool prepared_ = false;
  std::vector<Biquad> bandpass_;
  std::vector<Biquad> allpass_;
};

}  // namespace sonare::mastering::saturation
