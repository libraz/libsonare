#pragma once

/// @file imager.h
/// @brief Stereo width processor.

#include "mastering/common/processor_base.h"

namespace sonare::mastering::stereo {

struct ImagerConfig {
  float width = 1.0f;
  float output_gain_db = 0.0f;
};

class Imager : public common::ProcessorBase {
 public:
  explicit Imager(ImagerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const ImagerConfig& config);
  const ImagerConfig& config() const { return config_; }

 private:
  static void validate_config(const ImagerConfig& config);
  static float db_to_linear(float db);

  ImagerConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
