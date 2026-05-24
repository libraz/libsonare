#pragma once

/// @file fdn_reverb.h
/// @brief Small colorless-style feedback delay network reverb.

#include <array>

#include "effects/common/dc_blocker.h"
#include "rt/delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::reverb {

struct FdnReverbConfig {
  float decay = 0.55f;        ///< T60_lf = max(0.01, decay*10) seconds.
  float hf_damping = 0.5f;    ///< HF decay shortening, [0, 1].
  float dry_wet = 0.35f;
};

class FdnReverb : public rt::ProcessorBase {
 public:
  explicit FdnReverb(FdnReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

 private:
  FdnReverbConfig config_{};
  std::array<rt::DelayLine, 4> delays_;
  std::array<float, 4> state_{{0.0f, 0.0f, 0.0f, 0.0f}};
  // Per-line one-pole absorption filters (frequency-dependent decay).
  std::array<float, 4> a1_{{0.0f, 0.0f, 0.0f, 0.0f}};
  std::array<float, 4> b0_{{0.0f, 0.0f, 0.0f, 0.0f}};
  std::array<float, 4> filt_state_{{0.0f, 0.0f, 0.0f, 0.0f}};
  effects::common::DcBlocker dc_blocker_;
};

}  // namespace sonare::effects::reverb
