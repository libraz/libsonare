#pragma once

/// @file tick_conversion.h
/// @brief Shared, domain-qualified PPQ<->tick conversions.
///
/// Two distinct tick domains exist and were historically expressed as
/// identically-named (`ppq_to_ticks` / `ticks_to_ppq`) file-local helpers in
/// clock_sync.cpp (fixed 24-PPQN MIDI clock) and smf.cpp (variable SMF PPQN).
/// Living in anonymous namespaces they never collided, but the same-name /
/// different-meaning pairing invited confusion. They are consolidated here with
/// domain-qualified names; the arithmetic is byte-for-byte the original.

#include <cmath>
#include <cstdint>

#include "midi/clock_sync.h"  // kClockPulsesPerQuarter

namespace sonare::midi {

/// @brief PPQ (quarter notes) -> MIDI-clock ticks at 24 PPQN. One quarter note
///        == @ref kClockPulsesPerQuarter ticks.
inline double clock_ppq_to_ticks(double ppq) noexcept { return ppq * kClockPulsesPerQuarter; }

/// @brief MIDI-clock ticks at 24 PPQN -> PPQ (quarter notes).
inline double clock_ticks_to_ppq(int64_t ticks) noexcept {
  return static_cast<double>(ticks) / kClockPulsesPerQuarter;
}

/// @brief SMF ticks at `ppqn` resolution -> PPQ (quarter-note units, matching
///        MidiClipEvent::ppq). Returns 0 for a degenerate `ppqn` of 0.
inline double smf_ticks_to_ppq(uint32_t ticks, uint16_t ppqn) noexcept {
  if (ppqn == 0) return 0.0;
  return static_cast<double>(ticks) / static_cast<double>(ppqn);
}

/// @brief PPQ (quarter notes) -> SMF ticks at `ppqn` (rounded to nearest,
///        non-negative; 0 for non-finite or non-positive input).
inline int64_t smf_ppq_to_ticks(double ppq, uint16_t ppqn) noexcept {
  const double t = ppq * static_cast<double>(ppqn);
  if (!std::isfinite(t) || t <= 0.0) return 0;
  return static_cast<int64_t>(std::llround(t));
}

}  // namespace sonare::midi
