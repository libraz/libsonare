/// @file edit_command.cpp
/// @brief Implementation of deterministic, invertible arrangement edit commands.

#include "arrangement/edit_command.h"

#include <algorithm>
#include <utility>

namespace sonare::arrangement {
namespace {

bool clip_can_be_inserted(const Project& project, const EditClip& clip, ClipId ignore_clip_id = 0) {
  if (clip.id == 0 || project.has_clip(clip.id)) return false;
  if (!project.has_track(clip.track_id) || !project.has_source(clip.source_id)) return false;
  if (!(clip.length_ppq > 0.0) || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0) {
    return false;
  }
  if (clip.loop_mode == LoopMode::kLoop && !(clip.loop_length_ppq > 0.0)) return false;
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq, ignore_clip_id)) {
    return false;
  }
  return true;
}

}  // namespace

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

bool RemoveTrack::apply(Project& project, MidiContentStore& /*store*/) {
  return project.remove_track(id_).second;
}

EditCommandPtr RemoveTrack::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(id_);
  if (t == nullptr) {
    return nullptr;
  }
  // Restore the exact track on undo; pre-seed the id so it returns with the same
  // stable id (and so a later redo reuses it too), and the original index so the
  // track ordering is preserved.
  auto cmd = std::make_unique<AddTrack>(*t);
  cmd->reseed_id(id_);
  cmd->reseed_index(before.track_index(id_));
  return cmd;
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

// ===========================================================================
// Clip commands
// ===========================================================================

bool AddClip::apply(Project& project, MidiContentStore& store) {
  if (allocated_id_ != 0) {
    EditClip c = clip_;
    c.id = allocated_id_;
    if (!project.insert_clip_raw(std::move(c), restore_index_)) {
      return false;
    }
    project.ensure_next_clip_id(allocated_id_);
    if (has_restore_events_) {
      store.events[allocated_id_] = restore_events_;
    }
    return true;
  }
  allocated_id_ = project.add_clip(clip_);
  return allocated_id_ != 0;
}

EditCommandPtr AddClip::invert(const Project& /*before*/,
                               const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveClip>(allocated_id_);
}

bool RemoveClip::apply(Project& project, MidiContentStore& store) {
  const bool ok = project.remove_clip(id_).second;
  if (ok) {
    store.events.erase(id_);
  }
  return ok;
}

EditCommandPtr RemoveClip::invert(const Project& before,
                                  const MidiContentStore& store_before) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  auto add = std::make_unique<AddClip>(*c);
  // The restored clip keeps its original id, position, and any MIDI content.
  add->reseed_id(id_);
  add->reseed_index(before.clip_index(id_));
  const auto it = store_before.events.find(id_);
  if (it != store_before.events.end()) {
    add->set_restore_events(it->second);
  }
  return add;
}

bool SplitClip::apply(Project& project, MidiContentStore& store) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (!(split_ppq_ > c->start_ppq) || !(split_ppq_ < c->end_ppq())) {
    return false;
  }
  const double left_len = split_ppq_ - c->start_ppq;
  const double right_len = c->end_ppq() - split_ppq_;

  // Build the right-hand clip from the original before shortening.
  EditClip right = *c;
  right.id = 0;
  right.start_ppq = split_ppq_;
  right.length_ppq = right_len;
  right.source_offset_ppq = c->source_offset_ppq + left_len;
  right.fade_in = ClipFade{};  // inner edge has no fade-in
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(right.track_id, right.start_ppq, right.length_ppq, id_)) {
    return false;
  }
  // Left keeps fade_in, drops fade_out at the cut.
  c->length_ppq = left_len;
  c->fade_out = ClipFade{};

  if (new_clip_id_ != 0) {
    right.id = new_clip_id_;
    if (!clip_can_be_inserted(project, right, id_) || !project.insert_clip_raw(right)) {
      return false;
    }
    project.ensure_next_clip_id(new_clip_id_);
  } else {
    new_clip_id_ = project.add_clip(right);
    if (new_clip_id_ == 0) {
      return false;
    }
  }
  // Split MIDI content by source PPQ so the two clips do not carry duplicate
  // event lists after editing / serialization. Note cutting at the boundary is a
  // later MIDI-editor concern; this preserves event ownership deterministically.
  const auto it = store.events.find(id_);
  if (it != store.events.end()) {
    MidiClipEventList left_events;
    MidiClipEventList right_events;
    const double split_source_ppq = right.source_offset_ppq;
    for (const MidiClipEvent& ev : it->second) {
      if (ev.ppq < split_source_ppq) {
        left_events.push_back(ev);
      } else {
        right_events.push_back(ev);
      }
    }
    it->second = std::move(left_events);
    store.events[new_clip_id_] = std::move(right_events);
  }
  return true;
}

