#pragma once

/// @file rotary.h
/// @brief Rotary-speaker (Leslie) simulation: dual-rotor doppler + tremolo.
///
/// Each rotor carries a target rate and a live rate that glides toward it, so
/// a speed change takes time rather than arriving on the next sample. The
/// glide is first-order with a separate time constant per direction, which is
/// how a rotary's spin-up and spin-down are ordinarily written, and it is
/// asymmetric in a second way: speeding up settles a fixed distance short of
/// the target, slowing down arrives on it. Both time constants are held in
/// seconds and turned into per-sample coefficients in `prepare()`, so a glide
/// lasts as long at any sample rate.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct RotaryConfig {
  /// Treble horn rotor target rate. The two rotors are independent; the drum's
  /// default is 0.74 of this one's, which is the ratio the pair has always run
  /// at, but nothing holds them to it once either is set.
  float rate_hz = 6.0f;
  float drum_rate_hz = 4.44f;  ///< bass drum rotor target rate.
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
  /// How long a rotor takes to close the gap to a new target rate, one time
  /// constant per direction. Seconds rather than a per-sample coefficient, so
  /// the glide keeps its duration at every sample rate. Zero: arrive at once.
  float accel_tau_s = 0.0f;
  float decel_tau_s = 0.0f;
  /// How far short of its target a rotor settles while speeding up; slowing
  /// down arrives exactly. Zero: both directions arrive on the target.
  float undershoot_hz = 0.0f;       ///< horn rotor.
  float drum_undershoot_hz = 0.0f;  ///< drum rotor.
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
  //   0 = rate_hz (the horn rotor's target; it glides there)
  //   1 = depth_ms
  //   2 = tremolo
  //   3 = dry_wet
  //   4 = drum_rate_hz (the drum rotor's target; it glides there)
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// The rate each rotor is turning at now, which is what drives its LFO. It
  /// equals the configured target except while a glide is in flight or where
  /// the rotor has settled short of a target it was speeding up toward.
  float horn_rate_hz() const noexcept { return horn_rate_; }
  float drum_rate_hz() const noexcept { return drum_rate_; }

 private:
  /// Returns the crossover filter to rest once a non-finite value has reached
  /// it, once per block (see util/non_finite_state.h).
  void discard_non_finite() noexcept;

  /// Moves one rotor a sample closer to `target` and returns its new rate.
  float advance_rotor(float& rate, float target, float undershoot) const noexcept;

  static constexpr float kCrossoverHz = 800.0f;

  RotaryConfig config_{};
  double sample_rate_ = 48000.0;
  float lp_coeff_ = 0.0f;
  float accel_coeff_ = 1.0f;  ///< per-sample gap fraction closed while speeding up.
  float decel_coeff_ = 1.0f;  ///< ... and while slowing down.
  float horn_rate_ = 0.0f;    ///< live rotor rates, gliding toward the config's.
  float drum_rate_ = 0.0f;
  std::array<float, 2> lp_state_{{0.0f, 0.0f}};  ///< crossover lowpass memory.
  std::array<Lfo, 2> horn_lfo_;                  ///< [L, R] anti-phase.
  std::array<Lfo, 2> drum_lfo_;
  std::array<ModDelayLine, 2> horn_delay_;
  std::array<ModDelayLine, 2> drum_delay_;
};

}  // namespace sonare::effects::modulation
