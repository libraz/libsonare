#pragma once

/// @file edit_model.h
/// @brief Headless arrangement / DAW project data model (control-thread only).
///
/// This subsystem is value-oriented and CONTROL-THREAD-ONLY. It holds no live
/// realtime (RT) object pointers, performs no file or device I/O, and uses no
/// system headers. The arrangement compiler turns this mutable model into immutable RT
/// snapshots; the model itself never touches the audio callback.
///
/// Time model: edit-side positions are PPQ (pulses-per-quarter-note / musical
/// time). Sample/frame conversion happens only in the arrangement compiler via a runtime
/// transport::TempoMap built from the plain tempo data stored here. Clip source
/// offsets are therefore expressed in PPQ as well (see EditClip::source_offset_ppq).
///
/// Ids are stable and allocated by a deterministic monotonic counter on the
/// Project (no rand / no clock). Track, clip, and source ids share independent
/// counters but each is monotonic and never reused within a Project instance.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "transport/tempo_map.h"

namespace sonare::arrangement {

/// ABI / schema version of the arrangement project struct layout. Bumped when
/// the model.s field layout changes in a way that affects serialization.
inline constexpr uint32_t kProjectVersion = 1;

/// Stable identifiers. 0 means "unset/invalid".
using TrackId = uint32_t;
using ClipId = uint32_t;
using WarpRefId = uint32_t;

// ===========================================================================
// Warp maps
// ===========================================================================

/// A warp anchor mapping output/warped sample position to source sample
/// position. The arrangement layer stores this as plain value data; MIR/host
/// code owns any offline baking that turns it into rendered audio.
struct WarpAnchorRef {
  double warp_sample = 0.0;
  double source_sample = 0.0;

  bool operator==(const WarpAnchorRef& o) const noexcept {
    return warp_sample == o.warp_sample && source_sample == o.source_sample;
  }
  bool operator!=(const WarpAnchorRef& o) const noexcept { return !(*this == o); }
};

/// First-class project warp map referenced by EditClip::warp_ref_id.
struct WarpMapRef {
  WarpRefId id = 0;
  std::string name;
  std::vector<WarpAnchorRef> anchors;

  bool operator==(const WarpMapRef& o) const noexcept {
    return id == o.id && name == o.name && anchors == o.anchors;
  }
  bool operator!=(const WarpMapRef& o) const noexcept { return !(*this == o); }
};

// ===========================================================================
// EditClip
// ===========================================================================

/// Curve shape applied to a clip fade region.
enum class FadeCurve : uint32_t {
  kLinear = 0,
  kEqualPower = 1,
  kExponential = 2,
  kLogarithmic = 3,
};

/// Looping behaviour for a clip.
enum class LoopMode : uint32_t {
  kOff = 0,
  kLoop = 1,
};

/// A clip fade (in or out). Length is in PPQ. A zero length means no fade.
struct ClipFade {
  double length_ppq = 0.0;
  FadeCurve curve = FadeCurve::kLinear;
};

/// A placed clip on the timeline. The SAME struct/API is used for audio and
/// MIDI clips: the referenced ClipSource's kind decides interpretation. All
/// musical positions are PPQ.
struct EditClip {
  ClipId id = 0;
  TrackId track_id = 0;
  SourceId source_id = 0;

  /// Clip start on the timeline (PPQ).
  double start_ppq = 0.0;
  /// Clip length on the timeline (PPQ). Must be > 0.
  double length_ppq = 0.0;
  /// Offset into the source where playback begins (PPQ, musical time). Allows
  /// trimming the head of the source without moving the clip on the timeline.
  double source_offset_ppq = 0.0;

  /// Linear playback gain (1.0 = unity).
  float gain = 1.0f;

  ClipFade fade_in;
  ClipFade fade_out;

  LoopMode loop_mode = LoopMode::kOff;
  /// Loop length in PPQ. Used only when loop_mode == kLoop; must be > 0 then.
  double loop_length_ppq = 0.0;

