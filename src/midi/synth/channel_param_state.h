#pragma once

#include <cstdint>

namespace sonare::midi::synth {

/// Tracks MIDI RPN/NRPN selection and data-entry routing for one channel.
struct ChannelParamState {
  enum class Mode : uint8_t { kNone = 0, kRpn, kNrpn };

  Mode mode = Mode::kNone;
  uint8_t rpn_msb = 127;
  uint8_t rpn_lsb = 127;
  uint8_t nrpn_msb = 127;
  uint8_t nrpn_lsb = 127;

  void reset() noexcept {
    mode = Mode::kNone;
    rpn_msb = 127;
    rpn_lsb = 127;
    nrpn_msb = 127;
    nrpn_lsb = 127;
  }

  void select_rpn_msb(uint8_t value) noexcept {
    rpn_msb = value;
    mode = Mode::kRpn;
  }

  void select_rpn_lsb(uint8_t value) noexcept {
    rpn_lsb = value;
    mode = Mode::kRpn;
  }

  void select_nrpn_msb(uint8_t value) noexcept {
    nrpn_msb = value;
    mode = Mode::kNrpn;
  }

  void select_nrpn_lsb(uint8_t value) noexcept {
    nrpn_lsb = value;
    mode = Mode::kNrpn;
  }

  bool selected_rpn(uint8_t msb, uint8_t lsb) const noexcept {
    return mode == Mode::kRpn && rpn_msb == msb && rpn_lsb == lsb;
  }

  bool selected_nrpn() const noexcept { return mode == Mode::kNrpn; }
};

/// The eight GS part edits (40 1x 30 TONE MODIFY and the NRPNs it aliases)
/// reduced to quantities a voice applies directly. Defaults are no-ops.
///
/// It sits here, below either voice bank, so both consume one conversion
/// instead of each making its own; gs_part_mod() in gs_layer.h builds it.
struct GsPartMod {
  float cutoff_cents = 0.0f;     ///< added to the voice's filter cutoff offset.
  float resonance_gain = 1.0f;   ///< multiplies filter Q (floored at 0.5 by the caller).
  float attack_scale = 1.0f;     ///< multiplies the amplitude envelope's attack.
  float decay_scale = 1.0f;      ///< ... its decay.
  float release_scale = 1.0f;    ///< ... its release.
  float vib_rate_scale = 1.0f;   ///< multiplies the vibrato LFO frequency.
  float vib_depth_cents = 0.0f;  ///< added to the LFO's pitch depth (floored at 0).
  float vib_delay_scale = 1.0f;  ///< feeds gs_vib_delay_seconds, which is not a plain scale.

  /// True when the filter stage has been edited, which engages it: the manual
  /// gives no way to ask for a filter and then not hear it.
  bool filter_edited = false;
};

/// The LFO onset delay after a vibrato-delay edit of @p scale on a voice whose
/// own delay is @p base_s. Not a plain multiply: a base of zero would stay zero,
/// so a positive edit gives it an onset instead of nothing. The model bank's
/// LFO always starts at zero, so this is the only thing it can mean there.
inline float gs_vib_delay_seconds(float base_s, float scale) noexcept {
  const float delay_s = base_s * scale;
  if (delay_s < 1.0e-3f && scale > 1.0f) return 0.05f * (scale - 1.0f);
  return delay_s;
}

}  // namespace sonare::midi::synth
