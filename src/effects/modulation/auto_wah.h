#pragma once

/// @file auto_wah.h
/// @brief Envelope-following resonant-bandpass auto-wah.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/svf_bandpass.h"
#include "effects/modulation/wah.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

/// Which way a rising sweep position moves the centre frequency.
enum class AutoWahDirection {
  kUp,    ///< a louder input (or an LFO peak) opens the filter upward.
  kDown,  ///< the sweep is inverted: the filter rests open and closes as it rises.
};
inline constexpr int kAutoWahDirectionCount = 2;

struct AutoWahConfig {
  float sensitivity = 1.0f;   ///< how far the envelope pushes the cutoff up.
  float min_hz = 300.0f;      ///< resting (silent) centre frequency.
  float max_hz = 2500.0f;     ///< fully-open centre frequency.
  float resonance = 4.0f;     ///< bandpass Q.
  float attack_ms = 8.0f;     ///< envelope rise time.
  float release_ms = 120.0f;  ///< envelope fall time.
  float dry_wet = 1.0f;
  /// Rate of the LFO added to the envelope's sweep position.
  float lfo_rate_hz = 1.0f;
  /// Peak of the LFO's contribution to the sweep position (0..1); 0 turns the LFO off.
  float lfo_depth = 0.0f;
  WahFilterType filter_type = WahFilterType::kBandpass;
  AutoWahDirection direction = AutoWahDirection::kUp;
  WahSweepLaw sweep_law = WahSweepLaw::kLinearHz;
};

/// A resonant bandpass whose centre frequency tracks the input level: louder
/// playing opens the filter up (touch-wah). A single shared envelope drives both
/// channels so the sweep is stereo-coherent.
class AutoWah : public rt::ProcessorBase {
 public:
  explicit AutoWah(AutoWahConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = sensitivity
  //   1 = min_hz
  //   2 = max_hz
  //   3 = resonance
  //   4 = dry_wet
  //   5 = lfo_rate_hz (clamped to >= 0; updates the LFO in place)
  //   6 = lfo_depth (clamped to [0, 1])
  //   7 = filter_type (WahFilterType; a fractional or unnamed value is refused)
  //   8 = direction (AutoWahDirection; same refusal)
  //   9 = sweep_law (WahSweepLaw; same refusal)
  // attack/release are construction-time (they set per-sample smoothing coeffs).
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  void update_coeffs();

  /// Returns the envelope follower and the sweep filters to rest once a
  /// non-finite value has reached them, once per block (see
  /// util/non_finite_state.h).
  void discard_non_finite() noexcept;

  AutoWahConfig config_{};
  double sample_rate_ = 48000.0;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  float envelope_ = 0.0f;
  Lfo lfo_;
  std::array<SvfBandpass, 2> filters_;
};

}  // namespace sonare::effects::modulation
