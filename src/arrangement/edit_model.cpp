/// @file edit_model.cpp
/// @brief Implementation of the headless arrangement project model.

#include "arrangement/edit_model.h"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace sonare::arrangement {

namespace {

template <typename Id>
Id allocate_entity_id(Id& next) noexcept {
  static_assert(std::is_same_v<Id, uint32_t>);
  // 0 is the public failure/sentinel value and UINT32_MAX is reserved as the
  // exhausted-counter marker. Never increment either value.
  if (next == 0 || next == std::numeric_limits<Id>::max()) return 0;
  return next++;
}

template <typename Id>
void ensure_next_entity_id(Id id, Id& next) noexcept {
  const Id reserved = std::numeric_limits<Id>::max();
  if (id >= next) next = id >= reserved - 1 ? reserved : static_cast<Id>(id + 1);
}

}  // namespace

SourceId Project::add_audio_source(AudioSourceRef ref) {
  const SourceId id = allocate_entity_id(next_source_id_);
  if (id == 0) return 0;
  ref.id = id;
  sources_.emplace_back(std::move(ref));
  return id;
}

SourceId Project::add_midi_source(MidiSourceRef ref) {
  const SourceId id = allocate_entity_id(next_source_id_);
  if (id == 0) return 0;
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
  const TrackId id = allocate_entity_id(next_track_id_);
  if (id == 0) return 0;
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
  // Loop policy validation. Under LOOP, 0 is allowed and means "loop the entire
  // clip"; only negatives and NaN are rejected.
  if (clip.loop_mode == LoopMode::kLoop && !(clip.loop_length_ppq >= 0.0)) {
    return 0;
  }
  // Overlap policy.
  if (overlap_policy_ == OverlapPolicy::kDisallow &&
      clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq)) {
    return 0;
  }

  const ClipId id = allocate_entity_id(next_clip_id_);
  if (id == 0) return 0;
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
  // Anchors must define a well-formed forward mapping: each offset finite and
  // non-negative, and strictly increasing in both the warp and source axes. This
  // is the interpolation contract the C ABI enforces at its boundary; applying it
  // in the setter means every insertion path (edit commands and JSON project
  // load) shares it, so a hand-authored or corrupt project file can no longer
  // install a warp map that plays back mistimed or garbled.
  const WarpAnchorRef* prev = nullptr;
  for (const WarpAnchorRef& a : map.anchors) {
    if (!(std::isfinite(a.warp_sample) && a.warp_sample >= 0.0) ||
        !(std::isfinite(a.source_sample) && a.source_sample >= 0.0)) {
      return false;
    }
    if (prev != nullptr &&
        !(a.warp_sample > prev->warp_sample && a.source_sample > prev->source_sample)) {
      return false;
    }
    prev = &a;
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
  const uint32_t id = allocate_entity_id(next_marker_id_);
  if (id == 0) return 0;
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
  if (clip.id == 0 || clip.id == std::numeric_limits<ClipId>::max() || has_clip(clip.id)) {
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
  if (track.id == 0 || track.id == std::numeric_limits<TrackId>::max() || has_track(track.id)) {
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
  if (id == 0 || id == std::numeric_limits<SourceId>::max() || has_source(id)) {
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
  ensure_next_entity_id(id, next_source_id_);
}

void Project::ensure_next_track_id(TrackId id) noexcept {
  ensure_next_entity_id(id, next_track_id_);
}

void Project::ensure_next_clip_id(ClipId id) noexcept { ensure_next_entity_id(id, next_clip_id_); }

void Project::ensure_next_marker_id(uint32_t id) noexcept {
  ensure_next_entity_id(id, next_marker_id_);
}

}  // namespace sonare::arrangement
