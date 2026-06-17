#pragma once

/// @file multiband_expander.h
/// @brief Multiband expander built from Crossover and per-band expanders.

#include <vector>

#include "mastering/dynamics/expander.h"
#include "mastering/multiband/crossover.h"
#include "rt/processor_base.h"

namespace sonare::mastering::multiband {

struct MultibandExpanderConfig {
  CrossoverConfig crossover;
  std::vector<dynamics::ExpanderConfig> bands{
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
      {-40.0f, 2.0f, 5.0f, 100.0f, -60.0f},
  };
};

class MultibandExpander : public rt::ProcessorBase {
 public:
  explicit MultibandExpander(MultibandExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // Reports the linear-phase FIR crossover delay (0 in the zero-latency IIR
  // modes) so host plugin-delay-compensation stays correct. The per-band
  // expanders add no latency.
  int latency_samples() const noexcept override { return crossover_.latency_samples(); }

  void set_config(const MultibandExpanderConfig& config);
  const MultibandExpanderConfig& config() const { return config_; }
  const std::vector<float>& last_gain_reductions_db() const { return last_gain_reductions_db_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset).
  // Per-band block layout with kBandStride params per band: band b occupies
  // ids [b * kBandStride, b * kBandStride + kBandStride). Within each band the
  // ids forward directly to dynamics::Expander::set_parameter:
  //   +0 = threshold_db
  //   +1 = ratio (clamped to >= 1)
  //   +2 = attack_ms (clamped to >= 0; recomputes follower coefficients)
  //   +3 = release_ms (clamped to >= 0; recomputes follower coefficients)
  //   +4 = range_db (clamped to <= 0)
  // Crossover cutoff frequencies are not automatable here: changing them
  // requires rebuilding the crossover filters and would reset audio state.
  static constexpr unsigned int kBandStride = 5;
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: band b id [b*kBandStride .. +kBandStride) maps to
  // keys band{b}.thresholdDb, band{b}.ratio, band{b}.attackMs, band{b}.releaseMs,
  // band{b}.rangeDb.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const MultibandExpanderConfig& config);
  void rebuild_processors();

  MultibandExpanderConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  std::vector<dynamics::Expander> expanders_;
  std::vector<float> last_gain_reductions_db_;
};

}  // namespace sonare::mastering::multiband
