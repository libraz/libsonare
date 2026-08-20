#pragma once

/// @file ump.h
/// @brief Fixed-length, trivially-copyable Universal MIDI Packet (UMP) POD plus
///        constructors / adapters for MIDI 1.0 and MIDI 2.0 channel-voice
///        messages.
///
/// The internal MIDI representation is the Universal MIDI Packet (UMP). A single
/// @ref Ump is a fixed array of up to four 32-bit words (the 32/64/96/128-bit
/// UMP word forms) plus a small length/type tag. Because it is a POD with no
/// heap state, an Ump may ride RT structures (the realtime event queue and the
/// fixed-capacity active-note table) without any allocation.
///
/// Supported message families:
///   - MIDI 1.0 channel voice (Message Type 0x2): 7-bit note/velocity/CC.
///   - MIDI 2.0 channel voice (Message Type 0x4): 16-bit velocity, 32-bit CC,
///     per-note controllers, registered/assignable controllers.
///   - Program change / bank select (both protocols).
///   - A SysEx HANDLE: variable-length SysEx / property payloads are NEVER
///     carried inline. The Ump only stores a control-side handle id; the bytes
///     live in a control-thread @ref SysExStore (see below). This keeps the RT
///     path free of variable-length data.
///
/// Lossy MIDI 1.0 <-> MIDI 2.0 translation (DOCUMENTED, pinned in the test):
///   - Velocity: MIDI 1.0 uses 7-bit velocity; MIDI 2.0 uses 16-bit. The
///     1.0 -> 2.0 up-scale repeats the source bit pattern to fill the target
///     width. The 2.0 -> 1.0 down-scale takes the top 7
///     bits (>> 9). A round-trip 1.0 -> 2.0 -> 1.0 is LOSSLESS for velocity (the
///     top 7 bits survive), but a 2.0 velocity whose low 9 bits are non-zero is
///     LOSSY through 1.0.
///   - Control change value: MIDI 1.0 is 7-bit, MIDI 2.0 is 32-bit. 1.0 -> 2.0
///     left-justifies the 7-bit value into 32 bits (replicated); 2.0 -> 1.0
///     takes the top 7 bits (>> 25). Same lossless/lossy rule as velocity.
///   - Note number, channel, group, CC index, program, bank: identical 7-bit /
///     4-bit fields in both protocols, so they round-trip LOSSLESLY.
///   - Per-note controllers and registered/assignable controllers exist ONLY in
///     MIDI 2.0; converting them to MIDI 1.0 DROPS them (no 1.0 equivalent).
///   - Note-on attribute / per-note pitch (MIDI 2.0) is DROPPED on down-convert.

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sonare::midi {

/// UMP message type nibble (the high nibble of word[0], bits 28..31). Only the
/// subset needed by the MIDI core is enumerated; values match the UMP specification.
enum class UmpMessageType : uint8_t {
  kUtility = 0x0,
  kSystem = 0x1,
  kMidi1ChannelVoice = 0x2,
  kData64 = 0x3,  // 64-bit data (SysEx7) — represented via handle, not inline.
  kMidi2ChannelVoice = 0x4,
  kData128 = 0x5,  // 128-bit data (SysEx8/Mixed) — handle only.
};

/// MIDI channel-voice status nibbles (shared opcode space for 1.0 and 2.0).
enum class UmpStatus : uint8_t {
  kNoteOff = 0x8,
  kNoteOn = 0x9,
  kPolyPressure = 0xA,
  kControlChange = 0xB,
  kProgramChange = 0xC,
  kChannelPressure = 0xD,
  kPitchBend = 0xE,
  // MIDI 2.0-only registered per-note controller (status 0x0) and assignable
  // per-note/controller forms live in the 2.0 message type.
  kRegisteredPerNoteController = 0x0,
  kAssignablePerNoteController = 0x1,
  kRegisteredController = 0x2,
  kAssignableController = 0x3,
};

/// Handle id for an out-of-band SysEx / property payload. 0 means "no SysEx".
using SysExHandle = uint32_t;

/// Number of active 32-bit words a UMP of message type @p message_type occupies.
/// This is the whole 16-entry mapping the UMP specification fixes for the
/// message-type nibble, including the reserved types whose word form is already
/// assigned; only the low nibble of @p message_type is read. Every reader that
/// walks a UMP stream and every writer that emits one must size messages from
/// here, so a stream written by one and read by another cannot disagree.
constexpr uint8_t ump_word_count_for_message_type(uint8_t message_type) noexcept {
  switch (message_type & 0x0Fu) {
    case 0x0:  // Utility
    case 0x1:  // System real time / common
    case 0x2:  // MIDI 1.0 channel voice
    case 0x6:  // reserved, 32-bit
    case 0x7:  // reserved, 32-bit
      return 1;
    case 0x3:  // Data (SysEx7), 64-bit
    case 0x4:  // MIDI 2.0 channel voice
    case 0x8:  // reserved, 64-bit
    case 0x9:  // reserved, 64-bit
    case 0xA:  // reserved, 64-bit
      return 2;
    case 0xB:  // reserved, 96-bit
    case 0xC:  // reserved, 96-bit
      return 3;
    default:  // 0x5 (128-bit data), 0xD / 0xE / 0xF reserved 128-bit
      return 4;
  }
}

/// Active word count implied by a message's first word. Returns a value in
/// [1, 4] for every possible input, so a caller can also use it to bound an
/// index into Ump::words.
constexpr uint8_t ump_word_count_for_word0(uint32_t word0) noexcept {
  return ump_word_count_for_message_type(static_cast<uint8_t>((word0 >> 28) & 0x0Fu));
}

/// Fixed-length, trivially-copyable Universal MIDI Packet.
///
/// `words` holds 1..4 active 32-bit words (the rest are zero); `word_count`
/// records how many are active for the message form. `group` is the UMP group
/// (0..15). `sysex_handle` is non-zero only for messages that reference an
/// out-of-band SysEx payload; the payload bytes are NEVER stored here.
struct Ump {
  uint32_t words[4] = {0, 0, 0, 0};
  uint8_t word_count = 0;
  uint8_t group = 0;
  /// Control-side SysEx payload handle (0 = none). RT code only ever copies it.
  SysExHandle sysex_handle = 0;

  UmpMessageType message_type() const noexcept {
    return static_cast<UmpMessageType>((words[0] >> 28) & 0x0Fu);
  }
  uint8_t status_nibble() const noexcept { return static_cast<uint8_t>((words[0] >> 20) & 0x0Fu); }
  uint8_t channel() const noexcept { return static_cast<uint8_t>((words[0] >> 16) & 0x0Fu); }
  uint8_t data2_7bit() const noexcept { return static_cast<uint8_t>(words[0] & 0x7Fu); }

  bool is_note_on() const noexcept {
    if (status_nibble() != static_cast<uint8_t>(UmpStatus::kNoteOn)) return false;
    // MIDI 1.0 convention: note-on with velocity 0 is semantically note-off.
    // MIDI 2.0 does not use that convention, so only apply it to MT=2 packets.
    return message_type() != UmpMessageType::kMidi1ChannelVoice || data2_7bit() != 0;
  }
  bool is_note_off() const noexcept {
    return status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOff) ||
           (message_type() == UmpMessageType::kMidi1ChannelVoice &&
            status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOn) && data2_7bit() == 0);
  }
  /// Note number for note-on/off/poly-pressure messages (both protocols store
  /// it in word[0] bits 8..14).
  uint8_t note_number() const noexcept { return static_cast<uint8_t>((words[0] >> 8) & 0x7Fu); }

  bool operator==(const Ump& o) const noexcept {
    return words[0] == o.words[0] && words[1] == o.words[1] && words[2] == o.words[2] &&
           words[3] == o.words[3] && word_count == o.word_count && group == o.group &&
           sysex_handle == o.sysex_handle;
  }
  bool operator!=(const Ump& o) const noexcept { return !(*this == o); }
};

