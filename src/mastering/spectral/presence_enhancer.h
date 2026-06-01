#pragma once

#include <vector>

#include "rt/biquad_design.h"
#include "rt/processor_base.h"

namespace sonare::mastering::spectral {

struct PresenceEnhancerConfig {
  float amount = 0.2f;
  float drive = 2.0f;
  float center_frequency_hz = 3200.0f;
  float q = 1.2f;
};

class PresenceEnhancer : public rt::ProcessorBase {
 public:
  explicit PresenceEnhancer(PresenceEnhancerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const PresenceEnhancerConfig& config);

  // Automatable parameters (RT-safe: amount/drive are applied per-sample; the
  // bandpass center/Q recompute the cached biquad coefficients in place,
  // preserving filter state). Ids follow the PresenceEnhancerConfig declaration
  // order:
  //   0 = amount (clamped to [0, 1])
  //   1 = drive (clamped to > 0)
  //   2 = center_frequency_hz (clamped to > 0; recomputes bandpass coefficients)
  //   3 = q (clamped to > 0; recomputes bandpass coefficients)
  bool set_parameter(unsigned int param_id, float value) override;

  using Biquad = rt::BiquadState;

 private:
  static void validate_config(const PresenceEnhancerConfig& config);
  void ensure_state(int num_channels);

  PresenceEnhancerConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  std::vector<Biquad> bandpass_;
};

}  // namespace sonare::mastering::spectral