  /// Optional warp reference id (0 = none). Reserved for warp markers; stored
  /// here so warp data can be attached without changing the project model.
  WarpRefId warp_ref_id = 0;

  /// End position on the timeline (PPQ).
  double end_ppq() const noexcept { return start_ppq + length_ppq; }
};

// ===========================================================================
// Track
// ===========================================================================

/// A track owns placed clips (clips reference it by id) and automation lanes.
/// CC lanes are also automation lanes (MIDI CC <-> automation).
struct Track {
  enum class Kind : uint32_t {
    kAudio = 0,
    kMidi = 1,
    kAux = 2,
  };

  TrackId id = 0;
  std::string name;
  Kind kind = Kind::kAudio;

  /// Link to the mixing::api::Scene Strip by its string id (Track <-> Strip
  /// binding). Empty means "not yet bound to a strip".
  std::string channel_strip_ref;

  /// Per-track automation lanes (value data). CC lanes live here too.
  std::vector<automation::AutomationLane> automation_lanes;

  /// Minimal routing info: an output target id/string (e.g. a bus id resolved by
  /// the arrangement compiler against the Scene). Empty means "default/main output".
  std::string output_target;

  /// MIDI destination id the compiler stamps onto every MidiClipSchedule built
  /// from clips on this track (resolved against a host DestinationTable: a host
  /// instrument node id or an external MIDI port). 0 = default/null destination.
  /// This is what makes multi-track MIDI route to per-track instruments rather
  /// than collapsing every clip onto destination 0. Audio tracks ignore it.
  uint32_t midi_destination_id = 0;
};

// ===========================================================================
// Markers
// ===========================================================================

/// Timeline marker with an OWNED name. Compilation to transport::Marker (which
/// stores a non-owning const char*) happens in the compiler, which guarantees the storage
/// lifetime; the model owns the string here.
struct ProjectMarker {
  double ppq = 0.0;
  uint32_t id = 0;
  std::string name;
};

// ===========================================================================
// Harmonic annotation (chord-symbol granularity)
// ===========================================================================
//
// The chord-symbol / key harmonic representation is finalized as the
// first-class HarmonicTimeline value type in harmonic_timeline.h (ChordQuality,
// KeyMode, ChordSymbol, KeySegment, HarmonicTimeline). ProjectAnnotation below
// derives from HarmonicTimeline so the harmony seam is shared verbatim between
// MIR analysis (thin) and assist generation (rich), while the annotation adds
// the non-harmonic streams (sections, onsets, tempo confidence).

/// A section/structure marker (e.g. "Verse", "Chorus") over a PPQ span.
struct SectionSegment {
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  std::string label;
};

/// An onset marker (candidate split point). Automatic splitting is issued as an
/// explicit command later; the model only stores the candidate position.
struct OnsetMarker {
  double ppq = 0.0;
  /// Detection confidence in [0, 1] (0 = unset).
  float confidence = 0.0f;
};

/// Project-wide annotation timeline. A SINGLE annotation object is stored on the
/// Project (not a vector of annotations): it aggregates the parallel annotation
/// streams (harmony, sections, onsets, tempo confidence). This keeps a single
/// source of truth.
///
/// The harmonic part (key + chord-symbol segments) is the first-class
/// HarmonicTimeline; ProjectAnnotation derives from it so the `keys` and
/// `chords` members are the SAME storage that MIR fills and that assist
/// modules read/write. The annotation adds the non-harmonic streams below.
struct ProjectAnnotation : HarmonicTimeline {
  /// Overall tempo-estimation confidence in [0, 1] (0 = unset).
  float tempo_confidence = 0.0f;
  /// Section/structure spans.
  std::vector<SectionSegment> sections;
  /// Onset candidate markers.
  std::vector<OnsetMarker> onsets;

