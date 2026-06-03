#pragma once

/// @file project_view.h
/// @brief Read-only views over the arrangement model (ProjectView / MidiClipView).
///
/// These views are owned conceptually by the project model layer: they are thin,
/// non-mutating accessors that bundle the read-only state an offline consumer
/// needs (tracks / clips / tempo / harmony / its own assist sidecar). They are
/// reused by composition-assist so that assist modules read a stable,
/// immutable surface and NEVER touch a live Project. The view holds const
/// references to externally owned model objects (a snapshot lifetime contract:
/// the caller guarantees the referents outlive the view; for a background thread
/// the caller takes an immutable copy and views that).
///
/// Control-thread / offline only. No mutation, no I/O, no clock, no random.

#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"

namespace sonare::arrangement {

/// @brief Read-only accessor over a single clip's MIDI event list.
///
/// Wraps a const reference to a @ref MidiClipEventList (as stored in the
/// command-layer @ref MidiContentStore). Provides positional read access only.
class MidiClipView {
 public:
  explicit MidiClipView(const MidiClipEventList& events) noexcept : events_(events) {}

  /// @brief The clip's PPQ-positioned MIDI events (sorted as stored).
  const MidiClipEventList& events() const noexcept { return events_; }
  /// @brief Number of events on the clip.
  size_t size() const noexcept { return events_.size(); }
  bool empty() const noexcept { return events_.empty(); }

 private:
  const MidiClipEventList& events_;
};

/// @brief Read-only accessor over a Project + MidiContentStore + harmony, scoped
///        to one assist module's sidecar by module_id.
///
/// The view exposes the model's read surface (tracks, clips, sources, tempo,
/// harmonic timeline) plus a lookup of THIS module's own assist sidecar so an
/// assist module can resume from its persisted state. It never mutates and never
/// exposes a mutable handle. The harmony timeline is the shared
/// @ref HarmonicTimeline (key + chord-symbol granularity).
class ProjectView {
 public:
  /// @param project owning project (must outlive the view).
  /// @param midi    the command-layer MIDI content store (must outlive the view).
  /// @param module_id the assist module's namespacing key; sidecar lookups below
  ///                  are scoped to this module. Empty means "no module scope".
  ProjectView(const Project& project, const MidiContentStore& midi,
              std::string module_id = {}) noexcept
      : project_(project), midi_(midi), module_id_(std::move(module_id)) {}

  // ---- Tracks / clips / sources -------------------------------------------

  const std::vector<Track>& tracks() const noexcept { return project_.tracks(); }
  const Track* find_track(TrackId id) const noexcept { return project_.find_track(id); }

  const std::vector<EditClip>& clips() const noexcept { return project_.clips(); }
  const EditClip* find_clip(ClipId id) const noexcept { return project_.find_clip(id); }

  const std::vector<ClipSource>& sources() const noexcept { return project_.sources(); }
  const ClipSource* find_source(SourceId id) const noexcept { return project_.find_source(id); }

  // ---- Tempo / time ---------------------------------------------------------

  const std::vector<transport::TempoSegment>& tempo_segments() const noexcept {
    return project_.tempo_segments();
  }
  const std::vector<transport::TimeSignatureSegment>& time_signatures() const noexcept {
    return project_.time_signatures();
  }
  double sample_rate() const noexcept { return project_.sample_rate(); }

  // ---- Harmony (shared HarmonicTimeline) -----------------------------------

  /// @brief The project's key + chord-symbol timeline (read-only).
  const HarmonicTimeline& harmony() const noexcept { return project_.annotation().harmony(); }

  // ---- MIDI content ---------------------------------------------------------

  /// @brief Returns a read-only view of clip `clip_id`'s MIDI events, or nullptr
  ///        when the clip has no event list in the store.
  const MidiClipEventList* clip_events(ClipId clip_id) const noexcept {
    const auto it = midi_.events.find(clip_id);
    return it == midi_.events.end() ? nullptr : &it->second;
  }

  // ---- This module's assist sidecar ----------------------------------------

  /// @brief The module_id this view is scoped to (sidecar lookup key).
  const std::string& module_id() const noexcept { return module_id_; }

  /// @brief Returns the first assist sidecar owned by this view's module_id and
  ///        optionally matching `target_track_id` (0 = ignore track scope), or
  ///        nullptr when none. The core never interprets the payload bytes.
  const AssistSidecar* module_sidecar(TrackId target_track_id = 0) const noexcept {
    for (const auto& s : project_.assist_sidecars()) {
      if (s.module_id != module_id_) continue;
      if (target_track_id != 0 && s.target_track_id != target_track_id) continue;
      return &s;
    }
    return nullptr;
  }

  /// @brief The full underlying project (read-only). Provided for completeness so
  ///        modules can reach rarely-needed read accessors without widening this
  ///        view; it never exposes a mutable handle.
  const Project& project() const noexcept { return project_; }

 private:
  const Project& project_;
  const MidiContentStore& midi_;
  std::string module_id_;
};

}  // namespace sonare::arrangement
