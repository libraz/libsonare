#pragma once

/// @file edit_command_internal.h
/// @brief Shared file-local helpers for the arrangement edit-command TUs.
///
/// These validation/snapshot helpers and inverse-restore command classes are
/// implementation details of the edit-command split TUs. They are not part of
/// any public surface and live in @ref sonare::arrangement::detail so the
/// per-domain edit_command_*.cpp files can share a single definition.

#include <algorithm>
#include <cmath>
#include <utility>

#include "arrangement/edit_command.h"

namespace sonare::arrangement {
namespace detail {

void prune_unreferenced_sysex_payloads(MidiContentStore* store);
std::map<uint32_t, std::vector<uint8_t>> payloads_for_events(const MidiContentStore& store,
                                                             const MidiClipEventList& events);

inline bool clip_can_be_inserted(const Project& project, const EditClip& clip,
                                 ClipId ignore_clip_id = 0) {
  if (clip.id == 0 || project.has_clip(clip.id)) return false;
  if (!project.has_track(clip.track_id) || !project.has_source(clip.source_id)) return false;
  if (!(clip.length_ppq > 0.0) || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0) {
    return false;
  }
  // Under LOOP, 0 is valid and means "loop the entire clip"; reject negatives/NaN.
  if (clip.loop_mode == LoopMode::kLoop && !(clip.loop_length_ppq >= 0.0)) return false;
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq, ignore_clip_id)) {
    return false;
  }
  return true;
}

inline bool source_matches_track_kind(const Project& project, TrackId track_id,
                                      SourceId source_id) {
  const Track* track = project.find_track(track_id);
  const ClipSource* source = project.find_source(source_id);
  if (track == nullptr || source == nullptr) return false;
  const SourceKind kind = source_kind(*source);
  if (track->kind == Track::Kind::kAudio) return kind == SourceKind::kAudio;
  if (track->kind == Track::Kind::kMidi) return kind == SourceKind::kMidi;
  return false;
}

inline bool take_id_exists(const std::vector<ClipTake>& takes, TakeId id) {
  if (id == 0) return true;
  return std::any_of(takes.begin(), takes.end(),
                     [id](const ClipTake& take) { return take.id == id; });
}

inline bool clip_is_on_midi_track(const Project& project, const EditClip& clip) {
  const Track* track = project.find_track(clip.track_id);
  return track != nullptr && track->kind == Track::Kind::kMidi;
}

inline bool valid_clip_takes(const Project& project, const EditClip& clip,
                             const std::vector<ClipTake>& takes, TakeId active_take_id) {
  if (clip_is_on_midi_track(project, clip) && (!takes.empty() || active_take_id != 0)) {
    return false;
  }
  std::vector<TakeId> ids;
  ids.reserve(takes.size());
  for (const ClipTake& take : takes) {
    if (take.id == 0 || take.source_offset_ppq < 0.0 || !std::isfinite(take.source_offset_ppq)) {
      return false;
    }
    if (std::find(ids.begin(), ids.end(), take.id) != ids.end()) {
      return false;
    }
    ids.push_back(take.id);
    const SourceId source_id = take.source_id == 0 ? clip.source_id : take.source_id;
    if (!source_matches_track_kind(project, clip.track_id, source_id)) {
      return false;
    }
  }
  return take_id_exists(takes, active_take_id);
}

inline bool valid_comp_segments(const std::vector<ClipTake>& takes,
                                const std::vector<ClipCompSegment>& segments,
                                double clip_length_ppq) {
  double previous_end = 0.0;
  for (const ClipCompSegment& segment : segments) {
    if (!std::isfinite(segment.start_ppq) || !std::isfinite(segment.end_ppq) ||
        segment.start_ppq < 0.0 || !(segment.end_ppq > segment.start_ppq) ||
        segment.end_ppq > clip_length_ppq || segment.start_ppq < previous_end ||
        !take_id_exists(takes, segment.take_id)) {
      return false;
    }
    previous_end = segment.end_ppq;
  }
  return true;
}

