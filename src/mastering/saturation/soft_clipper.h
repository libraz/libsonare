#pragma once

#include <vector>

#include "mastering/common/adaa.h"
#include "mastering/common/nonlinearities.h"
#include "mastering/saturation/waveshaper.h"

namespace sonare::mastering::saturation {

struct SoftClipperConfig {
  float drive_db = 0.0f;
  float ceiling = 1.0f;
  float mix = 1.0f;
  common::AliasingControl aliasing = common::AliasingControl::None;
};

class SoftClipper : public common::ProcessorBase {
 public:
  explicit SoftClipper(SoftClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftClipperConfig& config);
  const SoftClipperConfig& config() const { return config_; }

 private:
  static void validate_config(const SoftClipperConfig& config);
  void ensure_state(int num_channels);
  float process_sample(float sample, int channel);

  SoftClipperConfig config_{};
  bool prepared_ = false;
  std::vector<common::Adaa1<common::TanhNonlinearity>> tanh_adaa_;
};

}  // namespace sonare::mastering::saturation
