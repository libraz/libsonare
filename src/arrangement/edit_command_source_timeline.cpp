/// @file edit_command_source_timeline.cpp
/// @brief Source, timeline, and automation edit-command apply/invert definitions.

#include <cmath>
#include <utility>

#include "arrangement/edit_command.h"

namespace sonare::arrangement {

// ===========================================================================
// Source commands
// ===========================================================================

bool AttachAudioSource::apply(Project& project, MidiContentStore& /*store*/) {
  if (allocated_id_ != 0) {
    AudioSourceRef ref = ref_;
    ref.id = allocated_id_;
    if (!project.insert_source_raw(ClipSource{ref}, restore_index_)) {
      return false;
    }
    project.ensure_next_source_id(allocated_id_);
    return true;
  }
  allocated_id_ = project.add_audio_source(ref_);
  return allocated_id_ != 0;
}

EditCommandPtr AttachAudioSource::invert(const Project& /*before*/,
                                         const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveSourceInternal>(allocated_id_);
}

bool AttachMidiSource::apply(Project& project, MidiContentStore& /*store*/) {
  if (allocated_id_ != 0) {
    MidiSourceRef ref = ref_;
    ref.id = allocated_id_;
    if (!project.insert_source_raw(ClipSource{ref}, restore_index_)) {
      return false;
    }
    project.ensure_next_source_id(allocated_id_);
    return true;
  }
  allocated_id_ = project.add_midi_source(ref_);
  return allocated_id_ != 0;
}

EditCommandPtr AttachMidiSource::invert(const Project& /*before*/,
                                        const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveSourceInternal>(allocated_id_);
}

bool ReplaceSource::apply(Project& project, MidiContentStore& /*store*/) {
  ClipSource* slot = project.find_source_mutable(id_);
  if (slot == nullptr) {
    return false;
  }
  ClipSource repl = replacement_;
  // The id is preserved regardless of the replacement's embedded id.
  if (auto* a = std::get_if<AudioSourceRef>(&repl)) {
    a->id = id_;
  } else if (auto* m = std::get_if<MidiSourceRef>(&repl)) {
    m->id = id_;
  }
  *slot = std::move(repl);
  return true;
}

EditCommandPtr ReplaceSource::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const ClipSource* prior = before.find_source(id_);
  if (prior == nullptr) {
    return nullptr;
  }
  return std::make_unique<ReplaceSource>(id_, *prior);
}

// ===========================================================================
// Timeline commands
// ===========================================================================

bool SetSampleRate::apply(Project& project, MidiContentStore& /*store*/) {
  if (!std::isfinite(sample_rate_) || !(sample_rate_ > 0.0)) {
    return false;
  }
  project.set_sample_rate(sample_rate_);
  return true;
}

EditCommandPtr SetSampleRate::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetSampleRate>(before.sample_rate());
}

bool SetOverlapPolicy::apply(Project& project, MidiContentStore& /*store*/) {
  project.set_overlap_policy(policy_);
  return true;
}

EditCommandPtr SetOverlapPolicy::invert(const Project& before,
                                        const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetOverlapPolicy>(before.overlap_policy());
}

bool SetScene::apply(Project& project, MidiContentStore& /*store*/) {
  project.scene() = scene_;
  return true;
}

EditCommandPtr SetScene::invert(const Project& before,
                                const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetScene>(before.scene());
}

bool SetMarker::apply(Project& project, MidiContentStore& /*store*/) {
  if (id_ == 0 && allocated_id_ == 0) {
    allocated_id_ = project.add_marker(ppq_, name_, kind_, key_fifths_, key_minor_);
    return allocated_id_ != 0;
  }
  const uint32_t target = id_ != 0 ? id_ : allocated_id_;
  for (ProjectMarker& m : project.markers_mutable()) {
    if (m.id == target) {
      m.ppq = ppq_;
      m.name = name_;
      m.kind = kind_;
      m.key_fifths = key_fifths_;
      m.key_minor = key_minor_;
      return true;
    }
  }
  // Marker does not exist (e.g. redo after the original add was undone): restore
  // it with its fixed id.
  ProjectMarker m;
  m.id = target;
  m.ppq = ppq_;
  m.name = name_;
  m.kind = kind_;
  m.key_fifths = key_fifths_;
  m.key_minor = key_minor_;
  project.markers_mutable().push_back(std::move(m));
  project.ensure_next_marker_id(target);
  return true;
}

