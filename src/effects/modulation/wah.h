#pragma once

/// @file wah.h
/// @brief LFO-swept resonant-bandpass wah.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/svf_bandpass.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct WahConfig {
  float rate_hz = 1.5f;    ///< sweep LFO rate (0 = a fixed mid-sweep wah).
  float min_hz = 400.0f;   ///< sweep lower bound.
  float max_hz = 2000.0f;  ///< sweep upper bound.
  float resonance = 4.0f;  ///< bandpass Q (the wah "peakiness").
  float dry_wet = 1.0f;    ///< the wah is an insert, so wet by default.
};

/// A resonant bandpass whose centre frequency is swept between min_hz and max_hz
/// by an LFO — the classic auto-wah / pedal-wah timbre. The two channels share
/// the sweep phase so the stereo image stays coherent.
class Wah : public rt::ProcessorBase {
 public:
  explicit Wah(WahConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = rate_hz (updates the LFO in place)
  //   1 = min_hz
  //   2 = max_hz
  //   3 = resonance
  //   4 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  WahConfig config_{};
  double sample_rate_ = 48000.0;
  Lfo lfo_;
  std::array<SvfBandpass, 2> filters_;
};

}  // namespace sonare::effects::modulation
