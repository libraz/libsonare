#pragma once

/// @file rotary.h
/// @brief Rotary-speaker (Leslie) simulation: dual-rotor doppler + tremolo.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct RotaryConfig {
  float rate_hz = 6.0f;  ///< horn rotor rate (the drum rotor tracks slower).
  /// Peak doppler delay swing. The modulated delay is centred on this value
  /// rather than on zero, so the effect carries a mean delay of `depth_ms` -
  /// 1.2 ms by default. That delay is part of the modulation, not a processing
  /// latency: it swings continuously and has no steady arrival point, so it is
  /// deliberately not reported through `latency_samples()` and is not
  /// compensated by mixer PDC. The whole modulation family follows this
  /// convention (`ChorusConfig::center_delay_ms` is 14 ms on the same terms).
  float depth_ms = 1.2f;
  float tremolo = 0.5f;  ///< amplitude-modulation depth [0, 1].
  /// L/R anti-phase amount [0, 1]. Construction/reset-only: changing it
  /// requires reconstructing or resetting the effect, so it is intentionally
  /// absent from the realtime automation parameter list.
  float stereo_spread = 1.0f;
  float dry_wet = 1.0f;
};

/// A two-rotor rotary-speaker model: the signal is split by a crossover into a
/// treble horn and a bass drum, each rotor imparting a pitch (doppler, via a
/// modulated delay) and amplitude (tremolo) modulation. The horn and drum spin
/// at different rates and the two channels are driven anti-phase, giving the
/// characteristic swirling stereo image.
class Rotary : public rt::ProcessorBase {
 public:
  explicit Rotary(RotaryConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = rate_hz (updates both rotor LFOs in place)
  //   1 = depth_ms
  //   2 = tremolo
  //   3 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static constexpr float kCrossoverHz = 800.0f;
  static constexpr float kDrumRateRatio = 0.74f;  ///< bass rotor spins slower.

  RotaryConfig config_{};
  double sample_rate_ = 48000.0;
  float lp_coeff_ = 0.0f;
  std::array<float, 2> lp_state_{{0.0f, 0.0f}};  ///< crossover lowpass memory.
  std::array<Lfo, 2> horn_lfo_;                  ///< [L, R] anti-phase.
  std::array<Lfo, 2> drum_lfo_;
  std::array<ModDelayLine, 2> horn_delay_;
  std::array<ModDelayLine, 2> drum_delay_;
};

}  // namespace sonare::effects::modulation
