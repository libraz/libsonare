#pragma once

/// @file multiband_imager.h
/// @brief Multiband stereo imager using mid/side width per band.

#include <array>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "rt/processor_base.h"

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

class MultibandImager : public rt::ProcessorBase {
 public:
  explicit MultibandImager(MultibandImagerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MultibandImagerConfig& config);
  const MultibandImagerConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset).
  // Per-band block layout with kBandStride params per band: band b occupies
  // ids [b * kBandStride, b * kBandStride + kBandStride):
  //   +0 = width (clamped to >= 0)
  //   +1 = decorrelation_amount (clamped to [0, 1])
  // The allpass decorrelation coefficients are fixed, so no coefficient
  // recompute is required. Crossover cutoffs and per-band enable/preserve_energy
  // switches are not automatable.
  static constexpr unsigned int kBandStride = 2;
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  struct Allpass {
    float coefficient = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    float process(float input) noexcept;
    void reset() noexcept;
  };

  static constexpr int kNumAllpassStages = 4;

  // Target decorrelation break frequencies (Hz) for the cascaded first-order
  // all-pass network. The sign of the recomputed coefficient alternates by
  // stage (low frequency -> positive coefficient, high frequency -> negative)
  // to spread the phase response across the spectrum. These frequencies
  // reproduce the previously hardcoded 44.1 kHz coefficients
  // {0.63, -0.51, 0.42, -0.34} and are re-warped to the actual sample rate in
  // prepare() so behavior is correct at 48 k / 96 k.
  static constexpr float kDecorrelationFrequenciesHz[kNumAllpassStages] = {18917.0f, 4405.0f,
                                                                           16607.0f, 6424.0f};

  static void validate_config(const MultibandImagerConfig& config);
  static float allpass_coefficient(float frequency_hz, double sample_rate) noexcept;

  MultibandImagerConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  std::vector<std::array<Allpass, kNumAllpassStages>> allpass_;
};

}  // namespace sonare::mastering::multiband
