/// @file edit_command_clip.cpp
/// @brief Clip edit-command apply/invert definitions.

#include <algorithm>
#include <cmath>
#include <utility>

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"

namespace sonare::arrangement {

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
    for (const auto& [handle, payload] : restore_sysex_payloads_) {
      store.sysex_payloads[handle] = payload;
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
    detail::prune_unreferenced_sysex_payloads(&store);
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
    add->set_restore_sysex_payloads(detail::payloads_for_events(store_before, it->second));
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
  if (c->loop_mode == LoopMode::kLoop) {
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
  right.comp_segments =
      detail::shifted_clamped_comp_segments(c->comp_segments, -left_len, right_len);
  if (!detail::shift_take_offsets(&right.takes, left_len)) {
    return false;
  }
  right.fade_in = ClipFade{};  // inner edge has no fade-in
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(right.track_id, right.start_ppq, right.length_ppq, id_)) {
    return false;
  }
  // Left keeps fade_in, drops fade_out at the cut.
  c->length_ppq = left_len;
  c->comp_segments = detail::shifted_clamped_comp_segments(c->comp_segments, 0.0, left_len);
  c->fade_out = ClipFade{};

  if (new_clip_id_ != 0) {
    right.id = new_clip_id_;
    if (!detail::clip_can_be_inserted(project, right, id_) || !project.insert_clip_raw(right)) {
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
                                 const MidiContentStore& store_before) const {
  const EditClip* original = before.find_clip(id_);
  if (original == nullptr) {
    return nullptr;
  }
  // Undo a split = remove the new right clip and restore the original clip's
  // length/fade. A small composite is expressed via a dedicated restore command.
  auto cmd = std::make_unique<UnsplitClip>(id_, new_clip_id_, split_ppq_, *original);
  // Capture the exact pre-split event list so undo restores it verbatim. The
  // store's per-clip list is not maintained sorted by ppq, so concatenating the
  // split left/right lists is not an identity round-trip; the captured list is.
  const auto it = store_before.events.find(id_);
  if (it != store_before.events.end()) {
    cmd->set_restore_events(it->second);
  }
  return cmd;
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
  std::vector<ClipTake> shifted_takes = c->takes;
  if (!detail::shift_take_offsets(&shifted_takes, delta)) {
    return false;
  }
  std::vector<ClipCompSegment> shifted_segments =
      detail::shifted_clamped_comp_segments(c->comp_segments, -delta, new_length_ppq_);
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(c->track_id, new_start_ppq_, new_length_ppq_, id_)) {
    return false;
  }
  c->start_ppq = new_start_ppq_;
  c->source_offset_ppq = new_offset;
  c->length_ppq = new_length_ppq_;
  c->takes = std::move(shifted_takes);
  c->comp_segments = std::move(shifted_segments);
  return true;
}

EditCommandPtr TrimClip::invert(const Project& before,
                                const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<detail::RestoreClip>(*c);
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
  // Reject moving a clip onto a track whose kind is incompatible with the
  // clip's source kind (audio track <- audio source, MIDI track <- MIDI
  // source; an aux track accepts neither). Allowing a cross-kind move would
  // produce a project that the compiler later rejects, so fail cleanly here
  // WITHOUT mutating state. Same-track moves (new_track_id_ == 0) are
  // unaffected. Mirrors edit_compiler.cpp::clip_matches_track_kind.
  if (new_track_id_ != 0) {
    const Track* dest = project.find_track(new_track_id_);
    const ClipSource* src = project.find_source(c->source_id);
    if (dest == nullptr || src == nullptr) {
      return false;
    }
    const SourceKind src_kind = source_kind(*src);
    const bool compatible = (dest->kind == Track::Kind::kAudio && src_kind == SourceKind::kAudio) ||
                            (dest->kind == Track::Kind::kMidi && src_kind == SourceKind::kMidi);
    if (!compatible) {
      return false;
    }
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
    if (!detail::clip_can_be_inserted(project, copy) || !project.insert_clip_raw(copy)) {
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
  // Clamp each fade to the clip length so the compiled schedule cannot place the
  // fade-out start before the clip start (an oversized fade would otherwise
  // attenuate the whole clip). A negative stored length is treated as no fade.
  const double max_fade_ppq = std::max(0.0, c->length_ppq);
  c->fade_in.length_ppq = std::clamp(c->fade_in.length_ppq, 0.0, max_fade_ppq);
  c->fade_out.length_ppq = std::clamp(c->fade_out.length_ppq, 0.0, max_fade_ppq);
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
  // Under LOOP, loop_length_ppq_ of 0 means "loop the entire clip"; reject only
  // negatives/NaN (and a comp lane that splits the clip, which loops cannot mix).
  if (mode_ == LoopMode::kLoop &&
      (!(loop_length_ppq_ >= 0.0) ||
       detail::comp_segments_split_clip(c->comp_segments, c->length_ppq))) {
    return false;
  }
  if (!(loop_crossfade_ppq_ >= 0.0)) {  // rejects negatives and NaN
    return false;
  }
  c->loop_mode = mode_;
  c->loop_length_ppq = loop_length_ppq_;
  c->loop_crossfade_ppq = loop_crossfade_ppq_;
  return true;
}

EditCommandPtr SetClipLoop::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipLoop>(id_, c->loop_mode, c->loop_length_ppq,
                                       c->loop_crossfade_ppq);
}

bool SetClipWarpRef::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  c->warp_ref_id = warp_ref_id_;
  return true;
}

EditCommandPtr SetClipWarpRef::invert(const Project& before,
                                      const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipWarpRef>(id_, c->warp_ref_id);
}

bool SetClipWarpMode::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  c->warp_mode = mode_;
  return true;
}

EditCommandPtr SetClipWarpMode::invert(const Project& before,
                                       const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipWarpMode>(id_, c->warp_mode);
}

bool SetClipTakes::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr || !detail::valid_clip_takes(project, *c, takes_, active_take_id_) ||
      !detail::valid_comp_segments(takes_, c->comp_segments, c->length_ppq)) {
    return false;
  }
  c->takes = takes_;
  c->active_take_id = active_take_id_;
  return true;
}

