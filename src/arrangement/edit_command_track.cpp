/// @file edit_command_track.cpp
/// @brief Track edit-command apply/invert definitions.

#include <algorithm>
#include <cmath>

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"

namespace sonare::arrangement {

// ===========================================================================
// Track commands
// ===========================================================================

bool AddTrack::apply(Project& project, MidiContentStore& /*store*/) {
  if (allocated_id_ != 0) {
    Track t = track_;
    t.id = allocated_id_;
    if (!project.insert_track_raw(std::move(t), restore_index_)) {
      return false;
    }
    project.ensure_next_track_id(allocated_id_);
    return true;
  }
  allocated_id_ = project.add_track(track_);
  return allocated_id_ != 0;
}

EditCommandPtr AddTrack::invert(const Project& /*before*/,
                                const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveTrack>(allocated_id_);
}

bool RemoveTrack::apply(Project& project, MidiContentStore& store) {
  if (!project.has_track(id_)) {
    return false;
  }

  std::vector<ClipId> clip_ids;
  for (const EditClip& clip : project.clips()) {
    if (clip.track_id == id_) {
      clip_ids.push_back(clip.id);
    }
  }
  for (ClipId clip_id : clip_ids) {
    if (!project.remove_clip(clip_id).second) {
      return false;
    }
    store.events.erase(clip_id);
  }
  detail::prune_unreferenced_sysex_payloads(&store);
  return project.remove_track(id_).second;
}

EditCommandPtr RemoveTrack::invert(const Project& before,
                                   const MidiContentStore& store_before) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  std::vector<detail::RemovedTrackClipSnapshot> clips;
  for (const EditClip& clip : before.clips()) {
    if (clip.track_id != id_) {
      continue;
    }
    detail::RemovedTrackClipSnapshot snapshot;
    snapshot.clip = clip;
    snapshot.index = before.clip_index(clip.id);
    const auto events = store_before.events.find(clip.id);
    if (events != store_before.events.end()) {
      snapshot.events = events->second;
      snapshot.sysex_payloads = detail::payloads_for_events(store_before, snapshot.events);
      snapshot.has_events = true;
    }
    clips.push_back(std::move(snapshot));
  }
  return std::make_unique<detail::RestoreTrackWithClips>(*t, before.track_index(id_),
                                                         std::move(clips));
}

bool RenameTrack::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->name = name_;
  return true;
}

EditCommandPtr RenameTrack::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<RenameTrack>(id_, t->name);
}

bool SetTrackRoute::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->channel_strip_ref = channel_strip_ref_;
  t->output_target = output_target_;
  return true;
}

EditCommandPtr SetTrackRoute::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackRoute>(id_, t->channel_strip_ref, t->output_target);
}

bool SetTrackKind::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  // Reject a kind change that would orphan existing clips: flipping a track that
  // still holds clips to an incompatible kind makes every later bounce/playback
  // suppress the whole project timeline (the compile-time gate rejects the entire
  // project, not just the offending clips). Verify each clip's source is
  // compatible with the target kind before committing.
  if (t->kind != kind_) {
    for (const EditClip& clip : project.clips()) {
      if (clip.track_id != id_) {
        continue;
      }
      const ClipSource* source = project.find_source(clip.source_id);
      if (source == nullptr ||
          !detail::track_kind_accepts_source_kind(kind_, source_kind(*source))) {
        return false;
      }
    }
  }
  t->kind = kind_;
  return true;
}

EditCommandPtr SetTrackKind::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackKind>(id_, t->kind);
}

bool SetTrackMidiDestination::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->midi_destination_id = destination_id_;
  return true;
}

EditCommandPtr SetTrackMidiDestination::invert(const Project& before,
                                               const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackMidiDestination>(id_, t->midi_destination_id);
}

bool SetTrackGain::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->gain = std::isfinite(gain_) ? std::max(0.0f, gain_) : 1.0f;
  return true;
}

EditCommandPtr SetTrackGain::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackGain>(id_, t->gain);
}

bool SetTrackMute::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->mute = mute_;
  return true;
}

EditCommandPtr SetTrackMute::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackMute>(id_, t->mute);
}

bool SetTrackSolo::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->solo = solo_;
  return true;
}

EditCommandPtr SetTrackSolo::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackSolo>(id_, t->solo);
}

bool SetTrackPan::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(id_);
  if (t == nullptr) {
    return false;
  }
  t->pan = std::isfinite(pan_) ? std::clamp(pan_, -1.0f, 1.0f) : 0.0f;
  return true;
}

EditCommandPtr SetTrackPan::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetTrackPan>(id_, t->pan);
}

}  // namespace sonare::arrangement
