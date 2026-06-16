#pragma once

/// @file edit_command.h
/// @brief Deterministic, invertible edit commands for the arrangement model.
///
/// Every mutation of a @ref sonare::arrangement::Project goes through an
/// @ref sonare::arrangement::EditCommand. Commands are CONTROL-THREAD-ONLY and
/// are NEVER invoked from the realtime audio callback. They perform no I/O, no
/// clock/random access, and no grid snapping (snap/quantize happens in a later
/// phase before a command is constructed): construction + apply + invert are
/// deterministic and value-oriented.
///
/// Apply / invert contract
/// -----------------------
/// `apply(Project&)` mutates the project and returns true on success. On the
/// FIRST successful apply a command snapshots the prior value(s) it needs to
/// undo itself, so that `invert(before)` can produce a command that returns the
/// project to the pre-apply state. The round-trip is:
///
///   auto inverse = cmd.invert(project_before_apply);
///   cmd.apply(project);      // project -> after
///   inverse->apply(project); // project -> before  (deep-equal on affected fields)
///
/// `invert(const Project& before, ...)` is called by @ref EditHistory AFTER a
/// successful apply, and is passed the project state captured BEFORE apply. It
/// reads the captured prior value(s) from `before` (e.g. the prior gain, the
/// removed track) and any apply-time results recorded on the command itself
/// (e.g. an Add*'s allocated id) to build the inverse command. Add/Remove are
/// mutual inverses; "Set*" style commands invert to a "Set*" that restores the
/// captured prior value. Add* commands invert to a Remove* AND restore the id
/// counter (via Project::ensure_next_*_id) so that a subsequent redo re-allocates
/// the SAME stable id.
///
/// MIDI event payload seam
/// -----------------------
/// MIDI content commands operate on @ref MidiClipEventList: PPQ-positioned POD
/// values that carry the first two UMP words for channel-voice messages. The
/// commands store the payload by value and the model owns it opaquely, so richer
/// MIDI storage can be introduced later without changing
/// ReplaceMidiClipEvents / PatchMidiClip signatures. The model keeps per-clip
/// event lists in a side map on the command-managed store so the EditClip
/// struct identity is unchanged.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "automation/automation_lane.h"
#include "transport/tempo_map.h"

namespace sonare::arrangement {

// ===========================================================================
// MIDI event POD seam
// ===========================================================================

/// Minimal, forward-compatible MIDI event POD. Positions are PPQ (musical time)
/// and data words are UMP words for valid channel-voice or data/SysEx-handle
/// events.
struct MidiClipEvent {
  /// Event position within the clip's source timeline (PPQ).
  double ppq = 0.0;
  /// First two UMP words. MIDI 1.0 channel-voice events use data0 and keep
  /// data1 at zero; MIDI 2.0 channel-voice events use both words.
  uint32_t data0 = 0;
  uint32_t data1 = 0;
  /// Non-zero for data/SysEx-handle events. Payload bytes live in
  /// MidiContentStore::sysex_payloads so command/event lists stay fixed-size.
  uint32_t sysex_handle = 0;

  bool operator==(const MidiClipEvent& o) const noexcept {
    return ppq == o.ppq && data0 == o.data0 && data1 == o.data1 && sysex_handle == o.sysex_handle;
  }
  bool operator!=(const MidiClipEvent& o) const noexcept { return !(*this == o); }
};

/// A clip's MIDI event payload. Owned value data.
using MidiClipEventList = std::vector<MidiClipEvent>;

/// Side store of per-clip MIDI event lists, keyed by clip id. This lives outside
/// the EditClip struct so the project model identity is unchanged; MIDI content
/// commands mutate it. The store is plain value data on the command layer; MIDI support
/// will fold this into the compiled MIDI schedule. It is attached to a Project
/// via the free functions below so the model header stays compact.
struct MidiContentStore {
  std::map<ClipId, MidiClipEventList> events;
  std::map<uint32_t, std::vector<uint8_t>> sysex_payloads;

  bool operator==(const MidiContentStore& o) const {
    return events == o.events && sysex_payloads == o.sysex_payloads;
  }
};

// ===========================================================================
// EditCommand interface
// ===========================================================================

/// Base class for all deterministic, invertible edit commands. Control-thread
/// only; never called from RT.
class EditCommand {
 public:
  virtual ~EditCommand() = default;

