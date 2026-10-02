#pragma once

/// @file edit_command_internal.h
/// @brief Shared file-local helpers for the arrangement edit-command TUs.
///
/// These validation/snapshot helpers and inverse-restore command classes are
/// implementation details of the edit-command split TUs. They are not part of
/// any public surface and live in @ref sonare::arrangement::detail so the
/// per-domain edit_command_*.cpp files can share a single definition.
///
/// The comp-segment well-formedness rule (@ref take_id_exists, @ref
/// valid_comp_segments) is shared with edit_compiler.cpp as well: a segment list
/// an edit command accepts must be one the compiler accepts, so the two paths
/// cannot be allowed to carry separate copies of the rule.

#include <algorithm>
#include <cmath>
#include <limits>
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
  if (!(clip.length_ppq > 0.0) || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0 ||
      (clip.source_offset_seconds.has_value() &&
       !(std::isfinite(*clip.source_offset_seconds) && *clip.source_offset_seconds >= 0.0))) {
    return false;
  }
  // Under LOOP, 0 is valid and means "loop the entire clip"; reject negatives/NaN.
  if (clip.loop_mode == LoopMode::kLoop && !(clip.loop_length_ppq >= 0.0)) return false;
  if (clip.loop_anchor.has_value()) {
    const LoopAnchor& anchor = *clip.loop_anchor;
    if (!(std::isfinite(anchor.period_seconds) && anchor.period_seconds > 0.0 &&
          std::isfinite(anchor.phase_seconds))) {
      return false;
    }
  }
  for (const ClipTake& take : clip.takes) {
    if (take.id == 0 || !std::isfinite(take.source_offset_ppq) || take.source_offset_ppq < 0.0 ||
        (take.source_offset_seconds.has_value() &&
         !(std::isfinite(*take.source_offset_seconds) && *take.source_offset_seconds >= 0.0))) {
      return false;
    }
  }
  if (!valid_clip_render_parts(clip)) return false;
  if (project.overlap_policy() == OverlapPolicy::kDisallow &&
      project.clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq, ignore_clip_id)) {
    return false;
  }
  return true;
}

// Whether a track of @p track_kind can hold a clip whose source is @p kind.
// An aux track holds no clips, so it accepts neither source kind.
inline bool track_kind_accepts_source_kind(Track::Kind track_kind, SourceKind kind) {
  if (track_kind == Track::Kind::kAudio) return kind == SourceKind::kAudio;
  if (track_kind == Track::Kind::kMidi) return kind == SourceKind::kMidi;
  return false;
}

