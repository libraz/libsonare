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
  float lookahead_ms = 0.0f;
  float sidechain_freq_hz = -1.0f;
  float sidechain_q = 1.0f;
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
  float q = sonare::constants::kButterworthQ;
  bool enabled = false;
  BiquadCoeffMode coeff_mode = BiquadCoeffMode::Rbj;

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
