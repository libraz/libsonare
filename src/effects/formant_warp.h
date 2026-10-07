#pragma once

/// @file formant_warp.h
/// @brief Lightweight formant-colour warp using LPC analysis context.
///
/// Core-side rather than voice-changer-side: note editing warps formants too,
/// and its build option is the one the voice changer already requires.

#include "core/audio.h"

namespace sonare {

/// Factor range the warp is defined over. It clamps its own factor to these, so
/// they belong to the warp rather than to any one caller's parameter policy.
inline constexpr float kFormantFactorMin = 0.55f;
inline constexpr float kFormantFactorMax = 1.65f;

/// Analysis frame of the warp, in samples at 48 kHz (about 21.3 ms). The hop is a
/// quarter of the frame and the FFT twice the frame.
inline constexpr int kFormantWarpFrameAt48k = 1024;

struct FormantWarpConfig {
  float factor = 1.0f;
  int lpc_order = 12;
  float amount = 1.0f;
  /// False keeps the frame at kFormantWarpFrameAt48k samples at every rate. True
  /// defines it in time instead: that many samples at 48 kHz, rescaled to the
  /// input rate and rounded to a multiple of four.
  bool frame_in_time = false;
};

/// @brief Factor the warp actually applies, after the range clamp and the
///        dry/wet fold.
/// @details Published so a caller that echoes the factor it was given reports the
///          one that was used. A NaN comes back a NaN, because std::clamp does not
///          launder one; the warp refuses it rather than resolving it.
float effective_formant_factor(float factor, float amount) noexcept;

class FormantWarp {
 public:
  explicit FormantWarp(FormantWarpConfig config = {});

  Audio process(const Audio& audio) const;
  const FormantWarpConfig& config() const noexcept { return config_; }

 private:
  FormantWarpConfig config_{};
};

}  // namespace sonare