  /// @brief Read-only view of just the harmonic (key + chord) timeline.
  const HarmonicTimeline& harmony() const noexcept { return *this; }
  /// @brief Mutable view of just the harmonic (key + chord) timeline.
  HarmonicTimeline& harmony() noexcept { return *this; }
};

// ===========================================================================
// Assist sidecar
// ===========================================================================

/// Namespaced opaque payload for round-tripping assist-module internal state.
/// The core NEVER interprets the payload: it stores and retrieves it by id only.
/// Used by assist modules and the project serializer for lossless round-trip.
struct AssistSidecar {
  /// Owning module identifier (namespacing key, e.g. "midi-sketch").
  std::string module_id;
  /// Module-owned schema version of the payload.
  uint32_t schema_version = 0;
  /// Opaque payload bytes (bytes or UTF-8 JSON; the core does not interpret).
  std::vector<uint8_t> payload;

  /// Optional target scope: a track id (0 = none) and/or a PPQ region.
  TrackId target_track_id = 0;
  /// Optional PPQ region [region_start_ppq, region_end_ppq). When
  /// region_end_ppq <= region_start_ppq the region is treated as unset.
  double region_start_ppq = 0.0;
  double region_end_ppq = 0.0;
};

// ===========================================================================
// Clip overlap policy
// ===========================================================================

/// Overlap policy for clips on the same track. The model DETECTS overlaps but
/// does not silently reject them: callers choose whether to allow them. The
/// default is to disallow (returns false from add_clip when an overlap exists),
/// which matches a single-lane-per-track audio editing model. MIDI/comp lanes
/// can opt into overlap via Project::set_allow_overlap(true).
enum class OverlapPolicy : uint32_t {
  kDisallow = 0,
  kAllow = 1,
};

// ===========================================================================
// Project
// ===========================================================================

/// The headless arrangement project: the single value-oriented model that holds
/// audio clips, MIDI clip sources, automation, MIR annotation, mixer topology,
/// and assist sidecars. Control-thread only; no internal locking.
class Project {
 public:
  Project() = default;

  // ---- Global properties --------------------------------------------------

  double sample_rate() const noexcept { return sample_rate_; }
  void set_sample_rate(double sr) noexcept { sample_rate_ = sr; }

  uint32_t project_version() const noexcept { return project_version_; }

  OverlapPolicy overlap_policy() const noexcept { return overlap_policy_; }
  void set_overlap_policy(OverlapPolicy policy) noexcept { overlap_policy_ = policy; }

  // ---- Tempo / time-signature (plain value data) --------------------------
  // The model stores plain segments; the compiler builds a runtime TempoMap.

  const std::vector<transport::TempoSegment>& tempo_segments() const noexcept {
    return tempo_segments_;
  }
  void set_tempo_segments(std::vector<transport::TempoSegment> segments) {
    tempo_segments_ = std::move(segments);
  }

  const std::vector<transport::TimeSignatureSegment>& time_signatures() const noexcept {
    return time_signatures_;
  }
  void set_time_signatures(std::vector<transport::TimeSignatureSegment> sigs) {
    time_signatures_ = std::move(sigs);
  }

  // ---- Source registry -----------------------------------------------------

  /// Registers an audio source and returns its stable id (allocated here).
  SourceId add_audio_source(AudioSourceRef ref);
  /// Registers a MIDI source and returns its stable id (allocated here).
  SourceId add_midi_source(MidiSourceRef ref);

  const std::vector<ClipSource>& sources() const noexcept { return sources_; }
  /// Returns the source with the given id, or nullptr if absent.
  const ClipSource* find_source(SourceId id) const noexcept;
  bool has_source(SourceId id) const noexcept { return find_source(id) != nullptr; }

  // ---- Tracks --------------------------------------------------------------

  /// Adds a track (id is allocated here, overriding any id on the argument) and
  /// returns its stable id.
  TrackId add_track(Track track);

  const std::vector<Track>& tracks() const noexcept { return tracks_; }
  const Track* find_track(TrackId id) const noexcept;
  Track* find_track_mutable(TrackId id) noexcept;
  bool has_track(TrackId id) const noexcept { return find_track(id) != nullptr; }

  // ---- Clips ---------------------------------------------------------------

