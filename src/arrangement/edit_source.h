#pragma once

/// @file edit_source.h
/// @brief Clip source references for the headless arrangement model.
///
/// A @ref sonare::arrangement::ClipSource is a value-oriented variant of source
/// references. The arrangement model never opens files or holds live realtime
/// (RT) objects; sources are pure metadata keyed by a stable id allocated on the
/// @ref sonare::arrangement::Project. Audio sources reference host-local data by
/// URI/string (the core does not perform file I/O) or by an inline storage
/// handle id for captured/generated audio. MIDI sources are reserved here so
/// runtime MIDI clip payloads can be attached without changing the project model.

#include <cstdint>
#include <string>
#include <variant>

namespace sonare::arrangement {

/// Stable identifier for a registered clip source. 0 means "unset/invalid".
/// Ids are allocated by a deterministic monotonic counter on the Project and are
/// stable across serialization, undo, and bindings.
using SourceId = uint32_t;

/// Reference to audio content. The core never opens files: audio is referenced
/// either by a host-local URI/string (e.g. a file path or asset id the host
/// resolves) or by an inline storage handle id for captured/generated audio
/// that the host registers separately. This is pure value data.
struct AudioSourceRef {
  /// Stable source id (mirrors the registry key; kept inline for value copies).
  SourceId id = 0;
  /// Host-local reference (URI / file path / asset id). The core treats this as
  /// an opaque, non-interpreted string and never opens it.
  std::string uri;
  /// Optional channel-count hint (0 = unknown; filled by the host/decoder).
  uint32_t channel_count = 0;
  /// Optional sample-rate hint in Hz (0 = unknown). The compiler resamples to
  /// the project sample rate when this differs; the model only stores the hint.
  double sample_rate_hint = 0.0;
  /// Optional inline storage handle id for captured/generated audio referenced
  /// by id rather than URI (0 = none). The core stores the id only and does not
  /// own or interpret the underlying buffer.
  uint32_t storage_handle_id = 0;
  /// Optional opaque content hash for integrity / identity (empty = unset). The
  /// host computes this (e.g. a digest of the referenced audio); the core treats
  /// it as a non-interpreted string and only round-trips it. Kept absent from
  /// serialized output when empty so existing projects stay byte-identical.
  std::string content_hash;
};

/// Reference to MIDI content. Reserved: only the stable id and minimal
/// metadata are present. The actual MIDI event payload (UMP / MidiClip) is
/// attached without altering this struct.s identity contract.
struct MidiSourceRef {
  /// Stable source id (mirrors the registry key).
  SourceId id = 0;
  /// Optional human-readable label for the MIDI source (owned). Purely
  /// informational; not interpreted by the core.
  std::string name;
  /// Reserved channel hint for MIDI routing (0 = unset). Kept so routing can populate
  /// runtime metadata without changing the variant shape.
  uint32_t channel_hint = 0;
};

/// Discriminated source reference. Adding future source kinds extends this
/// variant; the variant index is the source "kind".
using ClipSource = std::variant<AudioSourceRef, MidiSourceRef>;

/// Stable kind tags for a ClipSource, matching the variant alternative order.
/// Kept as an explicit enum so callers (and later bindings) do not depend on
/// std::variant::index() ordering directly.
enum class SourceKind : uint32_t {
  kAudio = 0,
  kMidi = 1,
};

/// Returns the kind tag for a source variant.
inline SourceKind source_kind(const ClipSource& source) noexcept {
  return std::holds_alternative<AudioSourceRef>(source) ? SourceKind::kAudio : SourceKind::kMidi;
}

/// Returns the stable id stored inside a source variant.
inline SourceId source_id(const ClipSource& source) noexcept {
  if (const auto* audio = std::get_if<AudioSourceRef>(&source)) {
    return audio->id;
  }
  return std::get<MidiSourceRef>(source).id;
}

}  // namespace sonare::arrangement
