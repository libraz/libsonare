#pragma once

/// @file smf2.h
/// @brief MIDI 2.0 Clip File (SMF2 / "MIDI Clip File", MIDI Association
///        M2-116-U v1.0) import / export.
///
/// Unlike @ref smf.h (Standard MIDI File, a MIDI 1.0 byte stream), the MIDI Clip
/// File is a stream of Universal MIDI Packets (UMP). It can therefore carry
/// MIDI 2.0 channel-voice messages (16-bit velocity, 32-bit CC, per-note and
/// registered/assignable controllers, bank-valid Program Change) LOSSLESSLY —
/// which the MIDI 1.0 SMF export drops. That losslessness is the reason this
/// format exists alongside the SMF path.
///
/// File layout (all multi-byte values are BIG-ENDIAN 32-bit UMP words):
///   - 8-byte ASCII file header "SMF2CLIP".
///   - Clip Configuration Header: a Delta Clockstamp of 0 (DCS, Utility MT=0x0
///     status 0x40) followed by a DCTPQ message (Utility status 0x30, low 16
///     bits = ticks per quarter note), then optional DCS-prepended config UMPs,
///     then DCS(0) + Start of Clip (UMP Stream MT=0xF status 0x20).
///   - Clip Sequence Data: DCS-prepended UMPs, terminated by DCS + End of Clip
///     (UMP Stream status 0x21).
///
/// Timing: every message is positioned by the running sum of the most recent
/// Delta Clockstamp tick deltas, converted to PPQ (quarter-note units) via the
/// DCTPQ resolution — matching @ref MidiClipEvent::ppq.
///
/// Meta (tempo / time-signature / key-signature / text) rides Flex Data
/// messages (MT=0xD). SMF "Marker" meta events have NO Flex Data equivalent in
/// the spec, so markers are NOT round-tripped through this format.
///
/// The importer never reads out of bounds on malformed / truncated input: it
/// returns a diagnostic status instead (same robustness contract as import_smf).
/// Fatal structural errors such as missing DCTPQ or truncated UMP words are
/// transactional: partially parsed sequence data is discarded and callers must
/// check status before consuming clips.

#include <cstdint>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

namespace sonare::midi {

/// Status of a MIDI Clip File import / export operation.
enum class Smf2Status : uint8_t {
  kOk = 0,
  kBadHeader,        ///< Missing / malformed "SMF2CLIP" file header.
  kTruncated,        ///< Buffer ended before a UMP word / the End of Clip.
  kMissingDctpq,     ///< No DCTPQ message in the configuration header.
  kInvalidArgument,  ///< Null / empty input where data was required.
};

/// Result of importing a MIDI Clip File byte buffer into normalized midi data.
///
/// A Clip File holds exactly one clip, so `clips` has size 0 (empty file) or 1.
/// The vector shape mirrors @ref SmfImportResult so the same C-ABI install path
/// can consume either importer.
struct Smf2ImportResult {
  Smf2Status status = Smf2Status::kOk;
  /// Human-readable diagnostic (empty when status == kOk).
  std::string diagnostic;

  /// DCTPQ resolution: Delta Clockstamp ticks per quarter note.
  uint16_t ticks_per_quarter = 0;

  /// The imported clip(s) (0 or 1). Channel-voice UMPs are stored LOSSLESSLY,
  /// preserving their original MIDI 1.0 / 2.0 message type.
  std::vector<MidiClip> clips;
  /// Track name (from Flex Data metadata), parallel to `clips`.
  std::vector<std::string> clip_names;
  /// Imported clip length in PPQ, parallel to `clips` (the End-of-Clip tick).
  std::vector<double> clip_lengths_ppq;

  /// Tempo map from Flex Data Set Tempo messages (sorted by start_ppq; defaults
  /// to a single 120 BPM segment when none present).
  std::vector<transport::TempoSegment> tempo_segments;
  /// Time-signature map from Flex Data Set Time Signature messages (sorted by
  /// start_ppq; defaults to a single 4/4 segment when none present).
  std::vector<transport::TimeSignatureSegment> time_signatures;
  /// SysEx payloads imported from SysEx7 / SysEx8 UMP data messages. Clip events
  /// reference entries here via Ump::sysex_handle.
  SysExStore sysex_store;

  /// Count of UMP messages skipped lossily (unsupported message types, unknown
  /// Flex Data forms, SysEx payloads that could not be stored, etc.).
  uint32_t skipped_events = 0;

  bool ok() const noexcept { return status == Smf2Status::kOk; }
};

/// Imports an in-memory MIDI Clip File. Never crashes / reads out of bounds on
/// malformed or truncated input: a diagnostic status is returned instead. A
/// null `data` with non-zero `size`, or a buffer shorter than the 8-byte file
/// header, yields an error status.
Smf2ImportResult import_clip_file(const uint8_t* data, size_t size);

/// Convenience overload taking a byte vector.
inline Smf2ImportResult import_clip_file(const std::vector<uint8_t>& data) {
  return import_clip_file(data.data(), data.size());
}

/// Options controlling MIDI Clip File export.
struct Smf2ExportOptions {
  /// Delta Clockstamp ticks per quarter note written as DCTPQ (and used to
  /// quantize PPQ event positions to integer ticks). Defaults to 480.
  uint16_t ticks_per_quarter = 480;
  /// Optional payload store used to serialize UMP SysEx handles back to UMP
  /// SysEx7 data messages. When null, SysEx-handle events are skipped (and
  /// counted in SmfExportResult::skipped_events).
  const SysExStore* sysex_store = nullptr;
  /// Optional clip / track name written as a Flex Data metadata message.
  std::string name;
};

/// Result of exporting a clip to a MIDI Clip File byte buffer.
struct Smf2ExportResult {
  Smf2Status status = Smf2Status::kOk;
  std::string diagnostic;
  /// The serialized "SMF2CLIP" byte buffer (empty on error).
  std::vector<uint8_t> bytes;
  /// Count of events that could not be represented and were dropped.
  uint32_t skipped_events = 0;

  bool ok() const noexcept { return status == Smf2Status::kOk; }
};

/// Exports a single clip plus its tempo / time-signature maps to a MIDI Clip
/// File. MIDI 2.0-only messages are written WITHOUT loss (this is the format's
/// purpose). Events are taken in their existing order; the clip should be sorted
/// by the caller (MidiClip::sort_stable) for deterministic output.
Smf2ExportResult export_clip_file(
    const MidiClip& clip, const std::vector<transport::TempoSegment>& tempo_segments,
    const std::vector<transport::TimeSignatureSegment>& time_signatures,
    const Smf2ExportOptions& options);

}  // namespace sonare::midi
