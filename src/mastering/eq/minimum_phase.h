#pragma once

/// @file minimum_phase.h
/// @brief Low-latency minimum-phase IIR equalizer.

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

class MinimumPhaseEq : public common::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  const EqBand& band(size_t index) const;
  int latency_samples() const { return 0; }

 private:
  ParametricEq eq_;
};

}  // namespace sonare::mastering::eq
