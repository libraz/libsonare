#pragma once

/// @file clock_sync.h
/// @brief MIDI clock (24 PPQN), MTC quarter-frame, and Song Position Pointer
///        (SPP) generation and parsing.
///
/// Layering: depends ONLY on transport/tempo_map (for tempo/position) + std. It
/// does NOT depend on engine/ or arrangement/.
///
/// System real-time / common messages are NOT channel-voice, so they are NOT
/// carried as UMP here; they are emitted as raw MIDI 1.0 status bytes into a
/// fixed-capacity output buffer (the byte stream a hardware/virtual MIDI port
/// expects). This keeps the representation faithful to what a sync slave reads.
///
/// Threading / RT contract
/// -----------------------
///  - AUDIO thread: generate_clock_block() emits the 0xF8 clock bytes whose tick
///    positions fall in a render-frame block, into a FIXED-capacity output. ZERO
///    heap allocation, no lock, no I/O. Overflow drops surplus ticks and bumps
///    an atomic telemetry counter. parse_byte() is a tiny state machine over one
///    incoming byte and is also alloc-0, so an audio-thread MIDI-in handler may
///    feed it directly (documented audio-safe).
///  - CONTROL thread: SPP and MTC full-frame helpers (start-of-transport
///    position broadcast) are typically issued on the control thread but are
///    themselves alloc-0 POD encoders, so they are safe anywhere.
///
/// Determinism: tick positions derive purely from the TempoMap (PPQ<->frame)
/// and integer tick math. No clock / random.

#include <array>
#include <cstddef>
#include <cstdint>

#include "rt/overflow_counter.h"
#include "transport/tempo_map.h"

namespace sonare::midi {

/// MIDI system real-time / common status bytes used for sync.
inline constexpr uint8_t kStatusMtcQuarterFrame = 0xF1u;
inline constexpr uint8_t kStatusSongPosition = 0xF2u;
inline constexpr uint8_t kStatusClock = 0xF8u;
inline constexpr uint8_t kStatusStart = 0xFAu;
inline constexpr uint8_t kStatusContinue = 0xFBu;
inline constexpr uint8_t kStatusStop = 0xFCu;

/// MIDI beat clock resolution: 24 pulses per quarter note.
inline constexpr int kClockPulsesPerQuarter = 24;
/// A MIDI "beat" (SPP unit) is a sixteenth note = 6 clock pulses.
inline constexpr int kClocksPerSppBeat = 6;

/// SMPTE frame-rate code carried in MTC quarter-frame / full-frame messages.
enum class MtcFrameRate : uint8_t {
  kFps24 = 0,
  kFps25 = 1,
  kFps29_97Drop = 2,
  kFps30 = 3,
};

/// Decoded SMPTE timecode used by MTC.
struct MtcTime {
  uint8_t hours = 0;    // 0..23
  uint8_t minutes = 0;  // 0..59
  uint8_t seconds = 0;  // 0..59
  uint8_t frames = 0;   // 0..(rate-1)
  MtcFrameRate rate = MtcFrameRate::kFps25;

  bool operator==(const MtcTime& o) const noexcept {
    return hours == o.hours && minutes == o.minutes && seconds == o.seconds && frames == o.frames &&
           rate == o.rate;
  }
};

/// Fixed-capacity byte output for one block of generated sync bytes.
struct ClockByteOutput {
  static constexpr size_t kCapacity = 256;
  std::array<uint8_t, kCapacity> bytes{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept {
    size = 0;
    overflowed = false;
  }
};

/// Number of frames-per-second for a given MTC rate (29.97-drop reports 30 for
/// integer frame counting; drop-frame numbering is a display concern handled by
/// callers).
int mtc_fps(MtcFrameRate rate) noexcept;

/// Encode an SPP (0xF2) message for `midi_beats` (sixteenth notes, 14-bit) into
/// the 3 bytes [F2, lsb, msb]. Returns bytes written (3) or 0 if `cap` < 3.
size_t encode_spp(uint16_t midi_beats, uint8_t* out, size_t cap) noexcept;

/// Convert a PPQ position to the SPP `midi_beats` value (sixteenth notes since
/// the start of the song), truncated to 14 bits.
uint16_t ppq_to_spp_beats(double ppq) noexcept;
/// Inverse of @ref ppq_to_spp_beats: SPP beats -> PPQ.
double spp_beats_to_ppq(uint16_t midi_beats) noexcept;

/// Convert SMPTE time to a quarter-frame piece (`piece` 0..7) data nibble byte,
/// returning the full [F1, data] pair length (2) into `out`. The 8 pieces in
/// sequence fully transmit one MtcTime (least-significant nibble first, frame
/// rate folded into piece 7's high nibble per the MTC spec). Returns 0 on bad
/// args.
size_t encode_mtc_quarter_frame(const MtcTime& time, int piece, uint8_t* out, size_t cap) noexcept;

/// Encode an MTC full-frame SysEx message:
/// [F0, 7F, device_id, 01, 01, hr, mn, se, fr, F7].
/// `device_id` is 0x7F for all-call. Returns 10 bytes written, or 0 on bad args
/// / invalid timecode.
size_t encode_mtc_full_frame(const MtcTime& time, uint8_t device_id, uint8_t* out,
                             size_t cap) noexcept;

/// Encode one MIDI transport real-time command byte (Start / Continue / Stop).
/// Returns 1 byte written, or 0 when `status` is not a transport command or
/// `out` is null / too small.
size_t encode_transport_command(uint8_t status, uint8_t* out, size_t cap) noexcept;

/// Stateful MTC quarter-frame stream generator. Emits pieces 0..7 in order for
/// the current timecode; after piece 7, advances the timecode by two SMPTE
/// frames because one full quarter-frame cycle spans eight quarter-frame
/// messages.
class MtcQuarterFrameGenerator {
 public:
  /// Sets the current stream position. Returns false and leaves the previous
  /// state unchanged when `start` is not a valid timecode.
  bool reset(const MtcTime& start, int next_piece = 0) noexcept;