EditCommandPtr SplitClip::invert(const Project& before,
                                 const MidiContentStore& /*store_before*/) const {
  const EditClip* original = before.find_clip(id_);
  if (original == nullptr) {
    return nullptr;
  }
  // Undo a split = remove the new right clip and restore the original clip's
  // length/fade. A small composite is expressed via a dedicated restore command.
  return std::make_unique<UnsplitClip>(id_, new_clip_id_, split_ppq_, *original);
}

bool TrimClip::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (!(new_length_ppq_ > 0.0) || new_start_ppq_ < 0.0) {
    return false;
  }
  const double delta = new_start_ppq_ - c->start_ppq;
  const double new_offset = c->source_offset_ppq + delta;
  if (new_offset < 0.0) {
    return false;
  }
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(c->track_id, new_start_ppq_, new_length_ppq_, id_)) {
    return false;
  }
  c->start_ppq = new_start_ppq_;
  c->source_offset_ppq = new_offset;
  c->length_ppq = new_length_ppq_;
  return true;
}

EditCommandPtr TrimClip::invert(const Project& before,
                                const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<TrimClip>(id_, c->start_ppq, c->length_ppq);
}

bool MoveClip::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (new_start_ppq_ < 0.0) {
    return false;
  }
  if (new_track_id_ != 0 && !project.has_track(new_track_id_)) {
    return false;
  }
  const TrackId target_track = new_track_id_ != 0 ? new_track_id_ : c->track_id;
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(target_track, new_start_ppq_, c->length_ppq, id_)) {
    return false;
  }
  c->start_ppq = new_start_ppq_;
  if (new_track_id_ != 0) {
    c->track_id = new_track_id_;
  }
  return true;
}

EditCommandPtr MoveClip::invert(const Project& before,
                                const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<MoveClip>(id_, c->start_ppq, c->track_id);
}

bool DuplicateClip::apply(Project& project, MidiContentStore& store) {
  const EditClip* src = project.find_clip(id_);
  if (src == nullptr) {
    return false;
  }
  EditClip copy = *src;
  copy.id = 0;
  copy.start_ppq = new_start_ppq_;
  if (copy.start_ppq < 0.0) {
    return false;
  }
  if (new_clip_id_ != 0) {
    copy.id = new_clip_id_;
    if (!clip_can_be_inserted(project, copy) || !project.insert_clip_raw(copy)) {
      return false;
    }
    project.ensure_next_clip_id(new_clip_id_);
  } else {
    new_clip_id_ = project.add_clip(copy);
    if (new_clip_id_ == 0) {
      return false;
    }
  }
  const auto it = store.events.find(id_);
  if (it != store.events.end()) {
    store.events[new_clip_id_] = it->second;
  }
  return true;
}

EditCommandPtr DuplicateClip::invert(const Project& /*before*/,
                                     const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveClip>(new_clip_id_);
}

bool SetClipGain::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  c->gain = gain_;
  return true;
}

EditCommandPtr SetClipGain::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipGain>(id_, c->gain);
}

bool SetClipFade::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  c->fade_in = fade_in_;
  c->fade_out = fade_out_;
  return true;
}

EditCommandPtr SetClipFade::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipFade>(id_, c->fade_in, c->fade_out);
}

bool SetClipLoop::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (mode_ == LoopMode::kLoop && !(loop_length_ppq_ > 0.0)) {
    return false;
  }
  c->loop_mode = mode_;
  c->loop_length_ppq = loop_length_ppq_;
  return true;
}

EditCommandPtr SetClipLoop::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipLoop>(id_, c->loop_mode, c->loop_length_ppq);
}

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

