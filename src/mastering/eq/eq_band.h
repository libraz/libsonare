#pragma once

/// @file eq_band.h
/// @brief Shared equalizer band model.

#include "util/constants.h"

namespace sonare::mastering::eq {

enum class EqBandType {
  Peak,
  LowShelf,
  HighShelf,
  LowPass,
  HighPass,
  BandPass,
  Notch,
  TiltShelf,
  FlatTilt,
  /// Unit magnitude everywhere; the band exists for the phase rotation it puts
  /// around its frequency, which is what aligns two sources that fight each
  /// other there. Nothing on a magnitude display moves, so a caller offering it
  /// has to say what it is. Not available in LinearPhase, which has no phase to
  /// give it.
  AllPass,
};

enum class BiquadCoeffMode {
  Rbj,
  Vicanek,
};

enum class StereoPlacement {
  Stereo,
  Left,
  Right,
  Mid,
  Side,
};

enum class PhaseMode {
  Inherit,
  ZeroLatency,
  NaturalPhase,
  LinearPhase,
};

struct DynamicParams {
  bool enabled = false;
  float threshold_db = -24.0f;
  bool auto_threshold = false;
  float ratio = 2.0f;
  float range_db = -6.0f;
  float attack_ms = 5.0f;
  float release_ms = 50.0f;
  // Delays the detector's view of the signal by this many ms; a larger value
  // makes the band react LATER, not earlier -- this is a detector delay, not
  // true look-ahead, and adds no latency to the audio path.
  float detector_delay_ms = 0.0f;
  float sidechain_freq_hz = -1.0f;
  float sidechain_q = 1.0f;
  bool external_sidechain = false;
};

struct EqBand {
  constexpr EqBand() = default;

  constexpr EqBand(EqBandType band_type, float frequency, float gain, float band_q, bool is_enabled,
                   BiquadCoeffMode coefficient_mode = BiquadCoeffMode::Rbj) noexcept
      : type(band_type),
        frequency_hz(frequency),
        gain_db(gain),
        q(band_q),
        enabled(is_enabled),
        coeff_mode(coefficient_mode) {}

  // Keep these first fields in the historical order so existing aggregate
  // initialization remains source-compatible.
  EqBandType type = EqBandType::Peak;
  float frequency_hz = 1000.0f;
  float gain_db = 0.0f;
  // Resonance / slope. NOTE: for LowShelf/HighShelf bands with
  // coeff_mode == Vicanek, q is IGNORED — the Vicanek matched-Z shelf design
  // has no Q/S parameter and uses a fixed shelf slope. q is still stored and
  // reported back verbatim, so a reflected q on a Vicanek shelf does not
  // describe the applied response. Use coeff_mode == Rbj for Q-controllable
  // shelves. (Peak/pass/notch bands honor q in both modes.)
  float q = sonare::constants::kButterworthQ;
  bool enabled = false;
  BiquadCoeffMode coeff_mode = BiquadCoeffMode::Rbj;

  // 6 selects a first-order section for pass and shelf bands, whose shelf
  // corner is its half-gain point and whose q is then unused.
  int slope_db_oct = 12;
  StereoPlacement placement = StereoPlacement::Stereo;
  PhaseMode phase = PhaseMode::Inherit;
  DynamicParams dyn;
  bool soloed = false;
  bool bypassed = false;
  bool proportional_q = false;
  float proportional_q_strength = 0.03f;
};

}  // namespace sonare::mastering::eq