  /// Encodes the next [F1, data] quarter-frame message and advances stream
  /// state. Returns 2 bytes written, or 0 on invalid args.
  size_t next(uint8_t* out, size_t cap) noexcept;

  const MtcTime& time() const noexcept { return time_; }
  int next_piece() const noexcept { return next_piece_; }

 private:
  MtcTime time_{};
  int next_piece_ = 0;
};

/// MIDI clock generator: emits 0xF8 ticks across a render-frame block.
class ClockGenerator {
 public:
  void prepare(const transport::TempoMap* tempo_map) noexcept { tempo_map_ = tempo_map; }

  /// AUDIO thread: append the clock (0xF8) bytes whose tick falls in
  /// [block_start_frame, block_start_frame + num_frames) to `out` (cleared
  /// first). RT-safe, no allocation. Returns the number of ticks emitted.
  /// Overflow drops surplus ticks and bumps overflow_count().
  size_t generate_clock_block(int64_t block_start_frame, int num_frames,
                              ClockByteOutput* out) noexcept;

  /// The absolute clock tick index of the first tick at/after `frame` (a clock
  /// tick fires every 1/24 quarter note). RT-safe.
  int64_t first_tick_at_or_after(int64_t frame) const noexcept;

  /// Render frame at which absolute clock tick `tick` fires. RT-safe.
  int64_t frame_of_tick(int64_t tick) const noexcept;

  uint32_t overflow_count() const noexcept { return overflow_count_.load(); }
  void reset_telemetry() noexcept { overflow_count_.reset(); }

 private:
  const transport::TempoMap* tempo_map_ = nullptr;
  rt::OverflowCounter overflow_count_{};
};

/// Incremental parser for incoming sync bytes. Tracks the last received SPP
/// position (in MIDI beats / PPQ) and counts clock ticks since the last SPP /
/// start, plus assembles MTC from quarter-frame pieces. Alloc-0 state machine.
class ClockParser {
 public:
  void reset() noexcept;

  /// Feed one incoming byte. RT-safe, no allocation. Returns true if the byte
  /// completed a meaningful message (a clock tick, an SPP, or a full MTC frame).
  bool parse_byte(uint8_t byte) noexcept;

  /// Clock ticks received since the last SPP / start / reset.
  int64_t clock_ticks() const noexcept { return clock_ticks_; }
  /// Estimated current position in PPQ: the SPP anchor plus elapsed clock ticks.
  double position_ppq() const noexcept;

  /// True once a complete SPP has been parsed since reset.
  bool has_spp() const noexcept { return has_spp_; }
  uint16_t spp_beats() const noexcept { return spp_beats_; }

  /// True once all 8 MTC quarter-frame pieces have been assembled.
  bool has_mtc() const noexcept { return mtc_complete_; }
  const MtcTime& mtc_time() const noexcept { return mtc_time_; }

 private:
  // Two-byte SPP assembly state.
  enum class Pending : uint8_t { kNone, kSppLsb, kSppMsb, kMtcData };
  Pending pending_ = Pending::kNone;
  uint8_t spp_lsb_ = 0;

  bool has_spp_ = false;
  uint16_t spp_beats_ = 0;
  int64_t clock_ticks_ = 0;

  // MTC quarter-frame assembly: 8 nibbles into a scratch MtcTime.
  std::array<uint8_t, 8> mtc_pieces_{};
  std::array<bool, 8> mtc_seen_{};
  bool mtc_complete_ = false;
  MtcTime mtc_time_{};

  void assemble_mtc() noexcept;
};

}  // namespace sonare::midi
