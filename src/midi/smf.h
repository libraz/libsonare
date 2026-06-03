#pragma once

/// @file smf.h
/// @brief Standard MIDI File (SMF) import / export operating on in-memory byte
///        buffers only. Converts between SMF format 0/1 byte
///        streams and normalized sonare::midi data (PPQ-timed MidiClip lists +
///        a transport tempo / time-signature map + track names / markers).
///
/// Layering: depends ONLY on midi/ump, midi/midi_clip and transport/. Does NOT
/// depend on arrangement/ or engine/. Control-thread only (parse / serialize
/// MAY allocate); nothing here runs on the audio thread.
///
/// core buffer-only: there is NO file I/O here. The caller is responsible for
/// reading / writing the byte buffer from disk (or a WASM ArrayBuffer). Import
/// takes a const byte view; export returns a std::vector<uint8_t>.
///
/// Feature coverage / lossiness (pinned by the round-trip test):
///   - SMF format 0 (single track) and format 1 (multi-track) are imported.
///     Format 2 (independent sequences) is rejected with a diagnostic.
///   - Channel-voice messages (note on/off, poly pressure, control change,
///     program change, channel pressure, pitch bend) are converted to UMP via
///     the ump.h MIDI-1.0 byte-stream adapter (never hand-rolled).
///   - Variable-length quantities and running status are handled on import.
///   - Meta events: set-tempo, time-signature, track name, marker and
///     end-of-track are recognized. Set-tempo / time-signature populate the
///     transport segment vectors, including SMF time-signature metronome bytes;
///     track name / marker are captured as strings.
///   - SysEx (F0 / F7) payloads are preserved in @ref SmfImportResult::sysex_store
///     and represented in clips as SysEx-handle UMP events. F7 escape status is
///     normalized to an F0 SysEx event on export because the normalized UMP
///     handle intentionally stores payload bytes, not the original SMF status
///     byte. Unrecognized meta events are skipped lossily (their delta time is
///     still consumed so timing of following events is correct), and counted in
///     @ref SmfImportResult::skipped_events.
///   - Export writes format 1: one tempo/meta track (track 0) carrying the
///     tempo + time-signature map, followed by one track per MidiClip whose
///     channel-voice UMP events are serialized back via the ump.h adapter.
///     SysEx handles are re-emitted as F0 SysEx events only when
///     SmfExportOptions::sysex_store is supplied. Markers supplied in
///     SmfExportOptions::markers are written to track 0. MIDI 2.0-only messages
///     are skipped lossily and counted in SmfExportResult::skipped_events.

#include <cstdint>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "transport/tempo_map.h"

namespace sonare::midi {

/// Status of an SMF import / export operation.
enum class SmfStatus : uint8_t {
  kOk = 0,
  kBadHeader,          ///< Missing / malformed "MThd" header chunk.
  kUnsupportedFormat,  ///< SMF format 2 (or an out-of-range format word).
  kTruncated,          ///< Buffer ended before a chunk / event completed.
  kBadTrack,           ///< Missing / malformed "MTrk" chunk header.
  kInvalidArgument,    ///< Null / empty input where data was required.
};

/// A named marker meta event positioned in musical time.
struct SmfMarker {
  double ppq = 0.0;
  std::string text;
};

/// Result of importing an SMF byte buffer into normalized sonare::midi data.
struct SmfImportResult {
  SmfStatus status = SmfStatus::kOk;
  /// Human-readable diagnostic (empty when status == kOk).
  std::string diagnostic;

  /// SMF division: ticks per quarter note (PPQN). 0 when SMPTE division was
  /// requested (SMPTE timing is not supported and yields a diagnostic).
  uint16_t ticks_per_quarter = 0;
  /// SMF format word (0 or 1) as parsed from the header.
  uint16_t format = 0;

  /// One MidiClip per imported track that carried MIDI events. Tracks that held
  /// only meta events (e.g. a conductor track) produce no clip.
  std::vector<MidiClip> clips;
  /// Track name (if any) parallel to `clips` by index. Empty string when the
  /// track had no name meta event.
  std::vector<std::string> clip_names;
  /// Imported clip length in PPQ, parallel to `clips`. Derived from the track's
  /// end-of-track tick when present; otherwise from the final parsed tick.
  std::vector<double> clip_lengths_ppq;

  /// Tempo map extracted from set-tempo meta events (sorted by start_ppq, at
  /// least one segment — defaults to 120 BPM when none present).
  std::vector<transport::TempoSegment> tempo_segments;
  /// Time-signature map (sorted by start_ppq, at least one 4/4 segment).
  std::vector<transport::TimeSignatureSegment> time_signatures;
  /// Markers across all tracks.
  std::vector<SmfMarker> markers;
  /// SysEx payloads imported from F0 / F7 events. Clip events reference entries
  /// here via Ump::sysex_handle.
  SysExStore sysex_store;

  /// Count of events skipped lossily (unknown meta, unsupported system events,
  /// SysEx payloads that could not be stored, etc.).
  uint32_t skipped_events = 0;

  bool ok() const noexcept { return status == SmfStatus::kOk; }
};

/// Imports an in-memory SMF byte buffer. Never crashes / reads out of bounds on
/// malformed or truncated input: a diagnostic status is returned instead. A
/// null `data` with non-zero `size`, or a header shorter than a valid MThd
/// chunk, yields an error status.
SmfImportResult import_smf(const uint8_t* data, size_t size);

/// Convenience overload taking a byte vector.
inline SmfImportResult import_smf(const std::vector<uint8_t>& data) {
  return import_smf(data.data(), data.size());
}

/// Options controlling SMF export.
struct SmfExportOptions {
  /// Ticks per quarter note written to the header (and used to quantize PPQ
  /// event positions to integer ticks). Defaults to the common 480 PPQN.
  uint16_t ticks_per_quarter = 480;
  /// Optional payload store used to serialize UMP SysEx handles back to SMF.
  /// When omitted, SysEx-handle events are skipped without failing export.
  const SysExStore* sysex_store = nullptr;
  /// Optional marker meta events written to track 0.
  std::vector<SmfMarker> markers;
};

/// Result of exporting normalized data to an SMF byte buffer.
struct SmfExportResult {
  SmfStatus status = SmfStatus::kOk;
  std::string diagnostic;
  std::vector<uint8_t> bytes;
  /// Count of events skipped lossily during export (unresolved SysEx handles,
  /// MIDI 2.0-only controller forms, non-channel voice packets, etc.).
  uint32_t skipped_events = 0;

  bool ok() const noexcept { return status == SmfStatus::kOk; }
};

/// Serializes PPQ-timed MidiClips plus a tempo / time-signature map into a
/// format-1 SMF byte buffer. Track 0 carries the tempo + time-signature meta
/// (and end-of-track); each clip becomes one MTrk whose channel-voice UMP
/// events are serialized via the ump.h adapter. SysEx-handle events are written
/// when `options.sysex_store` can resolve the payload. `clip_names` (if
/// non-empty and index-parallel to `clips`) supplies per-track name meta events.
/// The result is round-trippable through @ref import_smf for MIDI 1.0
/// channel-voice data and stored SysEx payloads.
SmfExportResult export_smf(const std::vector<MidiClip>& clips,
                           const std::vector<transport::TempoSegment>& tempo_segments,
                           const std::vector<transport::TimeSignatureSegment>& time_signatures,
                           const std::vector<std::string>& clip_names = {},
                           const SmfExportOptions& options = {});

}  // namespace sonare::midi