inline bool source_matches_track_kind(const Project& project, TrackId track_id,
                                      SourceId source_id) {
  const Track* track = project.find_track(track_id);
  const ClipSource* source = project.find_source(source_id);
  if (track == nullptr || source == nullptr) return false;
  return track_kind_accepts_source_kind(track->kind, source_kind(*source));
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
    if (take.id == 0 || take.source_offset_ppq < 0.0 || !std::isfinite(take.source_offset_ppq) ||
        (take.source_offset_seconds.has_value() &&
         !(std::isfinite(*take.source_offset_seconds) && *take.source_offset_seconds >= 0.0))) {
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

/// Builds the control-thread tempo map used by source-offset materialization.
/// The edit model stores plain tempo segments, so this keeps all PPQ->sample
/// conversion in one place and mirrors the arrangement compiler's setup.
inline bool make_edit_tempo_map(const Project& project, transport::TempoMap* out) {
  if (out == nullptr || !(std::isfinite(project.sample_rate()) && project.sample_rate() > 0.0)) {
    return false;
  }
  out->prepare(project.sample_rate());
  out->set_segments(project.tempo_segments());
  return true;
}

inline bool timeline_delta_seconds(const transport::TempoMap& tempo_map, double from_ppq,
                                   double to_ppq, double sample_rate, double* out) {
  if (out == nullptr || !std::isfinite(from_ppq) || !std::isfinite(to_ppq) ||
      !(std::isfinite(sample_rate) && sample_rate > 0.0)) {
    return false;
  }
  const int64_t from_sample = tempo_map.ppq_to_sample(from_ppq);
  const int64_t to_sample = tempo_map.ppq_to_sample(to_ppq);
  const long double delta =
      static_cast<long double>(to_sample) - static_cast<long double>(from_sample);
  const double seconds = static_cast<double>(delta / static_cast<long double>(sample_rate));
  if (!std::isfinite(seconds)) return false;
  *out = seconds;
  return true;
}

/// Materializes legacy PPQ source offsets on an audio clip before a
/// tempo-sensitive edit. The caller passes a private clip copy when the edit
/// must remain atomic. Existing physical values are retained verbatim.
inline bool materialize_audio_source_offsets(const Project& project, EditClip* clip,
                                             transport::TempoMap* tempo_map = nullptr) {
  if (clip == nullptr) return false;
  const ClipSource* source = project.find_source(clip->source_id);
  if (source == nullptr || source_kind(*source) != SourceKind::kAudio) return true;

  transport::TempoMap local_map;
  transport::TempoMap* map = tempo_map != nullptr ? tempo_map : &local_map;
  if (tempo_map == nullptr && !make_edit_tempo_map(project, map)) return false;
  const double sample_rate = project.sample_rate();
  const auto materialize = [&](double offset_ppq, std::optional<double>* physical) {
    if (physical == nullptr) return false;
    if (physical->has_value()) {
      return std::isfinite(**physical) && **physical >= 0.0;
    }
    if (!(std::isfinite(offset_ppq) && offset_ppq >= 0.0) || !std::isfinite(clip->start_ppq) ||
        !std::isfinite(clip->start_ppq + offset_ppq)) {
      return false;
    }
    double seconds = 0.0;
    if (!timeline_delta_seconds(*map, clip->start_ppq, clip->start_ppq + offset_ppq, sample_rate,
                                &seconds) ||
        seconds < 0.0) {
      return false;
    }
    *physical = seconds;
    return true;
  };

  if (!materialize(clip->source_offset_ppq, &clip->source_offset_seconds)) return false;
  for (ClipTake& take : clip->takes) {
    if (!materialize(take.source_offset_ppq, &take.source_offset_seconds)) return false;
  }
  return true;
}

inline bool shift_physical_source_offsets(EditClip* clip, double delta_seconds) {
  if (clip == nullptr || !std::isfinite(delta_seconds)) return false;
  if (clip->source_offset_seconds.has_value()) {
    const double shifted = *clip->source_offset_seconds + delta_seconds;
    if (!(std::isfinite(shifted) && shifted >= 0.0)) return false;
    clip->source_offset_seconds = shifted;
  }
  for (ClipTake& take : clip->takes) {
    if (!take.source_offset_seconds.has_value()) continue;
    const double shifted = *take.source_offset_seconds + delta_seconds;
    if (!(std::isfinite(shifted) && shifted >= 0.0)) return false;
    take.source_offset_seconds = shifted;
  }
  return true;
}

inline bool valid_comp_segments(const std::vector<ClipTake>& takes,
                                const std::vector<ClipCompSegment>& segments,
                                double clip_length_ppq) {
  double previous_end = 0.0;
  // Start of the part that plays immediately before the segment under test: the
  // previous segment when the two are contiguous, otherwise the fallback part
  // filling the gap. A crossfade is taken from inside it, so it bounds the fade.
  double preceding_start = 0.0;
  for (const ClipCompSegment& segment : segments) {
    if (!std::isfinite(segment.start_ppq) || !std::isfinite(segment.end_ppq) ||
        segment.start_ppq < 0.0 || !(segment.end_ppq > segment.start_ppq) ||
        segment.end_ppq > clip_length_ppq || segment.start_ppq < previous_end ||
        !take_id_exists(takes, segment.take_id)) {
      return false;
    }
    // A segment at 0 has nothing in front of it, so this resolves to 0 and the
    // check below admits only a 0 crossfade.
    const double preceding_length = segment.start_ppq > previous_end
                                        ? segment.start_ppq - previous_end
                                        : segment.start_ppq - preceding_start;
    if (!std::isfinite(segment.crossfade_ppq) || segment.crossfade_ppq < 0.0 ||
        segment.crossfade_ppq > segment.end_ppq - segment.start_ppq ||
        segment.crossfade_ppq > preceding_length) {
      return false;
    }
    preceding_start = segment.start_ppq;
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

inline double canonicalize_ppq_endpoint(double value, double target) {
  if (!std::isfinite(value) || !std::isfinite(target)) return value;
  return ppq_nearly_equal(value, target) ? target : value;
}

inline std::vector<ClipCompSegment> shifted_clamped_comp_segments(
    const std::vector<ClipCompSegment>& segments, double delta_ppq, double clip_length_ppq) {
  std::vector<ClipCompSegment> out;
  out.reserve(segments.size());
  for (ClipCompSegment segment : segments) {
    segment.start_ppq = std::max(0.0, segment.start_ppq + delta_ppq);
    segment.end_ppq = std::min(clip_length_ppq, segment.end_ppq + delta_ppq);
    segment.start_ppq = canonicalize_ppq_endpoint(segment.start_ppq, 0.0);
    segment.end_ppq = canonicalize_ppq_endpoint(segment.end_ppq, clip_length_ppq);
    if (segment.end_ppq > segment.start_ppq) {
      // A clamped segment is shorter than it was, and a segment pushed to 0 has
      // lost what it faded over, so the fade follows the span rather than
      // outliving it and failing revalidation.
      segment.crossfade_ppq =
          std::min({segment.crossfade_ppq, segment.end_ppq - segment.start_ppq, segment.start_ppq});
      out.push_back(segment);
    }
  }
  return out;
}

/// Materializes the compiler's comp fragments before an edit. The authored
/// segments stay the editable source of truth; the fragments carry seam
/// references and retained_only parts so hidden comp content survives a cut/trim
/// and is restored when the clip is re-extended.
inline bool materialize_comp_render_parts(EditClip* clip) {
  if (clip == nullptr) return false;
  if (!clip->comp_render_parts.empty()) return valid_clip_render_parts(*clip);
  if (clip->comp_segments.empty()) return true;
  if (!valid_comp_segments(clip->takes, clip->comp_segments, clip->length_ppq)) return false;

  std::vector<ClipCompRenderPart> parts;
  const TakeId fallback_take_id = clip->active_take_id;
  const auto push = [&](TakeId take_id, double visible_start, double visible_end,
                        double seam_fade_in) {
    if (!(visible_end > visible_start)) return;
    ClipCompRenderPart part;
    part.visible_start_ppq = visible_start;
    part.visible_end_ppq = visible_end;
    part.reference_start_ppq = visible_start;
    part.reference_end_ppq = visible_end;
    part.take_id = take_id;
    part.seam_fade_in_ppq = seam_fade_in;
    if (seam_fade_in > 0.0 && !parts.empty()) {
      parts.back().seam_fade_out_ppq = seam_fade_in;
    }
    parts.push_back(part);
  };

  double cursor = 0.0;
  for (const ClipCompSegment& segment : clip->comp_segments) {
    if (segment.start_ppq > cursor) {
      push(fallback_take_id, cursor, segment.start_ppq, 0.0);
    }
    const double render_start = segment.start_ppq - segment.crossfade_ppq;
    push(segment.take_id == 0 ? fallback_take_id : segment.take_id, render_start, segment.end_ppq,
         segment.crossfade_ppq);
    cursor = segment.end_ppq;
  }
  if (cursor < clip->length_ppq) {
    push(fallback_take_id, cursor, clip->length_ppq, 0.0);
  }
  clip->comp_render_parts = std::move(parts);
  return valid_clip_render_parts(*clip);
}

/// Clips render fragments to [window_start, window_end) in the old clip-local
/// domain and shifts the retained geometry into the new clip-local origin.
inline bool remap_comp_render_parts(const std::vector<ClipCompRenderPart>& source,
                                    double window_start, double window_end, double shift,
                                    std::vector<ClipCompRenderPart>* out,
                                    TakeId fallback_take_id = 0) {
  if (out == nullptr || !(std::isfinite(window_start) && std::isfinite(window_end) &&
                          window_end > window_start && std::isfinite(shift))) {
    return false;
  }
  out->clear();
  const auto append_fallback = [&](double start, double end) {
    if (!(end > start)) return;
    ClipCompRenderPart fallback;
    fallback.visible_start_ppq = start - shift;
    fallback.visible_end_ppq = end - shift;
    fallback.reference_start_ppq = fallback.visible_start_ppq;
    fallback.reference_end_ppq = fallback.visible_end_ppq;
    fallback.take_id = fallback_take_id;
    fallback.retained_only = false;
    out->push_back(fallback);
  };
  double covered_until = window_start;
  bool appended_trailing_fallback = false;
  for (const ClipCompRenderPart& part : source) {
    const double visible_start = std::max(part.reference_start_ppq, window_start);
    const double visible_end = std::min(part.reference_end_ppq, window_end);
    ClipCompRenderPart mapped = part;
    mapped.reference_start_ppq = part.reference_start_ppq - shift;
    mapped.reference_end_ppq = part.reference_end_ppq - shift;
    if (visible_end > visible_start) {
      if (visible_start > covered_until) append_fallback(covered_until, visible_start);
      mapped.visible_start_ppq = canonicalize_ppq_endpoint(visible_start - shift, 0.0);
      mapped.visible_end_ppq = canonicalize_ppq_endpoint(visible_end - shift, window_end - shift);
      mapped.retained_only = false;
      // Keep the complete seam reference domain even when the visible fragment
      // is clipped. The player uses this hidden range to evaluate the fade at
      // the same phase after a split or trim.
      mapped.reference_start_ppq =
          canonicalize_ppq_endpoint(mapped.reference_start_ppq, mapped.visible_start_ppq);
      mapped.reference_end_ppq =
          canonicalize_ppq_endpoint(mapped.reference_end_ppq, mapped.visible_end_ppq);
      covered_until = std::max(covered_until, visible_end);
    } else {
      // Preserve a part whose full seam reference is currently outside the
      // visible window. A later extension can intersect it and revive the
      // original take/fade; a zero-width visible sentinel keeps it out of the
      // compiler and visible coverage calculations.
      mapped.visible_start_ppq = 0.0;
      mapped.visible_end_ppq = 0.0;
      mapped.retained_only = true;
      if (part.reference_start_ppq >= window_end && covered_until < window_end) {
        append_fallback(covered_until, window_end);
        appended_trailing_fallback = true;
        covered_until = window_end;
      }
    }
    out->push_back(mapped);
  }
  if (!appended_trailing_fallback && covered_until < window_end) {
    append_fallback(covered_until, window_end);
  }
  return valid_comp_render_geometry(*out, window_end - shift);
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
  std::size_t retained_bytes() const noexcept override;
  bool mutates_midi_store() const noexcept override { return false; }

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
  std::size_t retained_bytes() const noexcept override;

 private:
  Track track_;
  size_t track_index_ = Project::kAppend;
  std::vector<RemovedTrackClipSnapshot> clips_;
};

}  // namespace detail
}  // namespace sonare::arrangement