inline bool comp_segments_split_clip(const std::vector<ClipCompSegment>& segments,
                                     double clip_length_ppq) {
  if (segments.empty()) return false;
  return segments.size() != 1 || segments.front().start_ppq != 0.0 ||
         segments.front().end_ppq != clip_length_ppq;
}

inline std::vector<ClipCompSegment> shifted_clamped_comp_segments(
    const std::vector<ClipCompSegment>& segments, double delta_ppq, double clip_length_ppq) {
  std::vector<ClipCompSegment> out;
  out.reserve(segments.size());
  for (ClipCompSegment segment : segments) {
    segment.start_ppq = std::max(0.0, segment.start_ppq + delta_ppq);
    segment.end_ppq = std::min(clip_length_ppq, segment.end_ppq + delta_ppq);
    if (segment.end_ppq > segment.start_ppq) {
      out.push_back(segment);
    }
  }
  return out;
}

inline bool shift_take_offsets(std::vector<ClipTake>* takes, double delta_ppq) {
  if (takes == nullptr) return false;
  for (ClipTake& take : *takes) {
    const double shifted = take.source_offset_ppq + delta_ppq;
    if (shifted < 0.0 || !std::isfinite(shifted)) {
      return false;
    }
    take.source_offset_ppq = shifted;
  }
  return true;
}

struct RemovedTrackClipSnapshot {
  EditClip clip;
  size_t index = Project::kAppend;
  MidiClipEventList events;
  std::map<uint32_t, std::vector<uint8_t>> sysex_payloads;
  bool has_events = false;
};

class RestoreClip final : public EditCommand {
 public:
  explicit RestoreClip(EditClip clip) : clip_(std::move(clip)) {}

  bool apply(Project& project, MidiContentStore& /*store*/) override {
    EditClip* clip = project.find_clip_mutable(clip_.id);
    if (clip == nullptr) {
      return false;
    }
    *clip = clip_;
    return true;
  }

  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& /*store_before*/) const override {
    const EditClip* clip = before.find_clip(clip_.id);
    if (clip == nullptr) {
      return nullptr;
    }
    return std::make_unique<RestoreClip>(*clip);
  }

  const char* type_name() const noexcept override { return "RestoreClip"; }

 private:
  EditClip clip_;
};

class RestoreTrackWithClips final : public EditCommand {
 public:
  RestoreTrackWithClips(Track track, size_t track_index,
                        std::vector<RemovedTrackClipSnapshot> clips)
      : track_(std::move(track)), track_index_(track_index), clips_(std::move(clips)) {}

  bool apply(Project& project, MidiContentStore& store) override {
    if (track_.id == 0 || project.has_track(track_.id)) {
      return false;
    }
    for (const RemovedTrackClipSnapshot& snapshot : clips_) {
      const EditClip& clip = snapshot.clip;
      if (clip.id == 0 || project.has_clip(clip.id) || clip.track_id != track_.id ||
          !project.has_source(clip.source_id)) {
        return false;
      }
    }

    if (!project.insert_track_raw(track_, track_index_)) {
      return false;
    }
    project.ensure_next_track_id(track_.id);

    for (const RemovedTrackClipSnapshot& snapshot : clips_) {
      if (!project.insert_clip_raw(snapshot.clip, snapshot.index)) {
        return false;
      }
      project.ensure_next_clip_id(snapshot.clip.id);
      if (snapshot.has_events) {
        store.events[snapshot.clip.id] = snapshot.events;
      }
      for (const auto& [handle, payload] : snapshot.sysex_payloads) {
        store.sysex_payloads[handle] = payload;
      }
    }
    return true;
  }

  EditCommandPtr invert(const Project& /*before*/,
                        const MidiContentStore& /*store_before*/) const override {
    return std::make_unique<RemoveTrack>(track_.id);
  }

  const char* type_name() const noexcept override { return "RestoreTrackWithClips"; }

 private:
  Track track_;
  size_t track_index_ = Project::kAppend;
  std::vector<RemovedTrackClipSnapshot> clips_;
};

}  // namespace detail
}  // namespace sonare::arrangement
