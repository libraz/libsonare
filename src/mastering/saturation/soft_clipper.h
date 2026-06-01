#pragma once

#include <vector>

#include "mastering/saturation/waveshaper.h"
#include "rt/adaa.h"
#include "rt/nonlinearities.h"

namespace sonare::mastering::saturation {

struct SoftClipperConfig {
  float drive_db = 0.0f;
  float ceiling = 1.0f;
  float mix = 1.0f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class SoftClipper : public rt::ProcessorBase {
 public:
  explicit SoftClipper(SoftClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftClipperConfig& config);
  const SoftClipperConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). Both are
  // read per sample in process_sample() with no precomputed coefficients:
  //   0 = drive_db
  //   1 = mix (clamped to [0, 1])
  // ceiling is NOT automatable: it normalizes the ADAA input, so changing it
  // would require clearing the antiderivative history. aliasing is an enum.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const SoftClipperConfig& config);
  void ensure_state(int num_channels);
  float process_sample(float sample, int channel);

  SoftClipperConfig config_{};
  bool prepared_ = false;
  std::vector<sonare::rt::Adaa1<sonare::rt::TanhNonlinearity>> tanh_adaa_;
};

}  // namespace sonare::mastering::saturation
