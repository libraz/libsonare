#pragma once

#include <vector>

#include "mastering/common/adaa.h"
#include "mastering/common/aliasing_control.h"
#include "mastering/common/nonlinearities.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct HardClipperConfig {
  float ceiling = 1.0f;
  common::AliasingControl aliasing = common::AliasingControl::None;
};

class HardClipper : public common::ProcessorBase {
 public:
  explicit HardClipper(HardClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const HardClipperConfig& config);
  const HardClipperConfig& config() const { return config_; }

 private:
  static void validate_config(const HardClipperConfig& config);
  void ensure_state(int num_channels);
  float process_sample(float sample, int channel);

  HardClipperConfig config_{};
  bool prepared_ = false;
  std::vector<common::Adaa1<common::HardClipNonlinearity>> hard_clip_adaa_;
};

}  // namespace sonare::mastering::saturation
