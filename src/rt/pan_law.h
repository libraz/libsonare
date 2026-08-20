#pragma once

/// @file pan_law.h
/// @brief The project's single stereo pan-law evaluator.
///
/// Every path that turns a pan position into a left/right gain pair goes through
/// compute_pan_gains(). It lives in rt/ rather than mixing/ because the mixer,
/// the mastering stereo processors and the clip player all need it, and
/// sonare_mixing links sonare_mastering — a mixing/ header would invert that
/// edge. The header is inline math with no link dependency, so a build with
/// BUILD_MIXING=OFF can use it too.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::rt {

/// @brief Curve mapping a pan position to a raw gain pair.
///
/// The centre attenuation in each name is the raw curve's own, before any
/// PanNormalization is applied.
enum class PanLaw {
  Const3dB,
  Const4p5dB,
  Const6dB,
  Linear0dB,
};

/// @brief Number of named laws, i.e. the exclusive bound of the wire encoding.
/// @details Callers that reject an out-of-range encoding instead of falling back
/// take their bound from here rather than repeating the literal.
inline constexpr int kPanLawCount = 4;

struct PanGains {
  float left = 1.0f;
  float right = 1.0f;
};

/// @brief Maps the wire/scene integer encoding of a law to the enum.
///
/// The integer form is what the mixer scene, the C ABI and the bindings carry;
/// it is the enum's declaration order. Anything out of range falls back to the
/// constant-power default rather than producing an unnamed law.
inline PanLaw pan_law_from_index(int index) noexcept {
  switch (index) {
    case 1:
      return PanLaw::Const4p5dB;
    case 2:
      return PanLaw::Const6dB;
    case 3:
      return PanLaw::Linear0dB;
    default:
      return PanLaw::Const3dB;
  }
}

/// @brief How the raw curve is rescaled before it reaches the signal.
///
/// A law defines the *shape* of the pair; the reference point is a separate
/// choice, and mixing the two up is what makes the same pan value sound
/// different on different routing paths.
enum class PanNormalization {
  /// The law's literal gains. Const3dB conserves energy here: left^2 + right^2
  /// == 1 across the whole pan range.
  Raw,
  /// Scaled so a centred pan is unity on both channels, which keeps the total
  /// stereo energy of a centred signal intact and makes a zero-depth modulator a
  /// true bypass. Const3dB stays energy-conserving under this scale
  /// (left^2 + right^2 == 2, one unity channel each side).
  CenterUnity,
  /// The channel toward the pan direction is pinned at unity and only the away
  /// channel is pulled down, by the ratio the law dictates. This is the balance
  /// control's reference: it leaves an existing stereo image alone and is unity
  /// at centre for every law.
  NearUnity,
};

namespace detail {

/// Evaluates the law's own curve, before normalization.
inline PanGains raw_pan_gains(float pan, PanLaw law) noexcept {
  const float p = std::clamp(pan, -1.0f, 1.0f);
  const float t = (p + 1.0f) * 0.5f;
  const float linear_left = 1.0f - t;
  const float linear_right = t;

  switch (law) {
    case PanLaw::Const3dB: {
      const float angle = t * ::sonare::constants::kHalfPi;
      return {std::cos(angle), std::sin(angle)};
    }
    case PanLaw::Const4p5dB: {
      const float angle = t * ::sonare::constants::kHalfPi;
      const float constant_left = std::cos(angle);
      const float constant_right = std::sin(angle);
      return {std::sqrt(linear_left * constant_left), std::sqrt(linear_right * constant_right)};
    }
    case PanLaw::Const6dB:
      return {linear_left, linear_right};
    case PanLaw::Linear0dB:
      return {p <= 0.0f ? 1.0f : 1.0f - p, p >= 0.0f ? 1.0f : 1.0f + p};
  }

  return {1.0f, 1.0f};
}

/// Reciprocal of the law's centred gain. The closed forms are spelled out rather
/// than derived from raw_pan_gains(0) so the scale is exact for the common laws.
inline float center_unity_scale(PanLaw law) noexcept {
  switch (law) {
    case PanLaw::Const3dB:
      // cos(pi/4) == sin(pi/4) == 1/sqrt(2), so the scale is exactly sqrt(2).
      return ::sonare::constants::kSqrt2;
    case PanLaw::Const6dB:
      // The linear curve is 0.5 on each channel at centre.
      return 2.0f;
    case PanLaw::Linear0dB:
      // Already unity at centre: neither channel is attenuated until pan leaves 0.
      return 1.0f;
    case PanLaw::Const4p5dB:
      break;
  }
  const PanGains center = raw_pan_gains(0.0f, law);
  return center.left > 0.0f ? 1.0f / center.left : 1.0f;
}

}  // namespace detail

/// @brief Rescales an already-evaluated gain pair.
///
/// Split out of compute_pan_gains() for the smoothed paths: they interpolate the
/// raw pair per sample and can only normalize the interpolated result.
/// @param gains Raw pair produced by @p law.
/// @param law Law that produced @p gains; needed for the centre reference.
inline PanGains normalize_pan_gains(PanGains gains, PanLaw law,
                                    PanNormalization normalization) noexcept {
  switch (normalization) {
    case PanNormalization::Raw:
      return gains;
    case PanNormalization::CenterUnity: {
      const float scale = detail::center_unity_scale(law);
      return {gains.left * scale, gains.right * scale};
    }
    case PanNormalization::NearUnity: {
      const float norm = std::max(gains.left, gains.right);
      const float inv_norm = norm > 0.0f ? 1.0f / norm : 0.0f;
      return {gains.left * inv_norm, gains.right * inv_norm};
    }
  }
  return gains;
}

/// @brief Derives the left/right gain pair for a pan position.
/// @param pan Pan position, clamped to [-1, +1]; 0 is centre, +1 hard right.
inline PanGains compute_pan_gains(float pan, PanLaw law = PanLaw::Const3dB,
                                  PanNormalization normalization = PanNormalization::Raw) noexcept {
  return normalize_pan_gains(detail::raw_pan_gains(pan, law), law, normalization);
}

}  // namespace sonare::rt