  /// Applies the mutation to `project` (and, for MIDI content commands, to the
  /// associated content store carried by the project wrapper). Returns true on
  /// success. On the first successful apply, the command snapshots whatever it
  /// needs to be inverted via invert().
  virtual bool apply(Project& project, MidiContentStore& store) = 0;

  /// Returns a command that, when applied, undoes this command's effect. The
  /// caller passes the project state BEFORE this command was applied so the
  /// inverse can capture the prior value. Returns nullptr only for commands that
  /// were not applicable (never expected in normal undo flow).
  virtual std::unique_ptr<EditCommand> invert(const Project& before,
                                              const MidiContentStore& store_before) const = 0;

  /// Stable command type tag (for diagnostics / serialization later).
  virtual const char* type_name() const noexcept = 0;
};

using EditCommandPtr = std::unique_ptr<EditCommand>;

// ===========================================================================
// Track commands
// ===========================================================================

class AddTrack final : public EditCommand {
 public:
  /// `track` carries the desired fields; its id is allocated on apply (or the
  /// pre-set id is honored when restoring via redo, see allocated_id()).
  explicit AddTrack(Track track) : track_(std::move(track)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "AddTrack"; }

  TrackId allocated_id() const noexcept { return allocated_id_; }

  /// Pre-seeds the id this command will restore (used when an inverse RemoveTrack
  /// rebuilds an AddTrack to bring a removed track back with its original id).
  void reseed_id(TrackId id) noexcept { allocated_id_ = id; }
  /// Pre-seeds the position at which a reseeded track is restored, so undoing a
  /// RemoveTrack keeps track ordering (and thus deep equality) stable.
  void reseed_index(size_t index) noexcept { restore_index_ = index; }

 private:
  Track track_;
  TrackId allocated_id_ = 0;
  size_t restore_index_ = Project::kAppend;
};

class RemoveTrack final : public EditCommand {
 public:
  explicit RemoveTrack(TrackId id) : id_(id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveTrack"; }

 private:
  TrackId id_;
};

class RenameTrack final : public EditCommand {
 public:
  RenameTrack(TrackId id, std::string name) : id_(id), name_(std::move(name)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RenameTrack"; }

 private:
  TrackId id_;
  std::string name_;
};

class SetTrackRoute final : public EditCommand {
 public:
  SetTrackRoute(TrackId id, std::string channel_strip_ref, std::string output_target)
      : id_(id),
        channel_strip_ref_(std::move(channel_strip_ref)),
        output_target_(std::move(output_target)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetTrackRoute"; }

 private:
  TrackId id_;
  std::string channel_strip_ref_;
  std::string output_target_;
};

class SetTrackKind final : public EditCommand {
 public:
  SetTrackKind(TrackId id, Track::Kind kind) : id_(id), kind_(kind) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetTrackKind"; }

 private:
  TrackId id_;
  Track::Kind kind_;
};

/// Sets a track's MIDI destination id (the route the compiler stamps onto the
/// track's MidiClipSchedules). Deterministic, undoable.
class SetTrackMidiDestination final : public EditCommand {
 public:
  SetTrackMidiDestination(TrackId id, uint32_t destination_id)
      : id_(id), destination_id_(destination_id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetTrackMidiDestination"; }

 private:
  TrackId id_;
  uint32_t destination_id_;
};

// ===========================================================================
// Clip commands
// ===========================================================================

class AddClip final : public EditCommand {
 public:
  explicit AddClip(EditClip clip) : clip_(std::move(clip)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "AddClip"; }

  ClipId allocated_id() const noexcept { return allocated_id_; }

  /// Pre-seeds the id this command restores (used by RemoveClip::invert to bring
  /// a removed clip back with its original id).
  void reseed_id(ClipId id) noexcept { allocated_id_ = id; }
  /// Pre-seeds the position at which a reseeded clip is restored, so undoing a
  /// RemoveClip keeps clip ordering (and thus deep equality) stable.
  void reseed_index(size_t index) noexcept { restore_index_ = index; }
  /// Pre-seeds MIDI content to restore alongside the clip (used by
  /// RemoveClip::invert so undoing a clip removal restores its events too).
  void set_restore_events(MidiClipEventList events) {
    restore_events_ = std::move(events);
    has_restore_events_ = true;
  }
  void set_restore_sysex_payloads(std::map<uint32_t, std::vector<uint8_t>> payloads) {
    restore_sysex_payloads_ = std::move(payloads);
  }

