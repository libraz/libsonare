#pragma once

/// @file multiband_dynamic_eq.h
/// @brief Multiband dynamic EQ built from Crossover and per-band DynamicEq processors.

#include <vector>

#include "mastering/eq/dynamic_eq.h"
#include "mastering/multiband/crossover.h"
#include "rt/processor_base.h"

namespace sonare::mastering::multiband {

struct MultibandDynamicEqConfig {
  CrossoverConfig crossover;
  std::vector<std::vector<eq::DynamicEqBand>> bands{{}, {}, {}};
};

class MultibandDynamicEq : public rt::ProcessorBase {
 public:
  explicit MultibandDynamicEq(MultibandDynamicEqConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // Reports the linear-phase FIR crossover delay (0 in the zero-latency IIR
  // modes) so host plugin-delay-compensation stays correct. The per-band
  // DynamicEq processors are zero-latency (the detector lookahead does not
  // delay the audio path).
  int latency_samples() const noexcept override { return crossover_.latency_samples(); }

  // Number of automatable parameters per crossover band: every crossover band
  // owns a full DynamicEq, so it spans kMaxBands * DynamicEq::kParamsPerBand
  // ids (= 8 * 11 = 88).
  static constexpr unsigned int kParamsPerCrossoverBand =
      static_cast<unsigned int>(eq::DynamicEq::kMaxBands) * eq::DynamicEq::kParamsPerBand;

  void set_config(const MultibandDynamicEqConfig& config);
  const MultibandDynamicEqConfig& config() const { return config_; }
  const std::vector<float>& last_detector_db() const { return last_detector_db_; }
  const std::vector<std::vector<float>>& last_applied_gain_db() const {
    return last_applied_gain_db_;
  }

  // Automatable parameters (RT-safe: delegates to the per-crossover-band
  // DynamicEq, which recomputes the affected band's coefficients in place
  // without resetting filter/envelope state). Parameters are laid out in
  // per-crossover-band blocks of kParamsPerCrossoverBand (= 88); crossover band
  // `cb` occupies `kParamsPerCrossoverBand*cb .. +87`. Within each block the
  // offset is the DynamicEq band layout (DynamicEq::kParamsPerBand fields per
  // dynamic band; see DynamicEq::set_parameter for the field order). Dynamic
  // bands that are disabled (the default) are no-ops until enabled via config.
  // Ids past the last crossover band are rejected (return false).
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const MultibandDynamicEqConfig& config);
  void rebuild_processors();
  void configure_processor(size_t band_index);

  MultibandDynamicEqConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Crossover crossover_;
  CrossoverScratch scratch_;
  std::vector<eq::DynamicEq> processors_;
  std::vector<float> last_detector_db_;
  std::vector<std::vector<float>> last_applied_gain_db_;
};

}  // namespace sonare::mastering::multiband
