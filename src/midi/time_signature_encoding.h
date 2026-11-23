#pragma once

/// @file time_signature_encoding.h
/// @brief The power-of-two exponent a Standard MIDI File and a MIDI Clip File
///        store in place of a time-signature denominator (2 => 4, 3 => 8), and the
///        signature lists both writers export and both readers store.
///
/// SMF accepts exponent 0 (a whole-note denominator); a MIDI Clip File reserves
/// 0 for a non-standard denominator, so it passes a minimum exponent of 1.

#include <cstdint>
#include <optional>
#include <vector>

#include "transport/tempo_map.h"

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

/// The signatures a writer exports for @p time_signatures (sorted by start): the first one
/// governs from ppq 0 in the model, so when it starts later it is also written at 0, or a
/// reader would take the file's default 4/4 until it.
inline std::vector<transport::TimeSignatureSegment> exported_time_signatures(
    const std::vector<transport::TimeSignatureSegment>& time_signatures) {
  std::vector<transport::TimeSignatureSegment> out = time_signatures;
  if (!out.empty() && out.front().start_ppq > 0.0) {
    transport::TimeSignatureSegment opening = out.front();
    opening.start_ppq = 0.0;
    out.insert(out.begin(), opening);
  }
  return out;
}

/// Stores the signatures a reader found (sorted by start). An absent signature is the
/// format's default, so a late first one gets @p format_default at 0 when the format states
/// one. An entry at 0 identical to the next is then dropped in favour of the later one, which
/// carries the bar line; the model reads the first signature from 0, so the map is unchanged.
inline void store_imported_time_signatures(
    std::vector<transport::TimeSignatureSegment>* time_signatures,
    std::optional<transport::TimeSignatureSegment> format_default) {
  if (format_default && !time_signatures->empty() && time_signatures->front().start_ppq > 0.0) {
    format_default->start_ppq = 0.0;
    time_signatures->insert(time_signatures->begin(), *format_default);
  }
  if (time_signatures->size() < 2) return;
  const transport::TimeSignatureSegment& first = (*time_signatures)[0];
  const transport::TimeSignatureSegment& next = (*time_signatures)[1];
  if (first.start_ppq == 0.0 && first.time_sig.numerator == next.time_sig.numerator &&
      first.time_sig.denominator == next.time_sig.denominator &&
      first.clocks_per_metronome_click == next.clocks_per_metronome_click &&
      first.thirty_seconds_per_quarter == next.thirty_seconds_per_quarter) {
    time_signatures->erase(time_signatures->begin());
  }
}

}  // namespace sonare::midi
