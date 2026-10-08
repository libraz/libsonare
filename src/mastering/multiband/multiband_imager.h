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
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // Reports the linear-phase FIR crossover delay (0 in the zero-latency IIR
  // modes) so host plugin-delay-compensation stays correct. The per-band
  // imaging stages add no latency.
  int latency_samples() const noexcept override { return crossover_.latency_samples(); }

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
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters (per-band block): 0=width, 1=decorrelationAmount
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  struct Allpass {
    float coefficient = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    float process(float input) noexcept;
    void reset() noexcept;
  };

  static constexpr int kNumAllpassStages = 4;

  // Decorrelation break frequencies (Hz) of the cascaded first-order all-pass
  // network: each stage's -90 degree point, held in Hz so prepare() places it
  // there at every rate. At 44.1 kHz they give the coefficients
  // {0.63, -0.51, 0.42, -0.34}, alternating low and high to spread the phase.
  static constexpr float kDecorrelationFrequenciesHz[kNumAllpassStages] = {3133.0f, 17645.0f,
                                                                           5443.0f, 15626.0f};

  static void validate_config(const MultibandImagerConfig& config);
  static float allpass_coefficient(float frequency_hz, double sample_rate) noexcept;

  MultibandImagerConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_working_channels_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  std::vector<std::array<Allpass, kNumAllpassStages>> allpass_;
};

}  // namespace sonare::mastering::multiband