static_assert(sizeof(Ump) <= 32, "Ump must stay small enough to ride RT structures");

// ===========================================================================
// MIDI 1.0 channel-voice constructors (Message Type 0x2, single 32-bit word)
// ===========================================================================

/// MIDI 1.0 note-on. `velocity7` is 7-bit (0..127). A velocity of 0 is, per the
/// MIDI 1.0 convention, equivalent to a note-off; callers that want a true
/// note-off should use @ref make_midi1_note_off.
Ump make_midi1_note_on(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity7) noexcept;
Ump make_midi1_note_off(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity7) noexcept;
Ump make_midi1_poly_pressure(uint8_t group, uint8_t channel, uint8_t note,
                             uint8_t pressure7) noexcept;
Ump make_midi1_control_change(uint8_t group, uint8_t channel, uint8_t controller,
                              uint8_t value7) noexcept;
Ump make_midi1_program_change(uint8_t group, uint8_t channel, uint8_t program) noexcept;
Ump make_midi1_channel_pressure(uint8_t group, uint8_t channel, uint8_t pressure7) noexcept;
/// MIDI 1.0 pitch bend. `bend14` is unsigned 14-bit, center = 8192.
Ump make_midi1_pitch_bend(uint8_t group, uint8_t channel, uint16_t bend14) noexcept;