 private:
  EditClip clip_;
  ClipId allocated_id_ = 0;
  size_t restore_index_ = Project::kAppend;
  MidiClipEventList restore_events_;
  std::map<uint32_t, std::vector<uint8_t>> restore_sysex_payloads_;
  bool has_restore_events_ = false;
};

class RemoveClip final : public EditCommand {
 public:
  explicit RemoveClip(ClipId id) : id_(id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveClip"; }

 private:
  ClipId id_;
};

/// Splits a clip at `split_ppq` (absolute timeline PPQ) into two clips. The
/// original clip is shortened to end at the split; a new clip continues from the
/// split with adjusted source offset. The new clip's id is allocated on apply.
class SplitClip final : public EditCommand {
 public:
  SplitClip(ClipId id, double split_ppq) : id_(id), split_ppq_(split_ppq) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SplitClip"; }

  ClipId new_clip_id() const noexcept { return new_clip_id_; }

  /// Pre-seeds the right-clip id (used when re-doing/inverting so the recreated
  /// right clip keeps its original id).
  void reseed_new_clip_id(ClipId id) noexcept { new_clip_id_ = id; }

 private:
  ClipId id_;
  double split_ppq_;
  ClipId new_clip_id_ = 0;
};

/// Trims a clip's start and/or length. `new_start_ppq` moves the timeline start
/// and shifts the source offset by the same delta; `new_length_ppq` sets length.
class TrimClip final : public EditCommand {
 public:
  TrimClip(ClipId id, double new_start_ppq, double new_length_ppq)
      : id_(id), new_start_ppq_(new_start_ppq), new_length_ppq_(new_length_ppq) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "TrimClip"; }

 private:
  ClipId id_;
  double new_start_ppq_;
  double new_length_ppq_;
};

/// Moves a clip to a new start position, optionally to a different track.
class MoveClip final : public EditCommand {
 public:
  MoveClip(ClipId id, double new_start_ppq, TrackId new_track_id = 0)
      : id_(id), new_start_ppq_(new_start_ppq), new_track_id_(new_track_id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "MoveClip"; }

 private:
  ClipId id_;
  double new_start_ppq_;
  TrackId new_track_id_;  // 0 = keep current track
};

/// Duplicates a clip at `new_start_ppq` (same track), allocating a fresh id and
/// copying any MIDI content for the source clip.
class DuplicateClip final : public EditCommand {
 public:
  DuplicateClip(ClipId id, double new_start_ppq) : id_(id), new_start_ppq_(new_start_ppq) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "DuplicateClip"; }

  ClipId new_clip_id() const noexcept { return new_clip_id_; }

 private:
  ClipId id_;
  double new_start_ppq_;
  ClipId new_clip_id_ = 0;
};

class SetClipGain final : public EditCommand {
 public:
  SetClipGain(ClipId id, float gain) : id_(id), gain_(gain) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipGain"; }

 private:
  ClipId id_;
  float gain_;
};

class SetClipFade final : public EditCommand {
 public:
  SetClipFade(ClipId id, ClipFade fade_in, ClipFade fade_out)
      : id_(id), fade_in_(fade_in), fade_out_(fade_out) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipFade"; }

 private:
  ClipId id_;
  ClipFade fade_in_;
  ClipFade fade_out_;
};

class SetClipLoop final : public EditCommand {
 public:
  SetClipLoop(ClipId id, LoopMode mode, double loop_length_ppq)
      : id_(id), mode_(mode), loop_length_ppq_(loop_length_ppq) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipLoop"; }

 private:
  ClipId id_;
  LoopMode mode_;
  double loop_length_ppq_;
};

class SetClipWarpRef final : public EditCommand {
 public:
  SetClipWarpRef(ClipId id, WarpRefId warp_ref_id) : id_(id), warp_ref_id_(warp_ref_id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipWarpRef"; }

 private:
  ClipId id_;
  WarpRefId warp_ref_id_;
};

class SetClipWarpMode final : public EditCommand {
 public:
  SetClipWarpMode(ClipId id, WarpMode mode) : id_(id), mode_(mode) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipWarpMode"; }

