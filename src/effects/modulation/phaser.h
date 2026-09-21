#pragma once

/// @file phaser.h
/// @brief Stereo first-order allpass phaser.
///
/// The cascade is a run of first-order allpass sections sharing one corner, so
/// the number of cancellations in the band is fixed by the section count alone:
/// a notch per two sections, sitting at fixed multiples of the corner read in
/// the tangent of frequency. Two structures sit over it, both off by default.
/// `mix_mode` chooses between crossfading the cascade against the dry signal
/// and summing the two, which are different effects at the same `dry_wet` --
/// a sum reaches twice the input where cascade and dry arrive in phase, a
/// crossfade never exceeds it. `feedback` closes a loop around the cascade,
/// whose return is one sample late and passes through a single high-pass pole;
/// it fills the cancellations in and lifts the peaks between them well past
/// what a sum alone can produce. Both shapes are what a period module's phaser
/// measures as.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

/// How the allpass cascade's output is combined with the dry signal.
enum class PhaserMixMode {
  kCrossfade,  ///< dry = 1 - dry_wet, wet = dry_wet; the output never exceeds the input.
  kDrySum,     ///< dry = 1, wet = dry_wet; a true sum, reaching +6 dB at dry_wet = 1.
};

struct PhaserConfig {
  float rate_hz = 0.4f;
  float min_hz = 300.0f;
  float max_hz = 1600.0f;
  int stages = 4;
  float dry_wet = 0.5f;
  /// Gain of the feedback loop closed around the cascade, clamped to +-0.95 in
  /// process(). Zero leaves the loop open and the cascade is then feed-forward.
  float feedback = 0.0f;
  PhaserMixMode mix_mode = PhaserMixMode::kCrossfade;
};

class Phaser : public rt::ProcessorBase {
 public:
  explicit Phaser(PhaserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0; updates the LFO in place)
  //   1 = min_hz (sweep lower bound)
  //   2 = max_hz (sweep upper bound)
  //   3 = dry_wet
  //   4 = feedback (clamped to [-0.95, 0.95] in process())
  // Note: `stages` is not automatable; changing it reallocates the allpass
  // state and requires prepare(). `mix_mode` is not automatable either: it
  // selects a structure rather than scaling one.
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Allpass coefficient for one channel's current LFO value, mapping the
  /// oscillator's [-1, 1] output onto the configured sweep range.
  float sweep_coeff(float lfo_value) const noexcept;
  float process_channel(float input, int channel, float coeff, float feedback);

  /// Returns the allpass sections and the feedback loop's cells to rest once a
  /// non-finite value has reached them, once per block (see
  /// util/non_finite_state.h).
  void discard_non_finite() noexcept;

  PhaserConfig config_{};
  double sample_rate_ = 48000.0;
  /// [L, R] sweep oscillators, offset by a quarter cycle in reset() so the two
  /// channels' notches never sit on the same frequencies (as in Chorus/Flanger).
  std::array<Lfo, 2> lfos_;
  std::array<std::vector<float>, 2> x1_;
  std::array<std::vector<float>, 2> y1_;
  /// [L, R] cells of the feedback path: the high-passed cascade output the next
  /// sample reads back, and the high-pass section's own input and output taps.
  std::array<float, 2> loop_return_{{0.0f, 0.0f}};
  std::array<float, 2> loop_highpass_in_{{0.0f, 0.0f}};
  std::array<float, 2> loop_highpass_out_{{0.0f, 0.0f}};
  /// Pole radius of the return path's high-pass, derived in prepare() from its
  /// corner in hertz. Never configured and never carried across a rate: it is
  /// exponential in the corner over the rate, so a stored value would move the
  /// corner every time the rate changed.
  float loop_highpass_pole_ = 0.0f;
};

}  // namespace sonare::effects::modulation
