#pragma once

#include "mastering/saturation/waveshaper.h"

namespace sonare::mastering::saturation {

struct SoftClipperConfig {
  float drive_db = 0.0f;
  float ceiling = 1.0f;
  float mix = 1.0f;
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
  SoftClipperConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::saturation
