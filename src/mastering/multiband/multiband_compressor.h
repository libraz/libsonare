#pragma once

/// @file multiband_compressor.h
/// @brief Multiband compressor built from Crossover and per-band compressors.

#include <vector>

#include "mastering/dynamics/compressor.h"
#include "mastering/multiband/crossover.h"
#include "rt/processor_base.h"

namespace sonare::mastering::multiband {

struct MultibandCompressorConfig {
  CrossoverConfig crossover;
  std::vector<dynamics::CompressorConfig> bands{
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
      {-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 0.0f, false, dynamics::DetectorMode::Rms},
  };
};

class MultibandCompressor : public rt::ProcessorBase {
 public:
  explicit MultibandCompressor(MultibandCompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandCompressorConfig& config);
  const MultibandCompressorConfig& config() const { return config_; }
  const std::vector<float>& last_gain_reductions_db() const { return last_gain_reductions_db_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset).
  // Per-band block layout with kBandStride params per band: band b occupies
  // ids [b * kBandStride, b * kBandStride + kBandStride). Within each band the
  // ids forward directly to dynamics::Compressor::set_parameter:
  //   +0 = threshold_db
  //   +1 = ratio (clamped to >= 1)
  //   +2 = attack_ms (clamped to >= 0; recomputes smoother coefficients)
  //   +3 = release_ms (clamped to >= 0; recomputes smoother coefficients)
  //   +4 = makeup_gain_db
  // Crossover cutoff frequencies are not automatable here: changing them
  // requires rebuilding the crossover filters and would reset audio state.
  static constexpr unsigned int kBandStride = 5;
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const MultibandCompressorConfig& config);
  void rebuild_processors();

  MultibandCompressorConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  std::vector<dynamics::Compressor> compressors_;
  std::vector<float> last_gain_reductions_db_;
};

}  // namespace sonare::mastering::multiband
