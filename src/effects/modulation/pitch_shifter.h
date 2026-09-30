#pragma once

/// @file pitch_shifter.h
/// @brief Real-time time-domain pitch shifter (dual crossfaded delay taps).

#include <array>
#include <vector>

#include "effects/common/mix_law.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/biquad_design.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

/// The processor splices twice per grain, so the drift between splices -- the
/// window -- is half the grain span.
constexpr float kDefaultWindowMs = 22.5f;

struct PitchShifterConfig {
  float semitones = 0.0f;  ///< shift amount; +12 = one octave up.
  float dry_wet = 1.0f;
  /// Distance the read-out drifts between splices, in milliseconds, and the
  /// distance the two taps sit apart in the delay line. The output repeats once
  /// per window of drift, so its beat period is `window_ms / |ratio - 1|`.
  float window_ms = kDefaultWindowMs;
  float cents = 0.0f;  ///< voice 1 fine offset added to `semitones`, in [-100, 100].
  /// Voice 1 balance, -1 left to +1 right; 0 leaves both sides at unity.
  float pan = 0.0f;
  float semitones2 = 0.0f;  ///< voice 2 shift, clamped to [-24, 24].
  float cents2 = 0.0f;      ///< voice 2 fine offset, in [-100, 100].
  float level2 = 0.0f;      ///< voice 2 linear gain in [0, 1]; 0 switches the voice off.
  float pan2 = 0.0f;        ///< voice 2 balance, as `pan`.
  /// Delay of each voice's read-out ahead of the shifter, in milliseconds. The
  /// delay line is sized from these at prepare(), so they are construction-only:
  /// no automation id, and the reported latency does not include them.
  float pre_delay_ms = 0.0f;
  float pre_delay2_ms = 0.0f;
  /// Fraction of the shifted sum written back into the delay line, in [-0.95, 0.95].
  float feedback = 0.0f;
  common::MixLaw mix_law = common::MixLaw::kCrossfade;
  /// How the taps read between samples; see DelayInterpolation. Lagrange3 reads
  /// no closer than one sample behind the write head.
  DelayInterpolation interpolation = DelayInterpolation::kLinear;
  /// When a voice's ratio exceeds 1, a fourth-order Linkwitz-Riley low-pass at
  /// fs / (2 * ratio) runs ahead of the write, so the content the shift would
  /// fold back above Nyquist is removed first. The corner follows the larger of
  /// the sounding voices' ratios.
  bool anti_alias = false;
};

/// A classic H910-style pitch shifter: a delay line read by two taps one window
/// apart, each tap's delay ramped at the pitch ratio and the two crossfaded by
/// an equal-power grain so the wrap discontinuity is masked. Each tap wraps once
/// per grain and they are staggered by half of one, so the splice rate -- and
/// with it the audible beat -- is one per window of drift.
/// No FFT, so it is realtime-safe and low-latency (offline, higher-quality
/// spectral pitch shifting lives in effects/pitch_shift.h).
class PitchShifter : public rt::ProcessorBase {
 public:
  explicit PitchShifter(PitchShifterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = semitones (clamped to [-24, 24]), 1 = dry_wet, 2 = cents, 3 = pan,
  //   4 = semitones2, 5 = cents2, 6 = level2, 7 = pan2, 8 = feedback,
  //   9 = mix_law (a whole number naming a law, refused otherwise)
  //   10 = interpolation (0 linear, 1 Lagrange3), 11 = anti_alias (0 or 1)
  // The pre-delays have no id: they size the delay line.
  bool set_parameter_impl(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  float read_tap(int channel, float delay) const noexcept;
  float read_tap_lagrange3(int channel, float delay) const noexcept;

  /// Derives the anti-alias corner, in hertz, from the larger sounding ratio and
  /// rebuilds the low-pass sections when it moved. A ratio of 1 or less leaves
  /// the section out of the path.
  void update_anti_alias(float max_ratio) noexcept;

  PitchShifterConfig config_{};
  double sample_rate_ = 48000.0;
  int grain_ = 2048;     ///< grain length in samples: two of config_.window_ms.
  float phase_ = 0.0f;   ///< voice 1 tap-1 delay position in [0, grain_).
  float phase2_ = 0.0f;  ///< the same for voice 2.
  std::array<float, 2> pre_delay_samples_{{0.0f, 0.0f}};
  std::array<float, 2> feedback_state_{{0.0f, 0.0f}};  ///< last shifted sum, per channel.
  std::array<std::vector<float>, 2> buffers_;
  std::array<int, 2> write_pos_{{0, 0}};
  /// Corner of the anti-alias low-pass in hertz; 0 while the section is out of the path.
  float anti_alias_corner_hz_ = 0.0f;
  /// [channel][section]: two Butterworth sections make one Linkwitz-Riley 4.
  std::array<std::array<rt::BiquadState, 2>, 2> anti_alias_;
};

}  // namespace sonare::effects::modulation
