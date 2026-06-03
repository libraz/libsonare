#pragma once

/// @file sound_destination.h
/// @brief Value descriptors for a MIDI destination — a host instrument graph
///        node id, or an external MIDI port descriptor — plus a small table
///        mapping a stable destination id to its descriptor.
///
/// Scope: this is DATA ONLY. It describes WHERE a sequencer's events should go,
/// not how to talk to a device. Live external MIDI I/O and the host instrument
/// graph seam; a destination resolves to a host
/// instrument node id (a future graph node) or to an external port descriptor
/// that is exercised only with null / mock destinations. No OS handles, no
/// device pointers, no I/O are stored here.
///
/// Layering: depends only on the standard library. The DestinationTable lives on
/// the CONTROL thread (build / edit time); it MAY allocate. The descriptors it
/// holds are plain value data the RT path can copy a `destination_id` against.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sonare::midi {

/// What a destination ultimately points at.
enum class DestinationKind : uint8_t {
  /// Nothing — events are discarded (default null destination).
  kNull = 0,
  /// A host instrument node in the (future) processing graph. Identified by a
  /// stable opaque node id assigned by the host; meaningful once the host graph
  /// seam exists. Until then it is data only and resolves to a mock node.
  kHostInstrument = 1,
  /// An external MIDI output port, described by data only (no OS handle). The
  /// live connection is opened.
  kExternalPort = 2,
};

/// Describes a host instrument graph node target. `node_id` is a stable id the
/// host assigns to the instrument node; 0 means "unassigned / mock".
struct HostInstrumentDescriptor {
  uint32_t node_id = 0;
  /// Optional human-readable label (editor display). Empty by default.
  std::string label;

  bool operator==(const HostInstrumentDescriptor& o) const noexcept {
    return node_id == o.node_id && label == o.label;
  }
};

/// Describes an external MIDI output port WITHOUT any OS handle. The fields are
/// the data the host needs to resolve a live port; they are inert
/// here. `port_id` is a host-stable index/id; `port_name` and `device_name` are
/// for display / matching.
struct ExternalPortDescriptor {
  uint32_t port_id = 0;
  std::string port_name;
  std::string device_name;
  /// UMP group (0..15) events on this destination are emitted on.
  uint8_t group = 0;
  /// True if this port speaks the MIDI 2.0 UMP protocol; false = MIDI 1.0.
  bool is_midi2 = false;

  bool operator==(const ExternalPortDescriptor& o) const noexcept {
    return port_id == o.port_id && port_name == o.port_name && device_name == o.device_name &&
           group == o.group && is_midi2 == o.is_midi2;
  }
};

/// A resolved destination descriptor: a tagged union (by `kind`) of the two
/// target descriptors. For kNull both sub-descriptors are default. Plain value
/// data; copyable.
struct SoundDestination {
  DestinationKind kind = DestinationKind::kNull;
  HostInstrumentDescriptor host_instrument{};
  ExternalPortDescriptor external_port{};

  /// Builds a null destination (events discarded).
  static SoundDestination Null() noexcept { return SoundDestination{}; }
  /// Builds a host-instrument destination for `node_id` with optional `label`.
  static SoundDestination HostInstrument(uint32_t node_id, std::string label = {});
  /// Builds an external-port destination.
  static SoundDestination ExternalPort(uint32_t port_id, std::string port_name,
                                       std::string device_name = {}, uint8_t group = 0,
                                       bool is_midi2 = false);

  bool operator==(const SoundDestination& o) const noexcept {
    return kind == o.kind && host_instrument == o.host_instrument &&
           external_port == o.external_port;
  }
  bool operator!=(const SoundDestination& o) const noexcept { return !(*this == o); }
};

/// Maps a stable `destination_id` (the id carried on a MidiClipSchedule and used
/// by the sequencer) to a SoundDestination descriptor. CONTROL-thread owned;
/// build / edit time only, MAY allocate. The RT path never touches this table —
/// it only carries the integer `destination_id`; the host resolves the
/// descriptor off the audio thread.
///
/// destination_id 0 is reserved for the implicit null destination: lookup of 0
/// always succeeds with a null descriptor and it cannot be added or removed.
class DestinationTable {
 public:
  /// Reserved id for the always-present null destination.
  static constexpr uint32_t kNullDestinationId = 0;

  DestinationTable() = default;

  /// Adds or replaces the descriptor for `destination_id`. Adding id 0 is a
  /// no-op (the null destination is implicit and immutable). Returns true if the
  /// table now holds the given descriptor for a non-reserved id.
  bool add(uint32_t destination_id, const SoundDestination& destination);

  /// Removes the descriptor for `destination_id`. Removing id 0 is a no-op.
  /// Returns true if an entry was removed.
  bool remove(uint32_t destination_id);

  /// True if `destination_id` resolves (id 0 always does).
  bool contains(uint32_t destination_id) const noexcept;

  /// Resolves `destination_id` to its descriptor. Id 0 (and any unknown id)
  /// returns a null descriptor; unknown ids never throw. Use @ref contains to
  /// distinguish an explicit null entry from an unknown id.
  SoundDestination lookup(uint32_t destination_id) const;

  /// Number of explicitly-stored descriptors (excludes the implicit null id 0).
  size_t size() const noexcept { return table_.size(); }

  /// Removes all explicit descriptors (the implicit null id 0 remains).
  void clear() noexcept { table_.clear(); }

  /// All explicitly-stored destination ids, sorted ascending (deterministic).
  std::vector<uint32_t> ids() const;

 private:
  std::unordered_map<uint32_t, SoundDestination> table_;
};

}  // namespace sonare::midi
