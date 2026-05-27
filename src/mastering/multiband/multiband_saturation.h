#pragma once

/// @file multiband_saturation.h
/// @brief Multiband saturation processor that delegates each band to a real
///        saturation processor (soft clipper, tape, tube, or exciter).

#include <memory>
#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/multiband/crossover.h"

namespace sonare::mastering::multiband {

/// @brief Saturation algorithm used for a band.
enum class SaturationType {
  SoftClip,
  Tape,
  Tube,
  Exciter,
};

struct SaturationBandConfig {
  float drive_db = 0.0f;
  float mix = 1.0f;
  float output_gain_db = 0.0f;
  bool enabled = true;
  // Appended after the original four fields so existing aggregate
  // initializations ({drive_db, mix, output_gain_db, enabled}) keep compiling.
  SaturationType type = SaturationType::SoftClip;
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
  ~MultibandSaturation() override;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandSaturationConfig& config);
  const MultibandSaturationConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset).
  // Per-band block layout with kBandStride params per band: band b occupies
  // ids [b * kBandStride, b * kBandStride + kBandStride):
  //   +0 = drive_db
  //   +1 = mix (clamped to [0, 1])
  //   +2 = output_gain_db
  // The underlying saturation processor's drive/mix are updated in place; the
  // band output gain is applied post-processing. Crossover cutoffs, the
  // per-band enable switch, and the saturation type are not automatable.
  static constexpr unsigned int kBandStride = 3;
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const MultibandSaturationConfig& config);
  void rebuild_processors();

  MultibandSaturationConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  // One real saturation processor per band (type chosen by config). Created in
  // rebuild_processors()/prepare(); never allocated on the audio thread.
  std::vector<std::unique_ptr<common::ProcessorBase>> processors_;
};

}  // namespace sonare::mastering::multiband
