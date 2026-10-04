// SONARE_WASM_EXCEPTION_UNWIND: release staged clips and sidecars when allocation propagates to
// history.
/// @file edit_command_clip.cpp
/// @brief Clip edit-command apply/invert definitions.

#include <algorithm>
#include <cmath>
#include <utility>

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"
#include "arrangement/vocal_edit_sidecar.h"

namespace sonare::arrangement {
namespace {

/// Re-clamps a clip's fades to its current length.
///
/// Fades are stored as absolute PPQ lengths, so any command that shortens a
/// clip can leave one longer than the clip that carries it. The renderer
/// re-clamps independently (edit_compiler), so the audio stays right and only
/// the stored model is wrong — which is exactly why it survives: the serializer
/// writes what the model holds, so an oversized fade is persisted into the
/// project document and read back on the next load. Every command that assigns
/// length_ppq calls this before returning, which is the whole of the invariant
/// and the reason it lives in one function rather than at each assignment.
void clamp_fades_to_length(EditClip* clip) {
  const double max_fade_ppq = std::max(0.0, clip->length_ppq);
  clip->fade_in.length_ppq = std::clamp(clip->fade_in.length_ppq, 0.0, max_fade_ppq);
  clip->fade_out.length_ppq = std::clamp(clip->fade_out.length_ppq, 0.0, max_fade_ppq);
}

}  // namespace

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
    vocal_sidecar::remove_clip_sidecars(&project, id_);
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
  return vocal_sidecar::wrap_inverse(std::move(add), before, {id_});
}

bool SplitClip::apply(Project& project, MidiContentStore& store) {
  EditClip* current = project.find_clip_mutable(id_);
  if (current == nullptr) {
    return false;
  }
  if (current->warp_ref_id != 0) {
    return false;
  }
  if (!(split_ppq_ > current->start_ppq) || !(split_ppq_ < current->end_ppq())) {
    return false;
  }
  if (current->loop_mode == LoopMode::kLoop) {
    return false;
  }
  const double original_start = current->start_ppq;
  const double left_len = split_ppq_ - original_start;
  const double right_len = current->end_ppq() - split_ppq_;

  // Materialize on private copies so a failed insert leaves the project untouched.
  const EditClip before = *current;
  EditClip original = before;
  if (!detail::materialize_comp_render_parts(&original)) return false;
  if (!detail::materialize_audio_source_offsets(project, &original)) return false;
  EditClip left = original;
  EditClip right = original;

  // Build the right-hand clip from the original before shortening.
  right.id = 0;
  right.start_ppq = split_ppq_;
  right.length_ppq = right_len;
  right.source_offset_ppq = original.source_offset_ppq + left_len;
  right.comp_segments =
      detail::shifted_clamped_comp_segments(original.comp_segments, -left_len, right_len);
  if (!original.comp_render_parts.empty() &&
      !detail::remap_comp_render_parts(original.comp_render_parts, left_len, original.length_ppq,
                                       left_len, &right.comp_render_parts,
                                       original.active_take_id)) {
    return false;
  }
  if (!detail::shift_take_offsets(&right.takes, left_len)) {
    return false;
  }
  const ClipSource* source = project.find_source(original.source_id);
  if (source != nullptr && source_kind(*source) == SourceKind::kAudio) {
    transport::TempoMap tempo_map;
    if (!detail::make_edit_tempo_map(project, &tempo_map)) return false;
    double split_delta_seconds = 0.0;
    if (!detail::timeline_delta_seconds(tempo_map, original_start, split_ppq_,
                                        project.sample_rate(), &split_delta_seconds) ||
        !detail::shift_physical_source_offsets(&right, split_delta_seconds)) {
      return false;
    }
  }
  right.fade_in = ClipFade{};     // inner edge has no fade-in
  clamp_fades_to_length(&right);  // the outer fade-out now has a shorter clip
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(right.track_id, right.start_ppq, right.length_ppq, id_)) {
    return false;
  }
  // add_clip checks overlap, so the left half is shortened (on the copy) first.
  const auto shorten_left = [&] {
    left.length_ppq = left_len;
    left.comp_segments = detail::shifted_clamped_comp_segments(left.comp_segments, 0.0, left_len);
    if (!original.comp_render_parts.empty()) {
      // The left half keeps the original clip-local origin, so its render
      // geometry only needs the right edge clipped to the split point.
      if (!detail::remap_comp_render_parts(original.comp_render_parts, 0.0, left_len, 0.0,
                                           &left.comp_render_parts, original.active_take_id)) {
        return false;
      }
    }
    left.fade_out = ClipFade{};    // inner edge has no fade-out
    clamp_fades_to_length(&left);  // the outer fade-in now has a shorter clip
    return true;
  };
  if (!shorten_left()) return false;
  const bool allocate_new_id = new_clip_id_ == 0;
  if (!allocate_new_id) {
    right.id = new_clip_id_;
    if (!detail::clip_can_be_inserted(project, right, id_) || !project.insert_clip_raw(right)) {
      return false;
    }
    project.ensure_next_clip_id(new_clip_id_);
  } else {
    // Install the shortened left only around add_clip; restore the original on failure.
    *current = left;
    new_clip_id_ = project.add_clip(right);
    if (new_clip_id_ == 0) {
      *current = before;
      return false;
    }
  }
  // The right clip is in; commit the prepared left snapshot.
  current = project.find_clip_mutable(id_);
  if (current == nullptr) return false;  // Defensive: insertion must preserve it.
  if (!allocate_new_id) {
    *current = std::move(left);
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
  vocal_sidecar::clone_clip_sidecars(&project, id_, new_clip_id_);
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
  return vocal_sidecar::wrap_inverse(std::move(cmd), before, {id_, new_clip_id_});
}