EditCommandPtr SetClipTakes::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipTakes>(id_, c->takes, c->active_take_id);
}

bool SetClipCompSegments::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr || (detail::clip_is_on_midi_track(project, *c) && !segments_.empty()) ||
      !detail::valid_comp_segments(c->takes, segments_, c->length_ppq) ||
      (c->loop_mode == LoopMode::kLoop &&
       detail::comp_segments_split_clip(segments_, c->length_ppq))) {
    return false;
  }
  c->comp_segments = segments_;
  return true;
}

EditCommandPtr SetClipCompSegments::invert(const Project& before,
                                           const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipCompSegments>(id_, c->comp_segments);
}

bool SetWarpMap::apply(Project& project, MidiContentStore& /*store*/) {
  return project.set_warp_map(map_);
}

EditCommandPtr SetWarpMap::invert(const Project& before,
                                  const MidiContentStore& /*store_before*/) const {
  const WarpMapRef* prior = before.find_warp_map(map_.id);
  if (prior != nullptr) {
    return std::make_unique<SetWarpMap>(*prior);
  }
  return std::make_unique<RemoveWarpMap>(map_.id);
}

bool RemoveWarpMap::apply(Project& project, MidiContentStore& /*store*/) {
  return project.remove_warp_map(id_).second;
}

EditCommandPtr RemoveWarpMap::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const WarpMapRef* prior = before.find_warp_map(id_);
  if (prior == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetWarpMap>(*prior);
}

bool SetClipSource::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (!detail::source_matches_track_kind(project, c->track_id, source_id_)) {
    return false;
  }
  c->source_id = source_id_;
  return true;
}

EditCommandPtr SetClipSource::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<SetClipSource>(id_, c->source_id);
}

}  // namespace sonare::arrangement