// ===========================================================================
// MIDI 2.0 channel-voice constructors (Message Type 0x4, two 32-bit words)
// ===========================================================================

/// MIDI 2.0 note-on. `velocity16` is the full 16-bit velocity; `attribute_type`
/// / `attribute_data` carry the optional note attribute (0 = none).
Ump make_midi2_note_on(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16,
                       uint8_t attribute_type = 0, uint16_t attribute_data = 0) noexcept;
Ump make_midi2_note_off(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16) noexcept;
Ump make_midi2_poly_pressure(uint8_t group, uint8_t channel, uint8_t note,
                             uint32_t pressure32) noexcept;
Ump make_midi2_control_change(uint8_t group, uint8_t channel, uint8_t controller,
                              uint32_t value32) noexcept;
Ump make_midi2_program_change(uint8_t group, uint8_t channel, uint8_t program, uint8_t bank_msb,
                              uint8_t bank_lsb, bool bank_valid) noexcept;
Ump make_midi2_channel_pressure(uint8_t group, uint8_t channel, uint32_t pressure32) noexcept;
Ump make_midi2_pitch_bend(uint8_t group, uint8_t channel, uint32_t bend32) noexcept;
/// MIDI 2.0 registered per-note controller (status 0x0). `note` selects the
/// note, `index` the controller, `value32` the 32-bit value.
Ump make_midi2_per_note_controller(uint8_t group, uint8_t channel, uint8_t note, uint8_t index,
                                   uint32_t value32) noexcept;
Ump make_midi2_assignable_per_note_controller(uint8_t group, uint8_t channel, uint8_t note,
                                              uint8_t index, uint32_t value32) noexcept;
/// MIDI 2.0 registered / assignable channel controllers (statuses 0x2 / 0x3).
Ump make_midi2_registered_controller(uint8_t group, uint8_t channel, uint8_t bank, uint8_t index,
                                     uint32_t value32) noexcept;
Ump make_midi2_assignable_controller(uint8_t group, uint8_t channel, uint8_t bank, uint8_t index,
                                     uint32_t value32) noexcept;

// ===========================================================================
// SysEx handle constructor
// ===========================================================================

/// Builds a data message that references an out-of-band SysEx payload by handle.
/// No payload bytes are stored in the Ump. `byte_count` records the payload
/// length for diagnostics only.
Ump make_sysex_handle(uint8_t group, SysExHandle handle) noexcept;

/// Stateful, allocation-free SysEx7 packetizer. Unlike the one-shot helper
/// below, read() may be called repeatedly with a bounded scratch buffer, so
/// control-thread backends can stream arbitrarily long payloads across
/// multiple OS packet lists without truncation.
class SysEx7Packetizer {
 public:
  SysEx7Packetizer(const uint8_t* data, size_t size, uint8_t group) noexcept;