 private:
  ClipId id_;
  WarpMode mode_;
};

class SetClipTakes final : public EditCommand {
 public:
  SetClipTakes(ClipId id, std::vector<ClipTake> takes, TakeId active_take_id)
      : id_(id), takes_(std::move(takes)), active_take_id_(active_take_id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipTakes"; }

 private:
  ClipId id_;
  std::vector<ClipTake> takes_;
  TakeId active_take_id_;
};

class SetClipCompSegments final : public EditCommand {
 public:
  SetClipCompSegments(ClipId id, std::vector<ClipCompSegment> segments)
      : id_(id), segments_(std::move(segments)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipCompSegments"; }

 private:
  ClipId id_;
  std::vector<ClipCompSegment> segments_;
};

class SetWarpMap final : public EditCommand {
 public:
  explicit SetWarpMap(WarpMapRef map) : map_(std::move(map)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetWarpMap"; }

 private:
  WarpMapRef map_;
};

class RemoveWarpMap final : public EditCommand {
 public:
  explicit RemoveWarpMap(WarpRefId id) : id_(id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveWarpMap"; }

 private:
  WarpRefId id_;
};

class SetClipSource final : public EditCommand {
 public:
  SetClipSource(ClipId id, SourceId source_id) : id_(id), source_id_(source_id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetClipSource"; }

 private:
  ClipId id_;
  SourceId source_id_;
};

// ===========================================================================
// Source commands
// ===========================================================================

class AttachAudioSource final : public EditCommand {
 public:
  explicit AttachAudioSource(AudioSourceRef ref) : ref_(std::move(ref)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "AttachAudioSource"; }

  SourceId allocated_id() const noexcept { return allocated_id_; }

  /// Pre-seeds the id this command restores (used by RemoveSourceInternal::invert
  /// to re-attach a removed source with its original stable id on undo).
  void reseed_id(SourceId id) noexcept { allocated_id_ = id; }
  /// Pre-seeds the position at which a reseeded source is restored, so undoing a
  /// source removal keeps source ordering (and thus deep equality) stable.
  void reseed_index(size_t index) noexcept { restore_index_ = index; }

 private:
  AudioSourceRef ref_;
  SourceId allocated_id_ = 0;
  size_t restore_index_ = Project::kAppend;
};

class AttachMidiSource final : public EditCommand {
 public:
  explicit AttachMidiSource(MidiSourceRef ref) : ref_(std::move(ref)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "AttachMidiSource"; }

  SourceId allocated_id() const noexcept { return allocated_id_; }

  /// Pre-seeds the id this command restores (used by RemoveSourceInternal::invert
  /// to re-attach a removed source with its original stable id on undo).
  void reseed_id(SourceId id) noexcept { allocated_id_ = id; }
  /// Pre-seeds the position at which a reseeded source is restored, so undoing a
  /// source removal keeps source ordering (and thus deep equality) stable.
  void reseed_index(size_t index) noexcept { restore_index_ = index; }

 private:
  MidiSourceRef ref_;
  SourceId allocated_id_ = 0;
  size_t restore_index_ = Project::kAppend;
};

/// Replaces the source variant of an existing source id in place (id preserved).
class ReplaceSource final : public EditCommand {
 public:
  ReplaceSource(SourceId id, ClipSource replacement)
      : id_(id), replacement_(std::move(replacement)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "ReplaceSource"; }

 private:
  SourceId id_;
  ClipSource replacement_;
};

// ===========================================================================
// Timeline commands
// ===========================================================================

/// Sets the project sample rate. Invalid rates are rejected so undo/redo does
/// not admit non-compileable project state.
class SetSampleRate final : public EditCommand {
 public:
  explicit SetSampleRate(double sample_rate) : sample_rate_(sample_rate) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetSampleRate"; }

 private:
  double sample_rate_;
};

/// Sets the project's clip-overlap policy.
class SetOverlapPolicy final : public EditCommand {
 public:
  explicit SetOverlapPolicy(OverlapPolicy policy) : policy_(policy) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetOverlapPolicy"; }

