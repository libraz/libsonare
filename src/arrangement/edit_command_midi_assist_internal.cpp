/// @file edit_command_midi_assist_internal.cpp
/// @brief MIDI-content, assist, and internal inverse-only edit-command definitions.

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"

namespace sonare::arrangement {

namespace detail {
namespace {

bool midi_store_references_sysex(const MidiContentStore& store, uint32_t handle) {
  if (handle == 0) return true;
  for (const auto& [clip_id, events] : store.events) {
    (void)clip_id;
    for (const MidiClipEvent& event : events) {
      if (event.sysex_handle == handle) return true;
    }
  }
  return false;
}

}  // namespace

void prune_unreferenced_sysex_payloads(MidiContentStore* store) {
  for (auto it = store->sysex_payloads.begin(); it != store->sysex_payloads.end();) {
    if (midi_store_references_sysex(*store, it->first)) {
      ++it;
    } else {
      it = store->sysex_payloads.erase(it);
    }
  }
}

std::map<uint32_t, std::vector<uint8_t>> payloads_for_events(const MidiContentStore& store,
                                                             const MidiClipEventList& events) {
  std::map<uint32_t, std::vector<uint8_t>> payloads;
  for (const MidiClipEvent& event : events) {
    if (event.sysex_handle == 0) continue;
    const auto it = store.sysex_payloads.find(event.sysex_handle);
    if (it != store.sysex_payloads.end()) {
      payloads.emplace(it->first, it->second);
    }
  }
  return payloads;
}

}  // namespace detail

// ===========================================================================
// MIDI content commands
// ===========================================================================

bool ReplaceMidiClipEvents::apply(Project& project, MidiContentStore& store) {
  if (!project.has_clip(clip_id_)) {
    return false;
  }
  for (const auto& [handle, payload] : sysex_payloads_) {
    store.sysex_payloads[handle] = payload;
  }
  store.events[clip_id_] = events_;
  detail::prune_unreferenced_sysex_payloads(&store);
  return true;
}

EditCommandPtr ReplaceMidiClipEvents::invert(const Project& /*before*/,
                                             const MidiContentStore& store_before) const {
  MidiClipEventList prior;
  const auto it = store_before.events.find(clip_id_);
  if (it != store_before.events.end()) {
    prior = it->second;
  }
  auto payloads = detail::payloads_for_events(store_before, prior);
  return std::make_unique<ReplaceMidiClipEvents>(clip_id_, std::move(prior), std::move(payloads));
}

bool PatchMidiClip::apply(Project& project, MidiContentStore& store) {
  if (!project.has_clip(patch_.clip_id)) {
    return false;
  }
  MidiClipEventList& list = store.events[patch_.clip_id];
  // Remove first (by value match), then add.
  for (const MidiClipEvent& rm : patch_.remove) {
    const auto it = std::find(list.begin(), list.end(), rm);
    if (it != list.end()) {
      list.erase(it);
    }
  }
  list.insert(list.end(), patch_.add.begin(), patch_.add.end());
  detail::prune_unreferenced_sysex_payloads(&store);
  return true;
}

EditCommandPtr PatchMidiClip::invert(const Project& /*before*/,
                                     const MidiContentStore& store_before) const {
  // The exact inverse restores the full prior event list, which is robust even
  // when add/remove sets overlap or duplicate events exist.
  MidiClipEventList prior;
  const auto it = store_before.events.find(patch_.clip_id);
  if (it != store_before.events.end()) {
    prior = it->second;
  }
  auto payloads = detail::payloads_for_events(store_before, prior);
  return std::make_unique<ReplaceMidiClipEvents>(patch_.clip_id, std::move(prior),
                                                 std::move(payloads));
}

// ===========================================================================
// Assist command
// ===========================================================================

namespace {

// Returns the index of a sidecar matching module_id + target scope, or npos.
size_t find_sidecar(const std::vector<AssistSidecar>& sidecars, const AssistSidecar& key) {
  for (size_t i = 0; i < sidecars.size(); ++i) {
    const AssistSidecar& s = sidecars[i];
    if (s.module_id == key.module_id && s.target_track_id == key.target_track_id &&
        s.region_start_ppq == key.region_start_ppq && s.region_end_ppq == key.region_end_ppq) {
      return i;
    }
  }
  return static_cast<size_t>(-1);
}

}  // namespace

bool SetAssistSidecar::apply(Project& project, MidiContentStore& /*store*/) {
  std::vector<AssistSidecar>& sidecars = project.assist_sidecars_mutable();
  const size_t idx = find_sidecar(sidecars, sidecar_);
  if (idx != static_cast<size_t>(-1)) {
    sidecars[idx] = sidecar_;
  } else {
    sidecars.push_back(sidecar_);
  }
  return true;
}

EditCommandPtr SetAssistSidecar::invert(const Project& before,
                                        const MidiContentStore& /*store_before*/) const {
  const std::vector<AssistSidecar>& sidecars = before.assist_sidecars();
  const size_t idx = find_sidecar(sidecars, sidecar_);
  if (idx != static_cast<size_t>(-1)) {
    // Restore the prior sidecar value.
    return std::make_unique<SetAssistSidecar>(sidecars[idx]);
  }
  // Newly added: inverse removes it by matching key.
  return std::make_unique<RemoveAssistSidecarInternal>(sidecar_);
}

// ===========================================================================
// Internal inverse-only commands
// ===========================================================================

bool RemoveSourceInternal::apply(Project& project, MidiContentStore& /*store*/) {
  return project.remove_source(id_).second;
}

EditCommandPtr RemoveSourceInternal::invert(const Project& before,
                                            const MidiContentStore& /*store_before*/) const {
  const ClipSource* s = before.find_source(id_);
  if (s == nullptr) {
    return nullptr;
  }
  // Re-attach the exact source with its original id and position on undo.
  const size_t index = before.source_index(id_);
  if (const auto* audio = std::get_if<AudioSourceRef>(s)) {
    auto cmd = std::make_unique<AttachAudioSource>(*audio);
    cmd->reseed_id(id_);
    cmd->reseed_index(index);
    return cmd;
  }
  auto cmd = std::make_unique<AttachMidiSource>(std::get<MidiSourceRef>(*s));
  cmd->reseed_id(id_);
  cmd->reseed_index(index);
  return cmd;
}

bool RemoveMarkerInternal::apply(Project& project, MidiContentStore& /*store*/) {
  std::vector<ProjectMarker>& markers = project.markers_mutable();
  for (auto it = markers.begin(); it != markers.end(); ++it) {
    if (it->id == id_) {
      markers.erase(it);
      return true;
    }
  }
  return false;
}

EditCommandPtr RemoveMarkerInternal::invert(const Project& before,
                                            const MidiContentStore& /*store_before*/) const {
  for (const ProjectMarker& m : before.markers()) {
    if (m.id == id_) {
      // Restore every field, not just ppq/name: dropping kind/key_fifths/
      // key_minor would resurrect a key-signature marker as a plain marker and
      // corrupt the key map on undo.
      return std::make_unique<SetMarker>(id_, m.ppq, m.name, m.kind, m.key_fifths, m.key_minor);
    }
  }
  return nullptr;
}

bool InsertAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr || lane_index_ > t->automation_lanes.size()) {
    return false;
  }
  t->automation_lanes.insert(t->automation_lanes.begin() + static_cast<std::ptrdiff_t>(lane_index_),
                             lane_);
  return true;
}

