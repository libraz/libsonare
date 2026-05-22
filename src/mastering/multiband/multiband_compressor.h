#pragma once

/// @file multiband_compressor.h
/// @brief Multiband compressor built from Crossover and per-band compressors.

#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

struct MultibandCompressorConfig {
  CrossoverConfig crossover;
  std::vector<dynamics::CompressorConfig> bands{
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
  };
};

class MultibandCompressor : public common::ProcessorBase {
 public:
  explicit MultibandCompressor(MultibandCompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandCompressorConfig& config);
  const MultibandCompressorConfig& config() const { return config_; }
  const std::vector<float>& last_gain_reductions_db() const { return last_gain_reductions_db_; }

 private:
  static void validate_config(const MultibandCompressorConfig& config);
  void rebuild_processors();

  MultibandCompressorConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  std::vector<dynamics::Compressor> compressors_;
  std::vector<float> last_gain_reductions_db_;
};

}  // namespace sonare::mastering::multiband