bool TrimClip::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* current = project.find_clip_mutable(id_);
  if (current == nullptr) {
    return false;
  }
  if (current->warp_ref_id != 0) {
    return false;
  }
  if (!(new_length_ppq_ > 0.0) || new_start_ppq_ < 0.0) {
    return false;
  }
  const double old_start = current->start_ppq;
  const double delta = new_start_ppq_ - old_start;
  EditClip next = *current;
  if (next.loop_mode == LoopMode::kLoop) {
    // Loop schedules must describe one source body. A hidden multi-part comp
    // cache would silently disable looping for the fragment, so reject that
    // state; a trivial single-part cache is redundant and can be discarded.
    if (!is_trivial_loop_comp_render_parts(next)) return false;
    next.comp_render_parts.clear();
  } else if (!detail::materialize_comp_render_parts(&next)) {
    return false;
  }
  if (!detail::materialize_audio_source_offsets(project, &next)) return false;
  const ClipSource* source = project.find_source(next.source_id);
  const bool audio_clip = source != nullptr && source_kind(*source) == SourceKind::kAudio;
  const bool preserve_loop_source_offsets = audio_clip && next.loop_mode == LoopMode::kLoop;
  double new_offset = next.source_offset_ppq;
  if (!preserve_loop_source_offsets) {
    new_offset += delta;
    if (new_offset < 0.0) {
      return false;
    }
    if (!detail::shift_take_offsets(&next.takes, delta)) {
      return false;
    }
  }
  std::vector<ClipCompSegment> shifted_segments =
      detail::shifted_clamped_comp_segments(next.comp_segments, -delta, new_length_ppq_);
  // The same loop/comp pair SetClipLoop and SetClipCompSegments refuse to write.
  if (next.loop_mode == LoopMode::kLoop &&
      detail::comp_segments_split_clip(shifted_segments, new_length_ppq_)) {
    return false;
  }
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(current->track_id, new_start_ppq_, new_length_ppq_, id_)) {
    return false;
  }
  double delta_seconds = 0.0;
  transport::TempoMap tempo_map;
  if (audio_clip) {
    if (!detail::make_edit_tempo_map(project, &tempo_map) ||
        !detail::timeline_delta_seconds(tempo_map, old_start, new_start_ppq_, project.sample_rate(),
                                        &delta_seconds)) {
      return false;
    }
  }
  if (preserve_loop_source_offsets) {
    const double old_loop_ppq = next.loop_length_ppq > 0.0 ? next.loop_length_ppq : next.length_ppq;
    double period_seconds = 0.0;
    double old_phase_seconds = 0.0;
    if (next.loop_anchor.has_value()) {
      period_seconds = next.loop_anchor->period_seconds;
      old_phase_seconds = next.loop_anchor->phase_seconds;
    } else if (!detail::timeline_delta_seconds(tempo_map, old_start, old_start + old_loop_ppq,
                                               project.sample_rate(), &period_seconds)) {
      return false;
    }
    if (!(std::isfinite(period_seconds) && period_seconds > 0.0 &&
          std::isfinite(old_phase_seconds))) {
      return false;
    }
    // A loop trim keeps the source body fixed; only the playback phase moves.
    const double phase_seconds = old_phase_seconds + delta_seconds;
    if (!std::isfinite(phase_seconds)) return false;
    next.loop_anchor = LoopAnchor{period_seconds, phase_seconds};
  } else if (audio_clip && !detail::shift_physical_source_offsets(&next, delta_seconds)) {
    return false;
  }
  next.start_ppq = new_start_ppq_;
  if (!preserve_loop_source_offsets) next.source_offset_ppq = new_offset;
  next.length_ppq = new_length_ppq_;
  next.comp_segments = std::move(shifted_segments);
  if (!next.comp_render_parts.empty()) {
    const double window_end = delta + new_length_ppq_;
    const std::vector<ClipCompRenderPart> before_render_parts = next.comp_render_parts;
    if (!detail::remap_comp_render_parts(before_render_parts, delta, window_end, delta,
                                         &next.comp_render_parts, next.active_take_id)) {
      return false;
    }
  }
  clamp_fades_to_length(&next);
  *current = std::move(next);
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
  EditClip* current = project.find_clip_mutable(id_);
  if (current == nullptr) {
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
    const ClipSource* src = project.find_source(current->source_id);
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
  const TrackId target_track = new_track_id_ != 0 ? new_track_id_ : current->track_id;
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(target_track, new_start_ppq_, current->length_ppq, id_)) {
    return false;
  }
  EditClip next = *current;
  if (!detail::materialize_audio_source_offsets(project, &next)) return false;
  next.start_ppq = new_start_ppq_;
  if (new_track_id_ != 0) {
    next.track_id = new_track_id_;
  }
  *current = std::move(next);
  return true;
}

