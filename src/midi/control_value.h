#pragma once

/// @file control_value.h
/// @brief Channel-voice control values held at MIDI 2.0 width.
///
/// Every value keeps the raw UMP bits. MIDI 1.0 input is widened on receipt: structured
/// registered-controller data (index 0-31) by Zero-Extension (M2-115-U §4), everything else by
/// min-center-max (M2-115-U §3). The upscales are the ump.h scalers, so this header and the
/// UMP translator cannot disagree.
///
/// Integer consumers read u7()/u14() (right-shift truncation); float consumers read f7()/f14(),
/// the exact piecewise-linear inverse of the min-center-max upscale. For MIDI 1.0 input both
/// reproduce the original MIDI 1.0 value exactly, so a MIDI 1.0 render is bit-identical whichever
/// reading a consumer takes. Truncation also reads structured RPN data back exactly whether the
/// sender used Zero-Extension or min-center-max.

#include <algorithm>
#include <cstdint>

#include "midi/ump.h"

namespace sonare::midi {

namespace control_value_detail {

/// Inverse of a monotone upscale `up` on [0, max_value]: exact at every upscale point, linear
/// between neighbouring points.
template <typename Up>
inline double inverse_upscale(uint32_t raw, uint32_t shift, uint32_t max_value, Up up) noexcept {
  uint32_t v = raw >> shift;
  uint32_t lo = up(v);
  if (lo > raw) {
    --v;
    lo = up(v);
  }
  if (v >= max_value) return static_cast<double>(max_value);
  const uint32_t hi = up(v + 1);
  return static_cast<double>(v) + static_cast<double>(raw - lo) / static_cast<double>(hi - lo);
}

}  // namespace control_value_detail

/// Note velocity at MIDI 2.0 width (16 bits). The floor of 1 applies on read only.
struct Velocity16 {
  uint16_t raw;

  static Velocity16 from7(uint8_t v7) noexcept { return {scale_velocity_7_to_16(v7)}; }
  static constexpr Velocity16 from_raw(uint16_t r) noexcept { return {r}; }

  /// Inverse of f7(): clamps to [1, 127] and rounds to the nearest raw between upscale points.
  static Velocity16 from_f7(float f) noexcept {
    const double x = f > 1.0f ? (f < 127.0f ? static_cast<double>(f) : 127.0) : 1.0;
    const auto v = static_cast<uint32_t>(x);
    if (v >= 127u) return {scale_velocity_7_to_16(127)};
    const uint32_t lo = scale_velocity_7_to_16(static_cast<uint8_t>(v));
    const uint32_t hi = scale_velocity_7_to_16(static_cast<uint8_t>(v + 1));
    const double offset = (x - static_cast<double>(v)) * static_cast<double>(hi - lo);
    return {static_cast<uint16_t>(lo + static_cast<uint32_t>(offset + 0.5))};
  }

  constexpr uint8_t u7() const noexcept {
    const auto v = static_cast<uint8_t>(raw >> 9);
    return v > 0 ? v : uint8_t{1};
  }

  float f7() const noexcept {
    const double f = control_value_detail::inverse_upscale(raw, 9u, 127u, [](uint32_t v) {
      return static_cast<uint32_t>(scale_velocity_7_to_16(static_cast<uint8_t>(v)));
    });
    return f > 1.0 ? static_cast<float>(f) : 1.0f;
  }
};

/// Controller value at MIDI 2.0 width (32 bits).
struct Control32 {
  uint32_t raw;

  static Control32 from7(uint8_t v7) noexcept { return {scale_cc_7_to_32(v7)}; }
  /// Encodes a 0..127 controller value at MIDI 2.0 width. Integer values land
  /// on the protocol's upscale points; fractional values are rounded between
  /// the neighbouring points so a folded MPE value keeps its extra resolution.
  static Control32 from_f7(double f) noexcept {
    const double x = !(f > 0.0) ? 0.0 : (f >= 127.0 ? 127.0 : f);
    const auto v = static_cast<uint32_t>(x);
    if (v >= 127u) return {scale_cc_7_to_32(127)};
    const uint32_t lo = scale_cc_7_to_32(static_cast<uint8_t>(v));
    const uint32_t hi = scale_cc_7_to_32(static_cast<uint8_t>(v + 1u));
    const double offset = (x - static_cast<double>(v)) * static_cast<double>(hi - lo);
    return {static_cast<uint32_t>(lo + static_cast<uint32_t>(offset + 0.5))};
  }
  static Control32 from14_mcm(uint16_t v14) noexcept { return {scale_cc_14_to_32(v14)}; }
  static Control32 from14_zero_ext(uint16_t v14) noexcept {
    return {scale_rpn_14_to_32_zero_extend(v14)};
  }
  static constexpr Control32 from_raw(uint32_t r) noexcept { return {r}; }

  constexpr uint8_t u7() const noexcept { return static_cast<uint8_t>(raw >> 25); }
  constexpr uint16_t u14() const noexcept { return static_cast<uint16_t>(raw >> 18); }

  float f7() const noexcept { return static_cast<float>(f7_double()); }

  /// Full-width semantic value for controller arithmetic before a float DSP
  /// consumer reads it. A float cannot distinguish neighbouring 32-bit inputs.
  double f7_double() const noexcept {
    return control_value_detail::inverse_upscale(
        raw, 25u, 127u, [](uint32_t v) { return scale_cc_7_to_32(static_cast<uint8_t>(v)); });
  }

  /// Q7.25 fixed point in semitones. Double, because a float cannot hold all 32 raw bits.
  constexpr double q7_25() const noexcept {
    return static_cast<double>(raw) / static_cast<double>(uint32_t{1} << 25);
  }
};

/// Adds a relative controller's two's-complement delta to @p current, saturating at the ends of
/// the 32-bit range rather than wrapping.
inline Control32 add_saturating(Control32 current, Control32 delta) noexcept {
  const int64_t sum =
      static_cast<int64_t>(current.raw) + static_cast<int64_t>(static_cast<int32_t>(delta.raw));
  const int64_t clamped = std::min<int64_t>(std::max<int64_t>(sum, 0), int64_t{0xFFFFFFFF});
  return Control32::from_raw(static_cast<uint32_t>(clamped));
}

/// Pitch bend at MIDI 2.0 width (32 bits, center 0x80000000).
struct Bend32 {
  uint32_t raw;

  static Bend32 from14(uint16_t v14) noexcept { return {scale_bend_14_to_32(v14)}; }
  static constexpr Bend32 from_raw(uint32_t r) noexcept { return {r}; }
  static constexpr Bend32 center() noexcept { return {0x80000000u}; }

  constexpr uint16_t u14() const noexcept { return static_cast<uint16_t>(raw >> 18); }

  float f14() const noexcept {
    return static_cast<float>(control_value_detail::inverse_upscale(
        raw, 18u, 16383u,
        [](uint32_t v) { return scale_bend_14_to_32(static_cast<uint16_t>(v)); }));
  }
};

}  // namespace sonare::midi