 private:
  OverlapPolicy policy_;
};

/// Replaces the project's pure-data mixer scene.
class SetScene final : public EditCommand {
 public:
  explicit SetScene(mixing::api::Scene scene) : scene_(std::move(scene)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetScene"; }

 private:
  mixing::api::Scene scene_;
};

/// Adds or replaces a marker. When `id` is 0 a new marker is allocated; when
/// `id` references an existing marker its ppq/name are replaced.
class SetMarker final : public EditCommand {
 public:
  SetMarker(uint32_t id, double ppq, std::string name, uint8_t kind = 0, int8_t key_fifths = 0,
            bool key_minor = false)
      : id_(id),
        ppq_(ppq),
        name_(std::move(name)),
        kind_(kind),
        key_fifths_(key_fifths),
        key_minor_(key_minor) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetMarker"; }

  uint32_t allocated_id() const noexcept { return allocated_id_; }

 private:
  uint32_t id_;
  double ppq_;
  std::string name_;
  uint8_t kind_;
  int8_t key_fifths_;
  bool key_minor_;
  uint32_t allocated_id_ = 0;
};

/// Replaces the whole project annotation block (key/chord/section/onset streams
/// and tempo confidence). Coarse-grained but exact and trivially invertible.
class SetAnnotation final : public EditCommand {
 public:
  explicit SetAnnotation(ProjectAnnotation annotation) : annotation_(std::move(annotation)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetAnnotation"; }

 private:
  ProjectAnnotation annotation_;
};

/// Replaces the project's tempo segment list (plain value data; the compiler
/// builds a runtime TempoMap from it).
class SetTempoSegment final : public EditCommand {
 public:
  explicit SetTempoSegment(std::vector<transport::TempoSegment> segments)
      : segments_(std::move(segments)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetTempoSegment"; }

 private:
  std::vector<transport::TempoSegment> segments_;
};

/// Replaces the project's time-signature segment list.
class SetTimeSignatureSegment final : public EditCommand {
 public:
  explicit SetTimeSignatureSegment(std::vector<transport::TimeSignatureSegment> segments)
      : segments_(std::move(segments)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetTimeSignatureSegment"; }

 private:
  std::vector<transport::TimeSignatureSegment> segments_;
};

/// Replaces the harmony (chord) progression in the project annotation.
class SetHarmonySegment final : public EditCommand {
 public:
  explicit SetHarmonySegment(std::vector<ChordSymbol> chords) : chords_(std::move(chords)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetHarmonySegment"; }

 private:
  std::vector<ChordSymbol> chords_;
};

// ===========================================================================
// Automation commands (CC lanes are automation lanes)
// ===========================================================================

class AddAutomationLane final : public EditCommand {
 public:
  AddAutomationLane(TrackId track_id, automation::AutomationLane lane)
      : track_id_(track_id), lane_(std::move(lane)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "AddAutomationLane"; }

  /// Index of the appended lane within the track (valid after apply).
  size_t lane_index() const noexcept { return lane_index_; }

 private:
  TrackId track_id_;
  automation::AutomationLane lane_;
  size_t lane_index_ = 0;
};

class RemoveAutomationLane final : public EditCommand {
 public:
  RemoveAutomationLane(TrackId track_id, size_t lane_index)
      : track_id_(track_id), lane_index_(lane_index) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveAutomationLane"; }

 private:
  TrackId track_id_;
  size_t lane_index_;
};

/// Replaces an existing automation lane (target param id + breakpoints) in place.
class EditAutomationLane final : public EditCommand {
 public:
  EditAutomationLane(TrackId track_id, size_t lane_index, automation::AutomationLane lane)
      : track_id_(track_id), lane_index_(lane_index), lane_(std::move(lane)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "EditAutomationLane"; }

 private:
  TrackId track_id_;
  size_t lane_index_;
  automation::AutomationLane lane_;
};

// ===========================================================================
// MIDI content commands (POD payload seam; see file header)
// ===========================================================================

/// Replaces a clip's entire MIDI event list in the content store.
class ReplaceMidiClipEvents final : public EditCommand {
 public:
  ReplaceMidiClipEvents(ClipId clip_id, MidiClipEventList events)
      : clip_id_(clip_id), events_(std::move(events)) {}
  ReplaceMidiClipEvents(ClipId clip_id, MidiClipEventList events,
                        std::map<uint32_t, std::vector<uint8_t>> sysex_payloads)
      : clip_id_(clip_id), events_(std::move(events)), sysex_payloads_(std::move(sysex_payloads)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "ReplaceMidiClipEvents"; }

