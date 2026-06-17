#pragma once

/// @file mid_side_eq.h
/// @brief Independent mid/side parametric equalizer.

#include <vector>

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

class MidSideEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_mid_band(size_t index, const EqBand& band);
  void set_side_band(size_t index, const EqBand& band);
  void clear_mid_band(size_t index);
  void clear_side_band(size_t index);
  void clear();

  // Automatable parameters (RT-safe: recomputes the affected band's biquad
  // coefficients in place, preserves filter state). The mid and side sections
  // each use ParametricEq's block-of-3 layout; the side section is offset by
  // `3 * kMaxBands` ids:
  //   mid band b  -> ids 3*b               .. 3*b + 2   {freq, gain_db, Q}
  //   side band b -> ids 3*kMaxBands + 3*b .. +2        {freq, gain_db, Q}
  // (with kMaxBands = 24: mid = ids 0..71, side = ids 72..143).
  bool set_parameter(unsigned int param_id, float value) override {
    constexpr unsigned int kSideBase = 3u * static_cast<unsigned int>(kMaxBands);
    if (param_id < kSideBase) {
      return mid_eq_.set_parameter(param_id, value);
    }
    return side_eq_.set_parameter(param_id - kSideBase, value);
  }

  // Automatable parameters: for each band b in [0, kMaxBands) the mid section
  // exposes ids 3*b..3*b+2 as midBand{b}.{frequencyHz,gainDb,q}, and the side
  // section exposes ids 3*kMaxBands+3*b..+2 as sideBand{b}.{frequencyHz,gainDb,q}.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  const EqBand& mid_band(size_t index) const;
  const EqBand& side_band(size_t index) const;

 private:
  void ensure_buffers(int num_samples);

  ParametricEq mid_eq_;
  ParametricEq side_eq_;
  std::vector<float> mid_buffer_;
  std::vector<float> side_buffer_;
  // Block size the RT scratch buffers were sized for in prepare(). When > 0,
  // process() chunks wider blocks to this size instead of reallocating on the
  // audio thread; 0 means "unbounded/offline" (ensure_buffers may grow off the
  // RT path on the first call).
  int max_block_size_ = 0;
};

}  // namespace sonare::mastering::eq