  /// Adds a clip after validating invariants. Returns the allocated clip id, or
  /// 0 on failure. Validation:
  ///  - clip.track_id references an existing track,
  ///  - clip.source_id references an existing source,
  ///  - length_ppq > 0 and start_ppq >= 0 and source_offset_ppq >= 0,
  ///  - loop_length_ppq > 0 when loop_mode == kLoop,
  ///  - no overlap with an existing clip on the same track when the overlap
  ///    policy is kDisallow.
  /// The id field of the argument is ignored; a fresh id is allocated.
  ClipId add_clip(EditClip clip);

  const std::vector<EditClip>& clips() const noexcept { return clips_; }
  const EditClip* find_clip(ClipId id) const noexcept;
  EditClip* find_clip_mutable(ClipId id) noexcept;
  bool has_clip(ClipId id) const noexcept { return find_clip(id) != nullptr; }

  /// Returns true if [start_ppq, start_ppq+length_ppq) on `track_id` overlaps an
  /// existing clip on that track. `ignore_clip_id` is excluded (use 0 to check
  /// all). Adjacency (touching endpoints) does NOT count as overlap.
  bool clip_overlaps(TrackId track_id, double start_ppq, double length_ppq,
                     ClipId ignore_clip_id = 0) const noexcept;

  // ---- Markers -------------------------------------------------------------

  /// Adds a marker (id is allocated here) and returns its stable id.
  uint32_t add_marker(double ppq, std::string name);
  const std::vector<ProjectMarker>& markers() const noexcept { return markers_; }

  // ---- Annotation ----------------------------------------------------------

  const ProjectAnnotation& annotation() const noexcept { return annotation_; }
  ProjectAnnotation& annotation() noexcept { return annotation_; }

  // ---- Mixer scene (pure data; held even when mixing runtime is disabled) --

  const mixing::api::Scene& scene() const noexcept { return scene_; }
  mixing::api::Scene& scene() noexcept { return scene_; }

  // ---- Assist sidecars (opaque) --------------------------------------------

  void add_assist_sidecar(AssistSidecar sidecar) { assist_sidecars_.push_back(std::move(sidecar)); }
  const std::vector<AssistSidecar>& assist_sidecars() const noexcept { return assist_sidecars_; }

  // ---- Warp maps ------------------------------------------------------------

  const std::vector<WarpMapRef>& warp_maps() const noexcept { return warp_maps_; }
  const WarpMapRef* find_warp_map(WarpRefId id) const noexcept;
  WarpMapRef* find_warp_map_mutable(WarpRefId id) noexcept;
  bool has_warp_map(WarpRefId id) const noexcept { return find_warp_map(id) != nullptr; }

  // ---- Low-level mutation helpers (used by EditCommand; not for general use) -
  //
  // The arrangement subsystem routes ALL public mutation through EditCommand
  // (internal). These helpers are the internal mechanism commands call to mutate the
  // model deterministically; callers outside the command layer should prefer
  // commands so that undo/redo, serialization, and deterministic replay stay
  // uniform. They perform no I/O and no validation beyond what is documented.

  /// Removes the track with `id`. Returns the removed Track (with its id) and
  /// true on success; on failure returns a default Track and false. Does NOT
  /// touch clips referencing the track (the command layer removes clips first).
  std::pair<Track, bool> remove_track(TrackId id);

  /// Removes the clip with `id`. Returns the removed clip and true on success.
  std::pair<EditClip, bool> remove_clip(ClipId id);

  /// Inserts a clip verbatim, preserving its id and bypassing id allocation and
  /// overlap validation. Used by invert() to restore a previously removed clip
  /// or a pre-mutation clip snapshot. Returns false if a clip with the same id
  /// already exists. `index` is the position to insert at (clamped to the end);
  /// restoring a removed clip at its original index keeps clip ordering stable
  /// across undo/redo so deep equality round-trips. Use the default (npos) to
  /// append.
  static constexpr size_t kAppend = static_cast<size_t>(-1);
  bool insert_clip_raw(EditClip clip, size_t index = kAppend);