EditCommandPtr SetMarker::invert(const Project& before,
                                 const MidiContentStore& /*store_before*/) const {
  const uint32_t target = id_ != 0 ? id_ : allocated_id_;
  for (const ProjectMarker& m : before.markers()) {
    if (m.id == target) {
      // Marker existed before: restore prior ppq/name/kind.
      return std::make_unique<SetMarker>(target, m.ppq, m.name, m.kind, m.key_fifths, m.key_minor);
    }
  }
  // Marker was newly created: inverse removes it.
  return std::make_unique<RemoveMarkerInternal>(target);
}

bool SetAnnotation::apply(Project& project, MidiContentStore& /*store*/) {
  project.annotation() = annotation_;
  return true;
}

EditCommandPtr SetAnnotation::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetAnnotation>(before.annotation());
}

bool SetTempoSegment::apply(Project& project, MidiContentStore& /*store*/) {
  project.set_tempo_segments(segments_);
  return true;
}

EditCommandPtr SetTempoSegment::invert(const Project& before,
                                       const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetTempoSegment>(before.tempo_segments());
}

bool SetTimeSignatureSegment::apply(Project& project, MidiContentStore& /*store*/) {
  project.set_time_signatures(segments_);
  return true;
}

EditCommandPtr SetTimeSignatureSegment::invert(const Project& before,
                                               const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetTimeSignatureSegment>(before.time_signatures());
}

bool SetHarmonySegment::apply(Project& project, MidiContentStore& /*store*/) {
  project.annotation().chords = chords_;
  return true;
}

EditCommandPtr SetHarmonySegment::invert(const Project& before,
                                         const MidiContentStore& /*store_before*/) const {
  return std::make_unique<SetHarmonySegment>(before.annotation().chords);
}

// ===========================================================================
// Automation commands
// ===========================================================================

bool AddAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr) {
    return false;
  }
  const uint32_t target = lane_.target_param_id();
  const auto existing =
      std::find_if(t->automation_lanes.begin(), t->automation_lanes.end(),
                   [target](const auto& lane) { return lane.target_param_id() == target; });
  if (existing != t->automation_lanes.end()) return false;
  t->automation_lanes.push_back(lane_);
  return true;
}

EditCommandPtr AddAutomationLane::invert(const Project& /*before*/,
                                         const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveAutomationLane>(track_id_, lane_.target_param_id());
}

bool RemoveAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr) {
    return false;
  }
  const auto found =
      std::find_if(t->automation_lanes.begin(), t->automation_lanes.end(),
                   [this](const auto& lane) { return lane.target_param_id() == target_param_id_; });
  if (found == t->automation_lanes.end()) return false;
  t->automation_lanes.erase(found);
  return true;
}

EditCommandPtr RemoveAutomationLane::invert(const Project& before,
                                            const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(track_id_);
  if (t == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(t->automation_lanes.begin(), t->automation_lanes.end(),
                   [this](const auto& lane) { return lane.target_param_id() == target_param_id_; });
  if (found == t->automation_lanes.end()) return nullptr;
  // Re-insert the removed lane at its original index.
  return std::make_unique<InsertAutomationLane>(
      track_id_, static_cast<size_t>(std::distance(t->automation_lanes.begin(), found)), *found);
}

bool EditAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr || lane_.target_param_id() != target_param_id_) {
    return false;
  }
  const auto found =
      std::find_if(t->automation_lanes.begin(), t->automation_lanes.end(),
                   [this](const auto& item) { return item.target_param_id() == target_param_id_; });
  if (found == t->automation_lanes.end()) return false;
  *found = lane_;
  return true;
}

EditCommandPtr EditAutomationLane::invert(const Project& before,
                                          const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(track_id_);
  if (t == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(t->automation_lanes.begin(), t->automation_lanes.end(),
                   [this](const auto& item) { return item.target_param_id() == target_param_id_; });
  if (found == t->automation_lanes.end()) return nullptr;
  return std::make_unique<EditAutomationLane>(track_id_, target_param_id_, *found);
}

}  // namespace sonare::arrangement
