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
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const MultibandExciterConfig& config);
  const MultibandExciterConfig& config() const { return config_; }

  /// Reports the crossover delay (non-zero for FIR linear-phase mode) so the
  /// host can compensate, matching the other multiband processors.
  int latency_samples() const noexcept override { return crossover_.latency_samples(); }

  // Automatable parameters (RT-safe, no allocation, no state reset).
  // Per-band block layout with kBandStride params per band: band b occupies
  // ids [b * kBandStride, b * kBandStride + kBandStride). Within each band the
  // ids forward directly to Exciter::set_parameter:
  //   +0 = frequency_hz (clamped positive; recomputes the band filters)
  //   +1 = drive_db
  //   +2 = amount (clamped to >= 0)
  //   +3 = q (clamped positive; recomputes the band filters)
  //   +4 = even_odd_mix (clamped to [0, 1])
  // The kept config_ mirror stays consistent so config() reflects automation.
  // Crossover cutoff frequencies are not automatable here: changing them
  // requires rebuilding the crossover filters and would reset audio state.
  static constexpr unsigned int kBandStride = 5;
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: band b id [b*kBandStride .. +kBandStride) maps to
  // keys band{b}.frequencyHz, band{b}.driveDb, band{b}.amount, band{b}.q,
  // band{b}.evenOddMix -- the same keys multiband_exciter_config() reads.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const MultibandExciterConfig& config);
  void rebuild_processors();

  MultibandExciterConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_working_channels_ = 0;
  bool prepared_ = false;
  multiband::Crossover crossover_;
  multiband::CrossoverScratch scratch_;
  std::vector<Exciter> exciters_;
};

}  // namespace sonare::mastering::saturation
