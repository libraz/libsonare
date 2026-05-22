#pragma once

/// @file multiband_dynamic_eq.h
/// @brief Multiband dynamic EQ built from Crossover and per-band DynamicEq processors.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct MultibandDynamicEqConfig {
  CrossoverConfig crossover;
  std::vector<std::vector<eq::DynamicEqBand>> bands{{}, {}, {}};
};

class MultibandDynamicEq : public common::ProcessorBase {
 public:
  explicit MultibandDynamicEq(MultibandDynamicEqConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandDynamicEqConfig& config);
  const MultibandDynamicEqConfig& config() const { return config_; }
  const std::vector<float>& last_detector_db() const { return last_detector_db_; }
  const std::vector<std::vector<float>>& last_applied_gain_db() const {
    return last_applied_gain_db_;
  }

 private:
  static void validate_config(const MultibandDynamicEqConfig& config);
  void rebuild_processors();
  void configure_processor(size_t band_index);

  MultibandDynamicEqConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  std::vector<eq::DynamicEq> processors_;
  std::vector<float> last_detector_db_;
  std::vector<std::vector<float>> last_applied_gain_db_;
};

}  // namespace sonare::mastering::multiband