EditCommandPtr MoveClip::invert(const Project& before,
                                const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<detail::RestoreClip>(*c);
}

bool DuplicateClip::apply(Project& project, MidiContentStore& store) {
  const EditClip* src = project.find_clip(id_);
  if (src == nullptr) {
    return false;
  }
  EditClip copy = *src;
  if (!detail::materialize_audio_source_offsets(project, &copy)) return false;
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
  vocal_sidecar::clone_clip_sidecars(&project, id_, new_clip_id_);
  return true;
}

EditCommandPtr DuplicateClip::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  return vocal_sidecar::wrap_inverse(std::make_unique<RemoveClip>(new_clip_id_), before,
                                     {id_, new_clip_id_});
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
  // Clamped so the compiled schedule cannot place the fade-out start before the
  // clip start (an oversized fade would otherwise attenuate the whole clip). A
  // negative stored length is treated as no fade.
  clamp_fades_to_length(c);
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
  if (mode_ == LoopMode::kLoop && !is_trivial_loop_visible_render_parts(*c)) {
    // Visible seam overlap left by a cut would render as a non-looped fragment,
    // so it must be cleaned up first. Retained-only parts play nothing and are
    // dropped with the cache below; undo restores them.
    return false;
  }
  if (!(loop_crossfade_ppq_ >= 0.0)) {  // rejects negatives and NaN
    return false;
  }
  c->loop_mode = mode_;
  c->loop_length_ppq = loop_length_ppq_;
  c->loop_crossfade_ppq = loop_crossfade_ppq_;
  // A newly authored loop supersedes any period/phase kept by an earlier loop trim.
  c->loop_anchor.reset();
  if (mode_ == LoopMode::kLoop) c->comp_render_parts.clear();
  return true;
}

