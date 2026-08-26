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
/// Coverage and lossiness, pinned by the round-trip test:
///   - Formats 0 and 1 import; format 2 (independent sequences) is rejected with
///     a diagnostic. Variable-length quantities and running status are handled.
///   - Channel-voice messages convert to UMP through the ump.h MIDI-1.0
///     byte-stream adapter, never hand-rolled.
///   - Set-tempo and time-signature populate the transport segments (metronome
///     bytes included); track name is captured as a string. The text-class events
///     (marker, text, lyric, cue point, key signature) land in
///     @ref SmfImportResult::markers tagged with a @ref SmfMarkerKind, collected
///     across all tracks into one timeline-global list — so a lyric's originating
///     track, and with it per-note alignment, is NOT preserved.
///   - SysEx payloads live in @ref SmfImportResult::sysex_store and appear in
///     clips as handle UMP events. F7 escapes normalise to F0 on export, since
///     the handle stores payload bytes rather than the original status byte.
///   - Unrecognized meta events and MIDI 2.0-only messages are skipped lossily
///     and counted in the result's `skipped_events`; a skipped event's delta time
///     is still consumed, so what follows stays in time.
///   - Export writes format 1: track 0 carries the tempo/time-signature map and
///     any @ref SmfExportOptions::markers, then one track per MidiClip. SysEx
///     handles re-emit only when `SmfExportOptions::sysex_store` is supplied.

#include <cstdint>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "transport/tempo_map.h"
#include "util/resource_limits.h"

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

/// Kind of a recognized SMF text-class meta event. Distinguishes markers from
/// the other timeline annotations (text / lyric / cue point / key signature)
/// that share the flat marker list. Values are part of the cross-surface ABI
/// (mirrored by SonareMarkerKind) and must not be renumbered.
enum class SmfMarkerKind : uint8_t {
  kMarker = 0,        ///< Marker meta (0x06).
  kText = 1,          ///< Text meta (0x01).
  kLyric = 2,         ///< Lyric meta (0x05).
  kCuePoint = 3,      ///< Cue point meta (0x07).
  kKeySignature = 4,  ///< Key signature meta (0x59).
};

/// A text-class meta event positioned in musical time. @ref kind distinguishes
/// markers, text, lyrics, cue points and key signatures. For kKeySignature the
/// structured key is carried in @ref key_fifths / @ref key_minor and @ref text
/// holds a human-readable rendering (e.g. "C major") for a display fallback.
struct SmfMarker {
  double ppq = 0.0;
  std::string text;
  SmfMarkerKind kind = SmfMarkerKind::kMarker;
  int8_t key_fifths = 0;   ///< Key signature only: -7..7 (sharps positive).
  bool key_minor = false;  ///< Key signature only: false = major, true = minor.
};

/// Result of importing an SMF byte buffer into normalized sonare::midi data.
struct SmfImportResult {
  SmfStatus status = SmfStatus::kOk;
  /// Human-readable diagnostic (empty when status == kOk). A truncated track
  /// yields kTruncated even when valid prefix/later-track data was recovered.
  std::string diagnostic;

  /// SMF division: ticks per quarter note (PPQN). 0 when SMPTE division was
  /// requested (SMPTE timing is not supported and yields a diagnostic).
  uint16_t ticks_per_quarter = 0;
  /// SMF format word (0 or 1) as parsed from the header.
  uint16_t format = 0;

  /// Sequence / song name, taken from the track-name meta of a conductor track
  /// (a meta-only track that carries no MIDI events — conventionally track 0 in
  /// a format-1 file). Empty when no such name is present. Track names that
  /// belong to tracks with MIDI events stay in `clip_names`, not here.
  std::string sequence_name;

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

  /// True when the importer recovered meaningful SMF content despite a
  /// truncated track. Project-level import APIs may install these results,
  /// while a completely unreadable truncated file remains an error.
  bool recoverable() const noexcept {
    return ok() || (status == SmfStatus::kTruncated && has_recovered_content);
  }

 private:
  // Kept internal to the importer contract; callers should use recoverable().
  bool has_recovered_content = false;

  friend SmfImportResult import_smf(const uint8_t* data, size_t size,
                                    const resource::MidiImportResourceLimits& limits);
};

/// Imports an in-memory SMF byte buffer. Never crashes / reads out of bounds on
/// malformed or truncated input: a diagnostic status is returned instead. A
/// null `data` with non-zero `size`, or a header shorter than a valid MThd
/// chunk, yields an error status.
SmfImportResult import_smf(
    const uint8_t* data, size_t size,
    const resource::MidiImportResourceLimits& limits = resource::kDefaultMidiImportResourceLimits);

/// Convenience overload taking a byte vector.
inline SmfImportResult import_smf(const std::vector<uint8_t>& data) {
  return import_smf(data.data(), data.size());
}

inline SmfImportResult import_smf(const std::vector<uint8_t>& data,
                                  const resource::MidiImportResourceLimits& limits) {
  return import_smf(data.data(), data.size(), limits);
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
/// channel-voice data and stored SysEx payloads. Format 1 reserves one 16-bit
/// track-count slot for the conductor track, so more than 65,534 clips are
/// rejected before any output bytes are written.
SmfExportResult export_smf(const std::vector<MidiClip>& clips,
                           const std::vector<transport::TempoSegment>& tempo_segments,
                           const std::vector<transport::TimeSignatureSegment>& time_signatures,
                           const std::vector<std::string>& clip_names = {},
                           const SmfExportOptions& options = {});

}  // namespace sonare::midi
