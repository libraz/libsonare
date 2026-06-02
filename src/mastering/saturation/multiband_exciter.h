#pragma once

#include <vector>

#include "mastering/multiband/crossover.h"
#include "mastering/saturation/exciter.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct MultibandExciterConfig {
  multiband::CrossoverConfig crossover;
  std::vector<ExciterConfig> bands{
      {},
      {},
      {},
  };
};

class MultibandExciter : public rt::ProcessorBase {
 public:
  explicit MultibandExciter(MultibandExciterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const MultibandExciterConfig& config);
  const MultibandExciterConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). Each id is
  // forwarded to every band's Exciter::set_parameter, which updates its config
  // and recomputes any biquad coefficients in place:
  //   0 = frequency_hz (all bands)
  //   1 = drive_db (all bands)
  //   2 = amount (all bands)
  //   3 = q (all bands)
  //   4 = even_odd_mix (all bands)
  // The kept config_ mirror stays consistent so config() reflects automation.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const MultibandExciterConfig& config);
  void rebuild_processors();

  MultibandExciterConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  multiband::Crossover crossover_;
  multiband::CrossoverScratch scratch_;
  std::vector<Exciter> exciters_;
};

}  // namespace sonare::mastering::saturation
