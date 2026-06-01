#pragma once

#include <vector>

#include "rt/adaa.h"
#include "rt/aliasing_control.h"
#include "rt/nonlinearities.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct HardClipperConfig {
  float ceiling = 1.0f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class HardClipper : public rt::ProcessorBase {
 public:
  explicit HardClipper(HardClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const HardClipperConfig& config);
  const HardClipperConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation):
  //   0 = ceiling (clamped to > 0)
  // In the None aliasing mode the ceiling is read per sample, so the change is
  // applied with no state reset. In the Adaa1/Adaa2 modes the ceiling is baked
  // into the ADAA nonlinearity objects, which expose no in-place threshold
  // mutator; updating it therefore reconstructs those objects, clearing their
  // 1-2 sample antiderivative history. For a clipper this momentary
  // discontinuity is inaudible and acceptable. aliasing is an enum (not exposed).
  bool set_parameter(unsigned int param_id, float value) override;

  /// @brief Returns the processing latency in samples for the active mode.
  /// @details None/Adaa1/Oversample4x add no integer latency here; Adaa2 adds one sample.
  int latency_samples() const noexcept override;

 private:
  static void validate_config(const HardClipperConfig& config);
  void ensure_state(int num_channels);
  void rebuild_adaa();
  float process_sample(float sample, int channel);

  HardClipperConfig config_{};
  bool prepared_ = false;
  std::vector<sonare::rt::Adaa1<sonare::rt::HardClipNonlinearity>> hard_clip_adaa_;
  std::vector<sonare::rt::Adaa2<sonare::rt::HardClipNonlinearity>> hard_clip_adaa2_;
};

}  // namespace sonare::mastering::saturation
