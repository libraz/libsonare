#pragma once

/// @file graphic_eq.h
/// @brief 31-band graphic equalizer.

#include <array>
#include <cstddef>
#include <vector>

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"
#include "rt/tail_budget.h"

namespace sonare::mastering::eq {

class GraphicEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kNumBands = 31;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override {
    rt::TailBudget tail = rt::TailBudget::reported(low_eq_.tail_samples());
    return tail.then(rt::TailBudget::reported(high_eq_.tail_samples())).samples();
  }

  void set_gain_db(size_t index, float gain_db);
  void set_gain_for_frequency(float frequency_hz, float gain_db);
  void clear();
  /// @brief Sets one Q for every band; 0 restores the gain-derived per-band Q.
  /// @details Redesigns the band filters in place (no allocation, state kept).
  void set_q(float q);

  // Automatable parameters (RT-safe: recomputes only the affected band's biquad
  // coefficients in place, preserves filter state). Each of the 31 ISO bands
  // exposes a single gain control; param_id is the band index:
  //   id b (0 .. kNumBands-1) = gain_db for band b (center frequency is fixed;
  //                             band Q is derived from gain via
  //                             band_q_for_gain_db()).
  //   id kNumBands            = q shared by all bands (0 = the gain-derived default).
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: id b (0 .. kNumBands-1) = "band<b>GainDb"; id kNumBands = "q".
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  float gain_db(size_t index) const;
  float center_frequency(size_t index) const;
  size_t nearest_band(float frequency_hz) const;
  static float band_q_for_gain_db(float gain_db);

 private:
  void rebuild_bands();
  // Recomputes a single band's coefficients in place (no state reset).
  void rebuild_band(size_t index);
  static void validate_index(size_t index);

  ParametricEq low_eq_;
  ParametricEq high_eq_;
  std::array<float, kNumBands> gains_db_{};
  // Captured in prepare(); used to clamp fixed ISO center frequencies to the
  // open (0 Hz, Nyquist) interval so bands above Nyquist do not throw.
  double sample_rate_ = 44100.0;
  // Shared band Q; 0 keeps band_q_for_gain_db().
  float q_ = 0.0f;
};

}  // namespace sonare::mastering::eq
