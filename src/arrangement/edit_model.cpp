/// @file edit_model.cpp
/// @brief Implementation of the headless arrangement project model.

#include "arrangement/edit_model.h"

#include <utility>

namespace sonare::arrangement {

SourceId Project::add_audio_source(AudioSourceRef ref) {
  const SourceId id = next_source_id_++;
  ref.id = id;
  sources_.emplace_back(std::move(ref));
  return id;
}

SourceId Project::add_midi_source(MidiSourceRef ref) {
  const SourceId id = next_source_id_++;
  ref.id = id;
  sources_.emplace_back(std::move(ref));
  return id;
}

const ClipSource* Project::find_source(SourceId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const ClipSource& s : sources_) {
    if (source_id(s) == id) {
      return &s;
    }
  }
  return nullptr;
}

TrackId Project::add_track(Track track) {
  const TrackId id = next_track_id_++;
  track.id = id;
  tracks_.emplace_back(std::move(track));
  return id;
}

const Track* Project::find_track(TrackId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const Track& t : tracks_) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

Track* Project::find_track_mutable(TrackId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (Track& t : tracks_) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

bool Project::clip_overlaps(TrackId track_id, double start_ppq, double length_ppq,
                            ClipId ignore_clip_id) const noexcept {
  const double end_ppq = start_ppq + length_ppq;
  for (const EditClip& c : clips_) {
    if (c.track_id != track_id || c.id == ignore_clip_id) {
      continue;
    }
    // Half-open interval overlap; touching endpoints (adjacency) do not count.
    if (start_ppq < c.end_ppq() && c.start_ppq < end_ppq) {
      return true;
    }
  }
  return false;
}

ClipId Project::add_clip(EditClip clip) {
  // Referential integrity.
  if (!has_track(clip.track_id) || !has_source(clip.source_id)) {
    return 0;
  }
  // PPQ range validation.
  if (!(clip.length_ppq > 0.0) || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0) {
    return 0;
  }
  // Loop policy validation.
  if (clip.loop_mode == LoopMode::kLoop && !(clip.loop_length_ppq > 0.0)) {
    return 0;
  }
  // Overlap policy.
  if (overlap_policy_ == OverlapPolicy::kDisallow &&
      clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq)) {
    return 0;
  }

  const ClipId id = next_clip_id_++;
  clip.id = id;
  clips_.emplace_back(std::move(clip));
  return id;
}

const EditClip* Project::find_clip(ClipId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const EditClip& c : clips_) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

EditClip* Project::find_clip_mutable(ClipId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (EditClip& c : clips_) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

const WarpMapRef* Project::find_warp_map(WarpRefId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const WarpMapRef& map : warp_maps_) {
    if (map.id == id) {
      return &map;
    }
  }
  return nullptr;
}

WarpMapRef* Project::find_warp_map_mutable(WarpRefId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (WarpMapRef& map : warp_maps_) {
    if (map.id == id) {
      return &map;
    }
  }
  return nullptr;
}

bool Project::set_warp_map(WarpMapRef map) {
  if (map.id == 0) {
    return false;
  }
  if (WarpMapRef* existing = find_warp_map_mutable(map.id)) {
    *existing = std::move(map);
    return true;
  }
  warp_maps_.emplace_back(std::move(map));
  return true;
}

std::pair<WarpMapRef, bool> Project::remove_warp_map(WarpRefId id) {
  for (auto it = warp_maps_.begin(); it != warp_maps_.end(); ++it) {
    if (it->id == id) {
      WarpMapRef removed = std::move(*it);
      warp_maps_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {WarpMapRef{}, false};
}

uint32_t Project::add_marker(double ppq, std::string name, uint8_t kind, int8_t key_fifths,
                             bool key_minor) {
  const uint32_t id = next_marker_id_++;
  ProjectMarker marker;
  marker.ppq = ppq;
  marker.id = id;
  marker.name = std::move(name);
  marker.kind = kind;
  marker.key_fifths = key_fifths;
  marker.key_minor = key_minor;
  markers_.emplace_back(std::move(marker));
  return id;
}

std::pair<Track, bool> Project::remove_track(TrackId id) {
  for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
    if (it->id == id) {
      Track removed = std::move(*it);
      tracks_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {Track{}, false};
}

std::pair<EditClip, bool> Project::remove_clip(ClipId id) {
  for (auto it = clips_.begin(); it != clips_.end(); ++it) {
    if (it->id == id) {
      EditClip removed = *it;
      clips_.erase(it);
      return {removed, true};
    }
  }
  return {EditClip{}, false};
}

bool Project::insert_clip_raw(EditClip clip, size_t index) {
  if (clip.id == 0 || has_clip(clip.id)) {
    return false;
  }
  if (index >= clips_.size()) {
    clips_.emplace_back(std::move(clip));
  } else {
    clips_.insert(clips_.begin() + static_cast<std::ptrdiff_t>(index), std::move(clip));
  }
  return true;
}

bool Project::insert_track_raw(Track track, size_t index) {
  if (track.id == 0 || has_track(track.id)) {
    return false;
  }
  if (index >= tracks_.size()) {
    tracks_.emplace_back(std::move(track));
  } else {
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(track));
  }
  return true;
}

ClipSource* Project::find_source_mutable(SourceId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (ClipSource& s : sources_) {
    if (source_id(s) == id) {
      return &s;
    }
  }
  return nullptr;
}

bool Project::insert_source_raw(ClipSource source, size_t index) {
  const SourceId id = source_id(source);
  if (id == 0 || has_source(id)) {
    return false;
  }
  if (index >= sources_.size()) {
    sources_.emplace_back(std::move(source));
  } else {
    sources_.insert(sources_.begin() + static_cast<std::ptrdiff_t>(index), std::move(source));
  }
  return true;
}

size_t Project::track_index(TrackId id) const noexcept {
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (tracks_[i].id == id) {
      return i;
    }
  }
  return kAppend;
}

size_t Project::clip_index(ClipId id) const noexcept {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (clips_[i].id == id) {
      return i;
    }
  }
  return kAppend;
}

size_t Project::source_index(SourceId id) const noexcept {
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (source_id(sources_[i]) == id) {
      return i;
    }
  }
  return kAppend;
}

std::pair<ClipSource, bool> Project::remove_source(SourceId id) {
  for (auto it = sources_.begin(); it != sources_.end(); ++it) {
    if (source_id(*it) == id) {
      ClipSource removed = std::move(*it);
      sources_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {ClipSource{}, false};
}

void Project::ensure_next_source_id(SourceId id) noexcept {
  if (id >= next_source_id_) {
    next_source_id_ = id + 1;
  }
}

void Project::ensure_next_track_id(TrackId id) noexcept {
  if (id >= next_track_id_) {
    next_track_id_ = id + 1;
  }
}

void Project::ensure_next_clip_id(ClipId id) noexcept {
  if (id >= next_clip_id_) {
    next_clip_id_ = id + 1;
  }
}

void Project::ensure_next_marker_id(uint32_t id) noexcept {
  if (id >= next_marker_id_) {
    next_marker_id_ = id + 1;
  }
}

}  // namespace sonare::arrangement