  bool valid() const noexcept { return valid_; }
  size_t packet_count() const noexcept { return packet_count_; }
  size_t packet_position() const noexcept { return packet_position_; }
  size_t remaining_packets() const noexcept { return packet_count_ - packet_position_; }

  /// Writes the next at most @p cap packets and advances the cursor. Returns 0
  /// for invalid/exhausted input or a null/zero-capacity destination.
  size_t read(Ump* out, size_t cap) noexcept;

  /// Positions the cursor for retry/resume. Returns false when out of range.
  bool seek_packet(size_t packet_position) noexcept;

 private:
  const uint8_t* data_ = nullptr;
  size_t begin_ = 0;
  size_t end_ = 0;
  size_t packet_count_ = 0;
  size_t packet_position_ = 0;
  uint8_t group_ = 0;
  bool valid_ = false;
};

/// Packetizes a resolved SysEx payload into a sequence of SysEx7 (message type
/// 0x3, 64-bit) UMP data messages on `group`. A leading 0xF0 / trailing 0xF7
/// MIDI 1.0 frame is stripped (UMP SysEx7 carries only the inner data). Each UMP
/// holds up to 6 payload bytes; the status nibble runs Complete for a single
/// packet, otherwise Start / Continue… / End. Writes up to `cap` UMPs into `out`
/// and returns the count produced, or 0 if the payload is empty, contains an
/// 8-bit byte (SysEx7 is 7-bit; use SysEx8), or does not fit in `cap`. RT-safe:
/// no allocation. This is the inverse of the SysEx7 reader used by the SMF2
/// importer, and lets a live output backend resolve a SysExHandle to wire bytes.
size_t sysex7_payload_to_umps(const uint8_t* data, size_t size, uint8_t group, Ump* out,
                              size_t cap) noexcept;

/// Control-thread payload store for variable-length SysEx / property data.
/// UMPs carry only a SysExHandle so RT structures stay fixed-size; callers keep
/// the payload bytes here and pass handles through the MIDI graph.
class SysExStore {
 public:
  /// Stores `size` bytes and returns a non-zero handle, or 0 on invalid input /
  /// allocation failure. The bytes are copied and remain stable until remove()
  /// or clear().
  SysExHandle add(const uint8_t* data, size_t size);
  SysExHandle add(const std::vector<uint8_t>& data) { return add(data.data(), data.size()); }

  /// Stores `size` bytes under the explicit, non-zero `handle`, replacing any
  /// existing payload for it. Returns false for a zero handle or a null/empty
  /// buffer. Lets a producer that must assign a handle before the payload reaches
  /// the store (e.g. a realtime MIDI reader staging completed SysEx for a control
  /// thread) commit the bytes later under the pre-assigned handle.
  bool add_with_handle(SysExHandle handle, const uint8_t* data, size_t size) {
    if (handle == 0 || data == nullptr || size == 0) {
      return false;
    }
    payloads_[handle] = std::vector<uint8_t>(data, data + size);
    return true;
  }

  /// Returns the payload for `handle`, or nullptr if unknown / zero.
  const std::vector<uint8_t>* lookup(SysExHandle handle) const noexcept;

  /// Removes a payload. Returns true only when an existing non-zero handle was
  /// removed.
  bool remove(SysExHandle handle) noexcept;
  void clear() noexcept;
  size_t size() const noexcept { return payloads_.size(); }
  bool contains(SysExHandle handle) const noexcept { return lookup(handle) != nullptr; }

 private:
  SysExHandle allocate_handle() noexcept;

  SysExHandle next_handle_ = 1;
  std::unordered_map<SysExHandle, std::vector<uint8_t>> payloads_;
};

// ===========================================================================
// MIDI 1.0 byte-stream <-> UMP adapter
// ===========================================================================

