#pragma once

/// @file sysex_framing.h
/// @brief The one rule for SysEx framing bytes shared by every MIDI file format.
///
/// A stored SysEx payload (a @ref SysExStore entry) is `[F0] body [F7]`: the MIDI 1.0
/// start and end bytes are optional framing, and the body is everything between them.
/// A body that itself begins with F0 or ends with F7 — possible only for full-width
/// SysEx8 data or a malformed MIDI 1.0 message — is stored with explicit framing on that
/// side, so the boundary byte stays data. @ref sysex_body inverts
/// @ref sysex_payload_from_body for every body, which is what lets an exporter recover
/// the exact bytes an importer read without guessing from byte values.
///
/// @ref SmfSysExAssembler owns the SMF continuation rule: an F0 event starts a message,
/// an F7 event continues the unfinished one or is an independent escape, an empty
/// escape is a no-op, and only a transmitted MIDI message (never a meta event) abandons
/// an unfinished message.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::midi {

/// True for the MIDI 1.0 SysEx start byte (also the SMF F0 event status).
constexpr bool is_sysex_start_byte(uint8_t byte) noexcept { return byte == 0xF0u; }

/// True for the MIDI 1.0 SysEx end byte (also the SMF F7 event status).
constexpr bool is_sysex_end_byte(uint8_t byte) noexcept { return byte == 0xF7u; }

/// The body of a stored SysEx payload: a view into the payload's bytes.
struct SysExBody {
  const uint8_t* data = nullptr;
  size_t size = 0;

  /// True when the body is non-empty and every byte is a 7-bit data byte, so it can be
  /// carried by MIDI 1.0 SysEx and UMP SysEx7.
  bool is_7bit() const noexcept {
    if (size == 0) return false;
    for (size_t i = 0; i < size; ++i) {
      if (data[i] > 0x7Fu) return false;
    }
    return true;
  }
};

/// Returns the body of a stored payload: one leading F0 and one trailing F7 are framing.
inline SysExBody sysex_body(const uint8_t* payload, size_t size) noexcept {
  if (payload == nullptr || size == 0) return {};
  size_t begin = 0;
  size_t end = size;
  if (is_sysex_start_byte(payload[begin])) ++begin;
  if (end > begin && is_sysex_end_byte(payload[end - 1])) --end;
  return {payload + begin, end - begin};
}

inline SysExBody sysex_body(const std::vector<uint8_t>& payload) noexcept {
  return sysex_body(payload.data(), payload.size());
}

/// Returns the stored form of @p body. @p terminated appends the F7 end byte; a body
/// whose first byte is F0 or last byte is F7 gains framing on that side so
/// @ref sysex_body returns it unchanged.
std::vector<uint8_t> sysex_payload_from_body(const uint8_t* body, size_t size, bool terminated);

/// Assembles one SMF track's F0 / F7 SysEx events into stored payloads.
class SmfSysExAssembler {
 public:
  /// What one fed event produced.
  struct Outcome {
    bool completed = false;  ///< A message finished; read it with take().
    uint32_t abandoned = 0;  ///< Unfinished messages this event displaced.
  };

  /// True when @p status is an SMF SysEx event status (F0 or F7).
  static bool is_sysex_status(uint8_t status) noexcept {
    return is_sysex_start_byte(status) || is_sysex_end_byte(status);
  }

  /// Feeds an F0 or F7 event carrying @p size bytes, timed at @p ppq.
  Outcome feed(uint8_t status, const uint8_t* bytes, size_t size, double ppq);

  /// A transmitted MIDI message arrived, which an unfinished message cannot continue
  /// past. Returns the number of unfinished messages abandoned (0 or 1).
  uint32_t interrupt() noexcept;

  /// True while a message started by an F0 event awaits its F7 continuation.
  bool pending() const noexcept { return pending_; }

  /// Returns the completed message's stored payload and time, and resets for the next.
  std::vector<uint8_t> take(double* ppq);

 private:
  std::vector<uint8_t> bytes_;
  double ppq_ = 0.0;
  bool pending_ = false;
  bool completed_ = false;
};

}  // namespace sonare::midi