EditCommandPtr InsertAutomationLane::invert(const Project& /*before*/,
                                            const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveAutomationLane>(track_id_, lane_index_);
}

bool RemoveAssistSidecarInternal::apply(Project& project, MidiContentStore& /*store*/) {
  std::vector<AssistSidecar>& sidecars = project.assist_sidecars_mutable();
  const size_t idx = find_sidecar(sidecars, key_);
  if (idx == static_cast<size_t>(-1)) {
    return false;
  }
  sidecars.erase(sidecars.begin() + static_cast<std::ptrdiff_t>(idx));
  return true;
}

EditCommandPtr RemoveAssistSidecarInternal::invert(const Project& before,
                                                   const MidiContentStore& /*store_before*/) const {
  const std::vector<AssistSidecar>& sidecars = before.assist_sidecars();
  const size_t idx = find_sidecar(sidecars, key_);
  if (idx == static_cast<size_t>(-1)) {
    return nullptr;
  }
  return std::make_unique<SetAssistSidecar>(sidecars[idx]);
}

bool UnsplitClip::apply(Project& project, MidiContentStore& store) {
  EditClip* left = project.find_clip_mutable(original_id_);
  if (left == nullptr) {
    return false;
  }
  if (has_restore_events_) {
    // Restore the exact pre-split merged list captured by SplitClip::invert. This
    // preserves the original event order (including same-ppq tie order), which a
    // left ++ right concatenation cannot for an unsorted store list.
    store.events[original_id_] = restore_events_;
  } else {
    // Fallback for construction paths without a captured payload: reconstruct the
    // merged list by concatenating the split left/right lists.
    auto left_events = store.events.find(original_id_);
    const auto right_events = store.events.find(new_clip_id_);
    if (right_events != store.events.end()) {
      if (left_events == store.events.end()) {
        left_events = store.events.emplace(original_id_, MidiClipEventList{}).first;
      }
      left_events->second.insert(left_events->second.end(), right_events->second.begin(),
                                 right_events->second.end());
    }
  }
  // Remove the right clip created by the split and restore the original.
  project.remove_clip(new_clip_id_);
  store.events.erase(new_clip_id_);
  *left = original_;
  return true;
}

EditCommandPtr UnsplitClip::invert(const Project& /*before*/,
                                   const MidiContentStore& /*store_before*/) const {
  // Inverse of an unsplit is the original split (re-shorten the left clip and
  // recreate the right clip at the SAME id and split position).
  auto cmd = std::make_unique<SplitClip>(original_id_, split_ppq_);
  cmd->reseed_new_clip_id(new_clip_id_);
  return cmd;
}

}  // namespace sonare::arrangement
