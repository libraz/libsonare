#pragma once

/// @file minimum_phase.h
/// @brief Low-latency natural-phase equalizer backed by Vicanek matched-Z IIR.
///
/// This class is not a general arbitrary-magnitude minimum-phase FIR
/// reconstruction engine. It exposes the low-latency Natural Phase path by
/// forcing supported bands through the Vicanek coefficient design.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class MinimumPhaseEq : public common::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void prepare_channels(int num_channels);

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  // Automatable parameters: identical block-of-3 layout to ParametricEq (band
  // `b` -> ids 3*b freq, 3*b+1 gain_db, 3*b+2 Q). RT-safe; recomputes only the
  // affected band's biquad coefficients in place. Delegates to ParametricEq.
  bool set_parameter(unsigned int param_id, float value) override {
    return eq_.set_parameter(param_id, value);
  }

  const EqBand& band(size_t index) const;
  int latency_samples() const noexcept override { return 0; }

 private:
  static EqBand natural_band(EqBand band);

  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
