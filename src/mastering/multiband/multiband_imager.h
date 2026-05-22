#pragma once

/// @file multiband_imager.h
/// @brief Multiband stereo imager using mid/side width per band.

#include <array>
#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct ImagerBandConfig {
  float width = 1.0f;
  bool enabled = true;
  float decorrelation_amount = 0.0f;
  bool preserve_energy = true;
};

struct MultibandImagerConfig {
  CrossoverConfig crossover;
  std::vector<ImagerBandConfig> bands{
      {},
      {},
      {},
  };
};

class MultibandImager : public common::ProcessorBase {
 public:
  explicit MultibandImager(MultibandImagerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandImagerConfig& config);
  const MultibandImagerConfig& config() const { return config_; }

 private:
  struct Allpass {
    float coefficient = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    float process(float input) noexcept;
    void reset() noexcept;
  };

  static void validate_config(const MultibandImagerConfig& config);

  MultibandImagerConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  std::vector<std::array<Allpass, 4>> allpass_;
};

}  // namespace sonare::mastering::multiband
