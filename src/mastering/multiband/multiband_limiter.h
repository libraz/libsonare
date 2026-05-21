#pragma once

/// @file multiband_limiter.h
/// @brief Multiband limiter built from Crossover and per-band limiters.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct MultibandLimiterConfig {
  CrossoverConfig crossover;
  std::vector<dynamics::LimiterConfig> bands{
      {-1.0f, 0.0f, 50.0f},
      {-1.0f, 0.0f, 50.0f},
      {-1.0f, 0.0f, 50.0f},
  };
};

class MultibandLimiter : public common::ProcessorBase {
 public:
  explicit MultibandLimiter(MultibandLimiterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandLimiterConfig& config);
  const MultibandLimiterConfig& config() const { return config_; }
  const std::vector<float>& last_gain_reductions_db() const { return last_gain_reductions_db_; }

 private:
  static void validate_config(const MultibandLimiterConfig& config);
  void rebuild_processors();

  MultibandLimiterConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  std::vector<dynamics::Limiter> limiters_;
  std::vector<float> last_gain_reductions_db_;
};

}  // namespace sonare::mastering::multiband