/// Parses ONE MIDI 1.0 channel-voice message from a byte buffer into a UMP.
/// Returns the number of bytes consumed (0 on failure / incomplete). System
/// real-time and SysEx are not handled here (SysEx is a control-side handle).
/// `running_status` carries the last status byte for running-status streams; it
/// is updated in place. On success `*out` holds the MIDI-1.0-typed UMP.
size_t midi1_bytes_to_ump(const uint8_t* bytes, size_t len, uint8_t group, uint8_t* running_status,
                          Ump* out) noexcept;

/// Serializes a MIDI-1.0-typed UMP back to channel-voice bytes. Returns the byte
/// count written (0 if `ump` is not a MIDI 1.0 channel-voice message or `cap` is
/// too small). At most 3 bytes are produced.
size_t ump_to_midi1_bytes(const Ump& ump, uint8_t* out, size_t cap) noexcept;

// ===========================================================================
// MIDI 1.0 <-> MIDI 2.0 channel-voice conversion (see lossy notes in header)
// ===========================================================================

/// Up-converts a MIDI 1.0 channel-voice UMP to MIDI 2.0. Velocity/CC values are
/// bit-scaled up (lossless top bits). Returns the original unchanged if `ump`
/// is not a MIDI 1.0 channel-voice message.
Ump midi1_to_midi2(const Ump& ump) noexcept;

/// Down-converts a MIDI 2.0 channel-voice UMP to MIDI 1.0. Velocity/CC are
/// down-scaled to 7 bits (LOSSY low bits). Per-note / registered controllers
/// have no MIDI 1.0 equivalent: those return an Ump with `word_count == 0`
/// (caller should drop them). Returns the original unchanged if `ump` is not a
/// MIDI 2.0 channel-voice message.
Ump midi2_to_midi1(const Ump& ump) noexcept;

/// A MIDI 2.0 -> MIDI 1.0 conversion result that can carry multi-message
/// lowerings such as bank-select CC#0/CC#32 plus program-change.
struct Midi1MessageList {
  std::array<Ump, 3> messages{};
  uint8_t count = 0;
};

/// Down-converts one UMP into zero or more MIDI 1.0 channel-voice UMPs. Most
/// channel-voice messages produce one output; a MIDI 2.0 Program Change with
/// the bank-valid flag set produces CC#0, CC#32, and Program Change at the same
/// timestamp. MIDI 2.0-only controller forms produce count == 0.
Midi1MessageList midi2_to_midi1_messages(const Ump& ump) noexcept;

/// 7-bit -> 16-bit velocity up-scale (bit-replication scaling: repeats the
/// source bits to fill the destination width). Distinct from pitch bend's
/// true min-center-max scaling (see scale_bend_14_to_32), which velocity does
/// not need since it has no "unbent" center value to preserve.
uint16_t scale_velocity_7_to_16(uint8_t velocity7) noexcept;
/// 16-bit -> 7-bit velocity down-scale (top 7 bits). LOSSY.
uint8_t scale_velocity_16_to_7(uint16_t velocity16) noexcept;
/// 16-bit -> 7-bit down-scale for a NOTE-ON velocity. As
/// scale_velocity_16_to_7, except that a nonzero MIDI 2.0 velocity never
/// down-scales to 0: velocity 0 on a MIDI 1.0 note-on means note-off, so the
/// quietest audible note-on clamps to 1 instead of being silently dropped.
uint8_t scale_note_on_velocity_16_to_7(uint16_t velocity16) noexcept;
/// 7-bit -> 32-bit CC up-scale.
uint32_t scale_cc_7_to_32(uint8_t value7) noexcept;
/// 32-bit -> 7-bit CC down-scale (top 7 bits). LOSSY.
uint8_t scale_cc_32_to_7(uint32_t value32) noexcept;
/// 14-bit pitch bend -> 32-bit MIDI 2.0 value.
uint32_t scale_bend_14_to_32(uint16_t bend14) noexcept;
/// 32-bit MIDI 2.0 pitch bend -> 14-bit MIDI 1.0 value. LOSSY.
uint16_t scale_bend_32_to_14(uint32_t bend32) noexcept;

}  // namespace sonare::midi
