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
///
/// `RotaryModel::kGeometric` is a data-free alternative to the classic sine-swung
/// path: one mono horn feed heard at two microphones through distance delay,
/// inverse-distance gain and a low-pass whose corner follows the horn-to-mic
/// angle, an LR4 crossover, a 2 kHz horn peak, and a drum baffle AM sinusoidal
/// in decibels above 200 Hz. Corners, distances and depths are held in hertz,
/// metres and decibels.

#include <array>
#include <cmath>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "effects/modulation/svf_bandpass.h"
#include "rt/biquad_design.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

/// Which audio path the rotors drive.
enum class RotaryModel {
  kClassic,    ///< per-channel one-pole split, delay and gain swung by the rotor sine.
  kGeometric,  ///< two microphones around one horn; see the file comment.
};
inline constexpr int kRotaryModelCount = 2;

/// The geometric model's fixed quantities. The horn radius is `depth_ms` of sound travel, so the
/// doppler swing keeps the meaning it has in the classic model.
namespace rotary_geometry {
inline constexpr float kCrossoverHz = 800.0f;    ///< horn/drum LR4 split.
inline constexpr float kBaffleSplitHz = 200.0f;  ///< the drum band below this is not modulated.
inline constexpr float kHornPeakHz = 2000.0f;    ///< the horn's band-pass character.
inline constexpr float kHornPeakDb = 10.0f;
inline constexpr float kHornPeakQ = 1.0f;
inline constexpr float kMicGapM = 0.6f;             ///< microphone distance beyond the horn circle.
inline constexpr float kMicCentreDeg = -90.0f;      ///< both microphones at `stereo_spread` 0.
inline constexpr float kMicSpreadDeg = 45.0f;       ///< each microphone's offset at spread 1.
inline constexpr float kOnAxisCornerHz = 10000.0f;  ///< horn mouth facing the microphone.
inline constexpr float kOffAxisCornerHz = 1500.0f;  ///< horn mouth facing away.
inline constexpr float kBaffleDepthDb = 12.0f;      ///< peak-to-peak drum AM at `tremolo` 1.

/// Low-pass corner for a horn whose mouth points @p cos_angle away from the microphone:
/// geometric between the off- and on-axis corners, in (1 + cos) / 2.
inline float horn_corner_hz(float cos_angle) noexcept {
  const float t = 0.5f * (1.0f + cos_angle);
  return kOffAxisCornerHz * std::pow(kOnAxisCornerHz / kOffAxisCornerHz, t);
}
}  // namespace rotary_geometry

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
  /// L/R anti-phase amount [0, 1]: the right rotor's LFO phase offset and the mic
  /// angles; a live change shifts that phase by the delta, keeping rotor state.
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
  /// The two speeds of the speed switch, per rotor. Used only while `speed` >= 0.
  float horn_slow_hz = 0.8f;
  float horn_fast_hz = 6.0f;
  float drum_slow_hz = 0.6f;
  float drum_fast_hz = 4.44f;
  /// Speed switch: 0 slow, 1 fast (a value from 0.5 up is fast). Each rotor's target is then its
  /// slow or fast rate and `rate_hz` / `drum_rate_hz` are not read. Below zero the switch is off.
  float speed = -1.0f;
  float horn_level_db = 0.0f;  ///< horn rotor output level.
  float drum_level_db = 0.0f;  ///< drum rotor output level.
  /// How the delay lines read between samples; see DelayInterpolation.
  DelayInterpolation interpolation = DelayInterpolation::kLinear;
  /// Audio path. In the geometric model `stereo_spread` is the angle between the two
  /// microphones (0 one position, 1 at -135 and -45 degrees), `tremolo` the drum baffle depth,
  /// and the input is summed to mono.
  RotaryModel model = RotaryModel::kClassic;
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
  int tail_samples() const noexcept override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = rate_hz (the horn rotor's target; it glides there)
  //   1 = depth_ms
  //   2 = tremolo
  //   3 = dry_wet
  //   4 = drum_rate_hz (the drum rotor's target; it glides there)
  //   5..8 = horn_slow_hz, horn_fast_hz, drum_slow_hz, drum_fast_hz
  //   9 = speed (-1 off, 0 slow, 1 fast)
  //   10 = horn_level_db, 11 = drum_level_db
  //   12 = interpolation (0 linear, 1 Lagrange3)
  //   13 = model (0 classic, 1 geometric)
  //   14 = stereo_spread, 15 = accel_tau_s, 16 = decel_tau_s
  //   17 = undershoot_hz, 18 = drum_undershoot_hz
  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// The rate each rotor is turning at now, which is what drives its LFO. It
  /// equals the configured target except while a glide is in flight or where
  /// the rotor has settled short of a target it was speeding up toward.
  float horn_rate_hz() const noexcept { return static_cast<float>(horn_rate_); }
  float drum_rate_hz() const noexcept { return static_cast<float>(drum_rate_); }

 private:
  /// Returns the crossover filter to rest once a non-finite value has reached
  /// it, once per block (see util/non_finite_state.h).
  void discard_non_finite() noexcept;

  float horn_target_hz() const noexcept;
  float drum_target_hz() const noexcept;

  /// Moves one rotor a sample closer to `target` and returns its new rate.
  float advance_rotor(double& rate, float target, float undershoot) const noexcept;

  void process_geometric(float* const* channels, int active, int num_samples) noexcept;
  void reset_geometric() noexcept;
  /// Microphone azimuths, in radians, from the current stereo spread.
  void place_mics() noexcept;

  static constexpr float kCrossoverHz = 800.0f;

  RotaryConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  float lp_coeff_ = 0.0f;
  double accel_coeff_ = 1.0;  ///< per-sample gap fraction closed while speeding up.
  double decel_coeff_ = 1.0;  ///< ... and while slowing down.
  double horn_rate_ = 0.0;    ///< live rotor rates, gliding toward the config's.
  double drum_rate_ = 0.0;
  std::array<float, 2> lp_state_{{0.0f, 0.0f}};  ///< crossover lowpass memory.
  std::array<Lfo, 2> horn_lfo_;                  ///< [L, R] anti-phase.
  std::array<Lfo, 2> drum_lfo_;
  std::array<ModDelayLine, 2> horn_delay_;
  std::array<ModDelayLine, 2> drum_delay_;

  // Geometric model. The horn feed is mono, so both delay lines of a pair carry the same
  // signal and are read at each microphone's own distance.
  std::array<rt::BiquadState, 2> xover_lp_;   ///< LR4 low half (drum band).
  std::array<rt::BiquadState, 2> xover_hp_;   ///< LR4 high half (horn band).
  std::array<rt::BiquadState, 2> baffle_lp_;  ///< drum band below the baffle split.
  std::array<rt::BiquadState, 2> baffle_hp_;  ///< drum band the baffle modulates.
  rt::BiquadState horn_peak_;
  std::array<SvfBandpass, 2> horn_lp_;  ///< per-microphone angle low-pass.
  std::array<float, 2> mic_cos_{{0.0f, 0.0f}};
  std::array<float, 2> mic_sin_{{0.0f, 0.0f}};
  std::array<float, 2> mic_rad_{{0.0f, 0.0f}};
};

}  // namespace sonare::effects::modulation
