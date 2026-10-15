#pragma once

/// @file note_fx_shared.h
/// @brief Groove/swing helpers and note-retune logic shared by MidiFxChain,
/// MidiCapture and LayeredInstrument.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "midi/ump.h"

namespace sonare::midi::fx_shared {

/// Clamps to [0, 1]; non-finite maps to 0.
template <typename T>
T clamp01(T v) noexcept {
  if (!std::isfinite(v)) return T(0);
  if (v < T(0)) return T(0);
  if (v > T(1)) return T(1);
  return v;
}

/// Clamps a groove offset to [-1, 1]; non-finite maps to 0.
template <typename T>
T clamp_groove_offset(T v) noexcept {
  if (!std::isfinite(v)) return T(0);
  if (v < T(-1)) return T(-1);
  if (v > T(1)) return T(1);
  return v;
}

/// Groove step count bounded by the config's kMaxGrooveSteps.
template <typename Config>
size_t bounded_groove_steps(const Config& config) noexcept {
  return config.groove_steps > Config::kMaxGrooveSteps ? Config::kMaxGrooveSteps
                                                       : config.groove_steps;
}

/// Positive modulo of @p line into [0, steps); 0 when steps is 0.
inline size_t groove_index(int64_t line, size_t steps) noexcept {
  if (steps == 0) return 0;
  const int64_t s = static_cast<int64_t>(steps);
  int64_t idx = line % s;
  if (idx < 0) idx += s;
  return static_cast<size_t>(idx);
}

/// Patches the note address while preserving every other field of a channel
/// voice message.
inline Ump with_note_number(const Ump& src, uint8_t note) noexcept {
  Ump out = src;
  out.words[0] = (out.words[0] & ~(0x7Fu << 8u)) | (static_cast<uint32_t>(note & 0x7Fu) << 8u);
  return out;
}

/// Retunes @p src to @p note and shifts absolute per-note pitch (note-on
/// attribute 3, registered per-note controller 3) by @p delta semitones.
/// Returns false when the shifted pitch leaves its wire range.
inline bool retune_note_event(const Ump& src, uint8_t note, int64_t delta, Ump* out) noexcept {
  if (out == nullptr) return false;
  Ump copy = with_note_number(src, note);
  if (copy.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const uint8_t status = copy.status_nibble();
    if (status == static_cast<uint8_t>(UmpStatus::kNoteOn) && note_attribute_type(copy) == 3u) {
      const int64_t pitch =
          static_cast<int64_t>(note_attribute_data(src)) + delta * (int64_t{1} << 9);
      if (pitch < 0 || pitch > static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
        return false;
      }
      copy.words[1] = (copy.words[1] & ~uint32_t{0xFFFFu}) | static_cast<uint32_t>(pitch);
    } else if (status == static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController) &&
               (src.words[0] & 0xFFu) == 3u) {
      const int64_t pitch = static_cast<int64_t>(src.words[1]) + delta * (int64_t{1} << 25);
      if (pitch < 0 || pitch > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        return false;
      }
      copy.words[1] = static_cast<uint32_t>(pitch);
    }
  }
  *out = copy;
  return true;
}

}  // namespace sonare::midi::fx_shared
