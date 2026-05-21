#pragma once

/// @file multiband_expander.h
/// @brief Multiband expander built from Crossover and per-band expanders.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/expander.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct MultibandExpanderConfig {
  CrossoverConfig crossover;
  std::vector<dynamics::ExpanderConfig> bands{
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
  };
};

class MultibandExpander : public common::ProcessorBase {
 public:
  explicit MultibandExpander(MultibandExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandExpanderConfig& config);
  const MultibandExpanderConfig& config() const { return config_; }
  const std::vector<float>& last_gain_reductions_db() const { return last_gain_reductions_db_; }

 private:
  static void validate_config(const MultibandExpanderConfig& config);
  void rebuild_processors();

  MultibandExpanderConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  std::vector<dynamics::Expander> expanders_;
  std::vector<float> last_gain_reductions_db_;
};

}  // namespace sonare::mastering::multiband
