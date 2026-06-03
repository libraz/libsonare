#pragma once

/// @file routing.h
/// @brief RT-safe MIDI routing: group/channel filter, channel remap and MIDI
///        thru, transforming an input UMP event stream into a fixed-capacity
///        output.
///
/// Layering: depends ONLY on midi/ump and midi/midi_event (+ std). It does NOT
/// depend on engine/ or arrangement/ (one-way dependency; no cycle).
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread: set_config() installs a new MidiRouteConfig. Trivially
///    copyable POD config; the call itself is a plain struct copy (no alloc),
///    but it MUST NOT race process() on the audio thread — configure before the
///    route is handed to the audio path, or publish through the engine's
///    RtPublisher upstream.
///  - AUDIO thread: process() filters / remaps / thru-passes events from an
///    input span into a fixed-capacity MidiRouteOutput. ZERO heap allocation,
///    no lock, no I/O. When more events pass the filter than the output can
///    hold, the surplus is DROPPED and an atomic overflow telemetry counter is
///    bumped; the output is never grown.

#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "rt/overflow_counter.h"

namespace sonare::midi {

/// Sentinel meaning "match any group" / "match any channel" in a filter field.
inline constexpr uint8_t kRouteAnyGroup = 0xFFu;
inline constexpr uint8_t kRouteAnyChannel = 0xFFu;
/// Sentinel meaning "do not remap the channel" in a remap field.
inline constexpr uint8_t kRouteNoRemap = 0xFFu;

/// Fixed-capacity output of a single routing pass. The capacity is intentionally
/// generous for one audio block of dense polyphony; surplus events are dropped
/// (telemetry on the owning MidiRouter), never grown.
struct MidiRouteOutput {
  static constexpr size_t kCapacity = 512;
  std::array<MidiEvent, kCapacity> events{};
  size_t size = 0;
  /// True when at least one event was dropped because the buffer filled.
  bool overflowed = false;

  void clear() noexcept {
    size = 0;
    overflowed = false;
  }
};

/// POD routing configuration. Trivially copyable so it can ride RT structures
/// and be swapped wholesale on the control thread.
struct MidiRouteConfig {
  /// Filter: only events on this group pass. kRouteAnyGroup = accept any group.
  uint8_t filter_group = kRouteAnyGroup;
  /// Filter: only events on this channel pass. kRouteAnyChannel = accept any.
  /// Non-channel-voice messages (e.g. SysEx handles) bypass the channel filter.
  uint8_t filter_channel = kRouteAnyChannel;
  /// Remap: rewrite a channel-voice message's channel to this value.
  /// kRouteNoRemap = leave the channel unchanged.
  uint8_t remap_channel = kRouteNoRemap;
  /// MIDI thru toggle. When false, process() emits nothing (all input dropped,
  /// no overflow). When true, filtered/remapped events are passed through.
  bool thru = true;
};

/// RT-safe MIDI router. Holds a POD config plus an atomic overflow counter.
class MidiRouter {
 public:
  MidiRouter() = default;

  /// CONTROL thread: install a new config. Plain struct copy; see RT contract.
  void set_config(const MidiRouteConfig& config) noexcept { config_ = config; }
  const MidiRouteConfig& config() const noexcept { return config_; }

  /// AUDIO thread: route `count` input events into `out` (which is cleared
  /// first). Returns the number of events written. RT-safe, no allocation.
  /// Events that pass the filter are channel-remapped (if configured) and
  /// appended. Once `out` is full, further passing events are dropped and the
  /// overflow telemetry counter is incremented.
  size_t process(const MidiEvent* input, size_t count, MidiRouteOutput* out) noexcept;

  /// Total events dropped due to output overflow since construction / reset.
  uint32_t overflow_count() const noexcept { return overflow_count_.load(); }

  /// AUDIO/CONTROL thread: clear the overflow telemetry counter.
  void reset_telemetry() noexcept { overflow_count_.reset(); }

  /// AUDIO/CONTROL thread: forget all sounding-note remap state. Use when the
  /// route is flushed (e.g. all-notes-off / transport stop) so a subsequent
  /// note-off is not matched against a stale pre-flush note-on.
  void reset_active_notes() noexcept { active_count_ = 0; }

 private:
  // Tracks the remapped channel chosen for a currently-sounding note so its
  // matching note-off lands on the SAME channel as its note-on even if the
  // remap config changes mid-note (otherwise the note hangs on the synth).
  // Keyed by the note's ORIGINAL (pre-remap) identity.
  struct ActiveRemap {
    uint8_t group = 0;
    uint8_t src_channel = 0;
    uint8_t note = 0;
    uint8_t out_channel = 0;
  };
  static constexpr size_t kMaxActiveRemaps = 256;

  // Returns true if `ump` passes the configured group/channel filter.
  bool passes_filter(const Ump& ump) const noexcept;
  // Applies channel remap to a channel-voice UMP (returns a possibly-rewritten
  // copy). Non-channel-voice messages are returned unchanged. Note-on records
  // the chosen channel; note-off reuses the recorded channel for a stable pair.
  Ump apply_remap(const Ump& ump) noexcept;
  // Rewrites the channel nibble of a channel-voice UMP to `channel`.
  static Ump with_channel(const Ump& ump, uint8_t channel) noexcept;
  size_t find_active_remap(uint8_t group, uint8_t src_channel, uint8_t note) const noexcept;

  MidiRouteConfig config_{};
  std::array<ActiveRemap, kMaxActiveRemaps> active_remaps_{};
  size_t active_count_ = 0;
  rt::OverflowCounter overflow_count_{};
};

}  // namespace sonare::midi
