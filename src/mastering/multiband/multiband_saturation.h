#pragma once

/// @file multiband_saturation.h
/// @brief Multiband saturation processor that delegates each band to a real
///        saturation processor (soft clipper, tape, tube, or exciter).

#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "rt/parallel_paths.h"
#include "rt/processor_base.h"
#include "rt/stage_gate.h"

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

  bool operator==(const SaturationBandConfig& other) const {
    return drive_db == other.drive_db && mix == other.mix &&
           output_gain_db == other.output_gain_db && enabled == other.enabled && type == other.type;
  }
  bool operator!=(const SaturationBandConfig& other) const { return !(*this == other); }
};

struct MultibandSaturationConfig {
  CrossoverConfig crossover;
  std::vector<SaturationBandConfig> bands{
      {},
      {},
      {},
  };
};

class MultibandSaturation : public rt::ProcessorBase {
 public:
  explicit MultibandSaturation(MultibandSaturationConfig config = {});
  ~MultibandSaturation() override;

  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // The linear-phase FIR crossover delay (0 in the zero-latency IIR modes) plus
  // the deepest per-band saturation delay, to which band_paths_ pads every
  // shallower band before the sum.
  int latency_samples() const noexcept override { return latency_samples_q8() >> 8; }
  int latency_samples_q8() const noexcept override {
    return (crossover_.latency_samples() << 8) + band_paths_.latency_samples_q8();
  }
  /// The band split, then the longest band stage's ring.
  int tail_samples() const noexcept override {
    rt::TailBudget bands;
    for (const auto& processor : processors_) {
      bands.alongside(rt::TailBudget::reported(processor->tail_samples()));
    }
    rt::TailBudget tail = crossover_.tail();
    return tail.then(bands).samples();
  }

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
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters (per-band block): 0=driveDb, 1=mix, 2=outputGainDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const MultibandSaturationConfig& config);
  void rebuild_processors();
  /// @brief Delay one band's stage contributes at the summing point, in Q8
  ///        samples. A disabled band bypasses its stage, so it contributes none.
  int band_latency_q8(size_t band) const noexcept;
  /// @brief Declares every band's delay to band_paths_ so each arrives at the
  ///        sum aligned to the deepest. Control thread only (allocates).
  void rebuild_band_compensation();

  MultibandSaturationConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_working_channels_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  // One real saturation processor per band (type chosen by config). Created in
  // rebuild_processors()/prepare(); never allocated on the audio thread.
  std::vector<std::unique_ptr<rt::ProcessorBase>> processors_;
  std::vector<rt::StageGate> band_gates_;
  // One path per band. Without the alignment the crossover's allpass
  // reconstruction is summed from bands that no longer share a time reference.
  rt::ParallelPaths band_paths_;
  // The tape model has no mix of its own, so a Tape band's dry/wet blend runs
  // here: per band, path 0 is the band input and path 1 the tape output.
  // Bands of other types leave theirs empty.
  std::vector<rt::ParallelPaths> blend_paths_;
  std::vector<std::vector<float>> dry_scratch_;
};

}  // namespace sonare::mastering::multiband
