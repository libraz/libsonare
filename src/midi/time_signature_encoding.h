#pragma once

/// @file time_signature_encoding.h
/// @brief The power-of-two exponent a Standard MIDI File and a MIDI Clip File
///        store in place of a time-signature denominator (2 => 4, 3 => 8).
///
/// SMF accepts exponent 0 (a whole-note denominator); a MIDI Clip File reserves
/// 0 for a non-standard denominator, so it passes a minimum exponent of 1.

#include <cstdint>

namespace sonare::midi {

/// Highest exponent either format stores: a 128th-note denominator.
inline constexpr uint8_t kMaxTimeSignatureExponent = 7;

/// Denominator for a stored @p exponent, or 0 when it lies outside
/// [@p min_exponent, kMaxTimeSignatureExponent] and so cannot round-trip.
inline int decode_time_signature_denominator(uint8_t exponent, uint8_t min_exponent) noexcept {
  if (exponent < min_exponent || exponent > kMaxTimeSignatureExponent) return 0;
  return 1 << exponent;
}

/// Exponent storing @p denominator (non-positive reads as 4). A denominator that
/// is not a representable power of two rounds up, capped at
/// kMaxTimeSignatureExponent; @p exact reports whether the encoding was lossless.
inline uint8_t encode_time_signature_denominator(int denominator, uint8_t min_exponent,
                                                 bool* exact) noexcept {
  const int den = denominator > 0 ? denominator : 4;
  uint8_t exponent = min_exponent;
  while ((1 << exponent) < den && exponent < kMaxTimeSignatureExponent) ++exponent;
  if (exact != nullptr) *exact = (1 << exponent) == den;
  return exponent;
}

}  // namespace sonare::midi
