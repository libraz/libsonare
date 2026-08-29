#pragma once

/// @file gs_master_eq.h
/// @brief The GS master EQ at `40 02 00`-`03`: the two shelving bands as they
///        arrive on the wire, their power-on defaults, the byte -> physical-unit
///        conversions, and the stereo filter that runs them on the output.
///
/// The filter owns no allocation — two biquads per channel — so a coefficient
/// change applies to the running stage rather than rebuilding it, and set() is
/// safe from whichever thread owns the units. A band at 0 dB is skipped, which
/// is what makes the power-on state exactly transparent.
///
/// Per-part bypass is `40 4x 20`, held by the player rather than here: the EQ
/// is one stage on the output and the switch decides what reaches it.
///
/// Ranges and defaults are the Roland SC-8850 Owner's Manual, "Parameter
/// Address Map"; the shelf slope is ours, the manual gives none.

#include <cstdint>

#include "rt/biquad_design.h"

namespace sonare::midi::synth {

/// Raw wire state of the master EQ block, at the GS power-on defaults.
struct GsMasterEq {
  uint8_t low_freq = 0x00;   ///< 40 02 00, 0-1: 200 Hz / 400 Hz.
  uint8_t low_gain = 0x40;   ///< 40 02 01, 34-4C = -12..+12 dB, 40 = flat.
  uint8_t high_freq = 0x00;  ///< 40 02 02, 0-1: 3 kHz / 6 kHz.
  uint8_t high_gain = 0x40;  ///< 40 02 03, 34-4C, 40 = flat.
};

/// The two corners each FREQ address selects.
inline constexpr float kGsEqLowFreqHz[2] = {200.0f, 400.0f};
inline constexpr float kGsEqHighFreqHz[2] = {3000.0f, 6000.0f};

/// EQ LOW/HIGH FREQ 0-1 -> corner in Hz; any other value reads as 0.
float gs_eq_low_freq_hz(uint8_t value) noexcept;
float gs_eq_high_freq_hz(uint8_t value) noexcept;

/// EQ LOW/HIGH GAIN `34`-`4C` -> dB, `40` = 0 dB, clamped to +-12 dB.
float gs_eq_gain_db(uint8_t value) noexcept;

/// True when both bands sit at 0 dB — the power-on state, where the EQ is a
/// bit-exact pass-through.
bool gs_master_eq_is_flat(const GsMasterEq& eq) noexcept;

/// Stereo two-band shelving EQ across the output.
class GsMasterEqFilter {
 public:
  /// CONTROL thread: bind the design to @p sample_rate and clear the histories.
  void prepare(double sample_rate) noexcept;

  /// Recompute the shelf coefficients for @p eq. Allocation-free and leaves the
  /// filter histories, so a live edit keeps the signal continuous.
  void set(const GsMasterEq& eq) noexcept;

  void reset() noexcept;

  /// True when at least one band is off 0 dB and process() does anything.
  bool active() const noexcept { return low_active_ || high_active_; }

  /// AUDIO thread: filter @p n frames in place. A null channel is skipped.
  void process(float* left, float* right, int n) noexcept;

 private:
  double sample_rate_ = 48000.0;
  bool low_active_ = false;
  bool high_active_ = false;
  rt::BiquadState low_[2];
  rt::BiquadState high_[2];
};

}  // namespace sonare::midi::synth