  /// Inserts a track verbatim, preserving its id and bypassing id allocation.
  /// Used by invert() to restore a removed track. Returns false if a track with
  /// the same id already exists. `index` restores the original ordering (see
  /// insert_clip_raw).
  bool insert_track_raw(Track track, size_t index = kAppend);

  /// Returns the mutable source variant with `id`, or nullptr if absent. Used by
  /// ReplaceSource to swap a source ref in place (id is preserved by the caller).
  ClipSource* find_source_mutable(SourceId id) noexcept;

  /// Inserts a source variant verbatim, preserving its id and bypassing id
  /// allocation. Used by invert() to restore a previously registered source.
  /// Returns false if a source with the same id already exists. `index` restores
  /// the original ordering (see insert_clip_raw).
  bool insert_source_raw(ClipSource source, size_t index = kAppend);

  /// Returns the index of the track / clip / source with the given id, or kAppend
  /// when absent. Used by invert() to restore removed entries at their original
  /// position so undo/redo keeps ordering (and thus deep equality) stable.
  size_t track_index(TrackId id) const noexcept;
  size_t clip_index(ClipId id) const noexcept;
  size_t source_index(SourceId id) const noexcept;

  /// Removes the source with `id`. Returns the removed source and true on
  /// success. Does not touch clips referencing it (the command layer orders
  /// removals). On failure returns a default-constructed variant and false.
  std::pair<ClipSource, bool> remove_source(SourceId id);

  /// Sets the next id counters to at least the given values. invert() of an
  /// Add* command calls this so that a redo allocates the SAME id again, keeping
  /// ids stable across undo/redo round-trips.
  void ensure_next_source_id(SourceId id) noexcept;
  void ensure_next_track_id(TrackId id) noexcept;
  void ensure_next_clip_id(ClipId id) noexcept;
  void ensure_next_marker_id(uint32_t id) noexcept;

  SourceId next_source_id() const noexcept { return next_source_id_; }
  TrackId next_track_id() const noexcept { return next_track_id_; }
  ClipId next_clip_id() const noexcept { return next_clip_id_; }
  uint32_t next_marker_id() const noexcept { return next_marker_id_; }

  /// Mutable markers / sidecars / tempo for command use.
  std::vector<ProjectMarker>& markers_mutable() noexcept { return markers_; }
  std::vector<AssistSidecar>& assist_sidecars_mutable() noexcept { return assist_sidecars_; }
  std::vector<WarpMapRef>& warp_maps_mutable() noexcept { return warp_maps_; }
  std::vector<transport::TempoSegment>& tempo_segments_mutable() noexcept {
    return tempo_segments_;
  }

  /// Replaces or inserts a warp map by id. Returns false for id 0.
  bool set_warp_map(WarpMapRef map);
  /// Removes a warp map by id and returns the removed map plus success flag.
  std::pair<WarpMapRef, bool> remove_warp_map(WarpRefId id);

 private:
  double sample_rate_ = 22050.0;
  uint32_t project_version_ = kProjectVersion;
  OverlapPolicy overlap_policy_ = OverlapPolicy::kDisallow;

  std::vector<transport::TempoSegment> tempo_segments_;
  std::vector<transport::TimeSignatureSegment> time_signatures_;

  std::vector<ClipSource> sources_;
  std::vector<Track> tracks_;
  std::vector<EditClip> clips_;
  std::vector<ProjectMarker> markers_;
  ProjectAnnotation annotation_;
  mixing::api::Scene scene_;
  std::vector<AssistSidecar> assist_sidecars_;
  std::vector<WarpMapRef> warp_maps_;

  // Independent, monotonic, never-reused id counters (deterministic; no rand).
  SourceId next_source_id_ = 1;
  TrackId next_track_id_ = 1;
  ClipId next_clip_id_ = 1;
  uint32_t next_marker_id_ = 1;
};

}  // namespace sonare::arrangement