EditCommandPtr SetClipLoop::invert(const Project& before,
                                   const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<detail::RestoreClip>(*c);
}

bool SetClipWarpRef::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  // A non-zero id must name a warp map that is actually registered; 0 stays
  // legal as "no warp map" and always clears the reference.
  if (warp_ref_id_ != 0 && !project.has_warp_map(warp_ref_id_)) {
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
  const EditClip before = *c;
  c->takes = takes_;
  c->active_take_id = active_take_id_;
  // Changing the take table reauthors the render source of every fragment. The
  // old cache may refer to removed ids, so it must not survive this command.
  c->comp_render_parts.clear();
  vocal_sidecar::prune_changed_bindings(&project, before, *c);
  return true;
}

EditCommandPtr SetClipTakes::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return vocal_sidecar::wrap_inverse(std::make_unique<detail::RestoreClip>(*c), before, {id_});
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
  // The authored comp lane is now the source of truth; cached fragments belong
  // to the previous lane and would otherwise override this edit in the compiler.
  c->comp_render_parts.clear();
  return true;
}

EditCommandPtr SetClipCompSegments::invert(const Project& before,
                                           const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return std::make_unique<detail::RestoreClip>(*c);
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
  if (!project.remove_warp_map(id_).second) return false;
  for (const EditClip& clip : project.clips()) {
    if (clip.warp_ref_id != id_) continue;
    EditClip* mutable_clip = project.find_clip_mutable(clip.id);
    if (mutable_clip != nullptr) mutable_clip->warp_ref_id = 0;
  }
  return true;
}

EditCommandPtr RemoveWarpMap::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const WarpMapRef* prior = before.find_warp_map(id_);
  if (prior == nullptr) {
    return nullptr;
  }
  std::vector<ClipId> clip_ids;
  for (const EditClip& clip : before.clips()) {
    if (clip.warp_ref_id == id_) clip_ids.push_back(clip.id);
  }
  return std::make_unique<RestoreWarpMap>(*prior, std::move(clip_ids));
}

bool RestoreWarpMap::apply(Project& project, MidiContentStore& /*store*/) {
  if (!project.set_warp_map(map_)) return false;
  for (const ClipId id : clip_ids_) {
    EditClip* clip = project.find_clip_mutable(id);
    if (clip == nullptr) return false;
    clip->warp_ref_id = map_.id;
  }
  return true;
}

EditCommandPtr RestoreWarpMap::invert(const Project& /*before*/,
                                      const MidiContentStore& /*store_before*/) const {
  return std::make_unique<RemoveWarpMap>(map_.id);
}

bool SetClipSource::apply(Project& project, MidiContentStore& /*store*/) {
  EditClip* c = project.find_clip_mutable(id_);
  if (c == nullptr) {
    return false;
  }
  if (!detail::source_matches_track_kind(project, c->track_id, source_id_)) {
    return false;
  }
  const EditClip before = *c;
  c->source_id = source_id_;
  vocal_sidecar::prune_changed_bindings(&project, before, *c);
  return true;
}

EditCommandPtr SetClipSource::invert(const Project& before,
                                     const MidiContentStore& /*store_before*/) const {
  const EditClip* c = before.find_clip(id_);
  if (c == nullptr) {
    return nullptr;
  }
  return vocal_sidecar::wrap_inverse(std::make_unique<SetClipSource>(id_, c->source_id), before,
                                     {id_});
}

}  // namespace sonare::arrangement