bool SetMarker::apply(Project& project, MidiContentStore& /*store*/) {
  if (id_ == 0 && allocated_id_ == 0) {
    allocated_id_ = project.add_marker(ppq_, name_);
    return allocated_id_ != 0;
  }
  const uint32_t target = id_ != 0 ? id_ : allocated_id_;
  for (ProjectMarker& m : project.markers_mutable()) {
    if (m.id == target) {
      m.ppq = ppq_;
      m.name = name_;
      return true;
    }
  }
  // Marker does not exist (e.g. redo after the original add was undone): restore
  // it with its fixed id.
  ProjectMarker m;
  m.id = target;
  m.ppq = ppq_;
  m.name = name_;
  project.markers_mutable().push_back(std::move(m));
  project.ensure_next_marker_id(target);
  return true;
}

EditCommandPtr SetMarker::invert(const Project& before,
                                 const MidiContentStore& /*store_before*/) const {
  const uint32_t target = id_ != 0 ? id_ : allocated_id_;
  for (const ProjectMarker& m : before.markers()) {
    if (m.id == target) {
      // Marker existed before: restore prior ppq/name.
      return std::make_unique<SetMarker>(target, m.ppq, m.name);
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
  lane_index_ = t->automation_lanes.size();
  t->automation_lanes.push_back(lane_);
  return true;
}

EditCommandPtr AddAutomationLane::invert(const Project& /*before*/,
                                         const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveAutomationLane>(track_id_, lane_index_);
}

bool RemoveAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr || lane_index_ >= t->automation_lanes.size()) {
    return false;
  }
  t->automation_lanes.erase(t->automation_lanes.begin() + static_cast<std::ptrdiff_t>(lane_index_));
  return true;
}

EditCommandPtr RemoveAutomationLane::invert(const Project& before,
                                            const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(track_id_);
  if (t == nullptr || lane_index_ >= t->automation_lanes.size()) {
    return nullptr;
  }
  // Re-insert the removed lane at its original index.
  return std::make_unique<InsertAutomationLane>(track_id_, lane_index_,
                                                t->automation_lanes[lane_index_]);
}

bool EditAutomationLane::apply(Project& project, MidiContentStore& /*store*/) {
  Track* t = project.find_track_mutable(track_id_);
  if (t == nullptr || lane_index_ >= t->automation_lanes.size()) {
    return false;
  }
  t->automation_lanes[lane_index_] = lane_;
  return true;
}

EditCommandPtr EditAutomationLane::invert(const Project& before,
                                          const MidiContentStore& /*store_before*/) const {
  const Track* t = before.find_track(track_id_);
  if (t == nullptr || lane_index_ >= t->automation_lanes.size()) {
    return nullptr;
  }
  return std::make_unique<EditAutomationLane>(track_id_, lane_index_,
                                              t->automation_lanes[lane_index_]);
}

// ===========================================================================
// MIDI content commands
// ===========================================================================

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

}  // namespace

bool ReplaceMidiClipEvents::apply(Project& project, MidiContentStore& store) {
  if (!project.has_clip(clip_id_)) {
    return false;
  }
  for (const auto& [handle, payload] : sysex_payloads_) {
    store.sysex_payloads[handle] = payload;
  }
  store.events[clip_id_] = events_;
  prune_unreferenced_sysex_payloads(&store);
  return true;
}

EditCommandPtr ReplaceMidiClipEvents::invert(const Project& /*before*/,
                                             const MidiContentStore& store_before) const {
  MidiClipEventList prior;
  const auto it = store_before.events.find(clip_id_);
  if (it != store_before.events.end()) {
    prior = it->second;
  }
  auto payloads = payloads_for_events(store_before, prior);
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
  prune_unreferenced_sysex_payloads(&store);
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
  return std::make_unique<ReplaceMidiClipEvents>(patch_.clip_id, std::move(prior));
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
      return std::make_unique<SetMarker>(id_, m.ppq, m.name);
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
  auto left_events = store.events.find(original_id_);
  const auto right_events = store.events.find(new_clip_id_);
  if (right_events != store.events.end()) {
    if (left_events == store.events.end()) {
      left_events = store.events.emplace(original_id_, MidiClipEventList{}).first;
    }
    left_events->second.insert(left_events->second.end(), right_events->second.begin(),
                               right_events->second.end());
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
