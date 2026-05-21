#pragma once

/// @file multiband_saturation.h
/// @brief Multiband soft saturation processor.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct SaturationBandConfig {
  float drive_db = 0.0f;
  float mix = 1.0f;
  float output_gain_db = 0.0f;
  bool enabled = true;
};

struct MultibandSaturationConfig {
  CrossoverConfig crossover;
  std::vector<SaturationBandConfig> bands{
      {},
      {},
      {},
  };
};

class MultibandSaturation : public common::ProcessorBase {
 public:
  explicit MultibandSaturation(MultibandSaturationConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandSaturationConfig& config);
  const MultibandSaturationConfig& config() const { return config_; }

 private:
  static void validate_config(const MultibandSaturationConfig& config);
  static float db_to_linear(float db);
  static float saturate_sample(float sample, const SaturationBandConfig& config);

  MultibandSaturationConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
};

}  // namespace sonare::mastering::multiband
