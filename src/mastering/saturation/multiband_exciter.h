#pragma once

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/multiband/crossover.h"
#include "mastering/saturation/exciter.h"

namespace sonare::mastering::saturation {

struct MultibandExciterConfig {
  multiband::CrossoverConfig crossover;
  std::vector<ExciterConfig> bands{
      {},
      {},
      {},
  };
};

class MultibandExciter : public common::ProcessorBase {
 public:
  explicit MultibandExciter(MultibandExciterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const MultibandExciterConfig& config);
  const MultibandExciterConfig& config() const { return config_; }

 private:
  static void validate_config(const MultibandExciterConfig& config);
  void rebuild_processors();

  MultibandExciterConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  multiband::Crossover crossover_;
  std::vector<Exciter> exciters_;
};

}  // namespace sonare::mastering::saturation