 private:
  ClipId clip_id_;
  MidiClipEventList events_;
  std::map<uint32_t, std::vector<uint8_t>> sysex_payloads_;
};

/// A minimal patch description for MIDI content. Represents inserting and
/// removing events on a clip. The patch payload may be extended without changing
/// this command's identity (the patch is applied against MidiClipEventList).
struct MidiClipPatch {
  ClipId clip_id = 0;
  /// Events to append/insert.
  MidiClipEventList add;
  /// Events to remove (matched by value equality).
  MidiClipEventList remove;
};

/// Applies a @ref MidiClipPatch (add/remove events) as a command, so that
/// assist-generated patches are uniformly undoable/serializable.
class PatchMidiClip final : public EditCommand {
 public:
  explicit PatchMidiClip(MidiClipPatch patch) : patch_(std::move(patch)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "PatchMidiClip"; }

 private:
  MidiClipPatch patch_;
};

// ===========================================================================
// Assist command
// ===========================================================================

/// Adds or updates an assist sidecar entry. Sidecars are matched by module_id
/// plus target scope (track id + region). When a matching entry exists it is
/// replaced; otherwise the sidecar is appended.
class SetAssistSidecar final : public EditCommand {
 public:
  explicit SetAssistSidecar(AssistSidecar sidecar) : sidecar_(std::move(sidecar)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "SetAssistSidecar"; }

 private:
  AssistSidecar sidecar_;
};

// ===========================================================================
// Internal inverse-only commands
// ===========================================================================
//
// These commands are produced ONLY by invert() to express the precise inverse
// of an Add/Set that has no direct public counterpart (e.g. removing a source by
// id, removing a marker by id, re-inserting an automation lane at a fixed index,
// undoing a split). They are full EditCommands (deterministic + invertible) so
// they participate uniformly in undo/redo; callers normally use the public
// commands above.

/// Removes a source by id (inverse of Attach*Source's first apply).
class RemoveSourceInternal final : public EditCommand {
 public:
  explicit RemoveSourceInternal(SourceId id) : id_(id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveSourceInternal"; }

 private:
  SourceId id_;
};

/// Removes a marker by id (inverse of a SetMarker that created a new marker).
class RemoveMarkerInternal final : public EditCommand {
 public:
  explicit RemoveMarkerInternal(uint32_t id) : id_(id) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveMarkerInternal"; }

 private:
  uint32_t id_;
};

/// Inserts an automation lane at a fixed index (inverse of RemoveAutomationLane).
class InsertAutomationLane final : public EditCommand {
 public:
  InsertAutomationLane(TrackId track_id, size_t lane_index, automation::AutomationLane lane)
      : track_id_(track_id), lane_index_(lane_index), lane_(std::move(lane)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "InsertAutomationLane"; }

 private:
  TrackId track_id_;
  size_t lane_index_;
  automation::AutomationLane lane_;
};

/// Removes an assist sidecar matched by module_id + target scope (inverse of a
/// SetAssistSidecar that added a new entry).
class RemoveAssistSidecarInternal final : public EditCommand {
 public:
  explicit RemoveAssistSidecarInternal(AssistSidecar key) : key_(std::move(key)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "RemoveAssistSidecarInternal"; }

 private:
  AssistSidecar key_;
};

/// Undoes a SplitClip: removes the new right clip and restores the original
/// clip's length and fades (inverse of SplitClip). The `original` is the clip as
/// it was before the split; `split_ppq` is the original cut position so the
/// inverse can reproduce the exact split.
class UnsplitClip final : public EditCommand {
 public:
  UnsplitClip(ClipId original_id, ClipId new_clip_id, double split_ppq, EditClip original)
      : original_id_(original_id),
        new_clip_id_(new_clip_id),
        split_ppq_(split_ppq),
        original_(std::move(original)) {}

  bool apply(Project& project, MidiContentStore& store) override;
  EditCommandPtr invert(const Project& before, const MidiContentStore& store_before) const override;
  const char* type_name() const noexcept override { return "UnsplitClip"; }

 private:
  ClipId original_id_;
  ClipId new_clip_id_;
  double split_ppq_;
  EditClip original_;
};

}  // namespace sonare::arrangement
