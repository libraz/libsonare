#include <algorithm>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

// Pin the C automation-curve ordinals to their C++ enum so reordering a C++
// enum is caught at compile time (these flat-POD ordinals are part of the
// project ABI).
static_assert(static_cast<int>(sonare::AutomationCurve::Linear) == SONARE_CURVE_LINEAR,
              "SonareProjectAutomationCurve linear ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::Exponential) == SONARE_CURVE_EXPONENTIAL,
              "SonareProjectAutomationCurve exponential ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::Hold) == SONARE_CURVE_HOLD,
              "SonareProjectAutomationCurve hold ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::SCurve) == SONARE_CURVE_SCURVE,
              "SonareProjectAutomationCurve scurve ordinal drift");
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kOpaque) ==
                  SONARE_AUTOMATION_TARGET_OPAQUE,
              "SonareAutomationTargetKind opaque ordinal drift");
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kTrackFaderDb) ==
                  SONARE_AUTOMATION_TARGET_TRACK_FADER_DB,
              "SonareAutomationTargetKind fader ordinal drift");
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kTrackPan) ==
                  SONARE_AUTOMATION_TARGET_TRACK_PAN,
              "SonareAutomationTargetKind pan ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kAudio) == SONARE_TRACK_AUDIO,
              "SonareProjectTrackKind audio ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kMidi) == SONARE_TRACK_MIDI,
              "SonareProjectTrackKind midi ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kAux) == SONARE_TRACK_AUX,
              "SonareProjectTrackKind aux ordinal drift");

namespace {

// Validates lane descriptor fields and builds an automation::AutomationLane.
// The target parameter id must be non-zero; each breakpoint's ppq must be
// finite and >= 0, its value finite, and its curve ordinal in range.
SonareError automation_lane_from_values(uint32_t target_param_id, uint32_t target_kind,
                                        const SonareAutomationPoint* point_data, size_t point_count,
                                        sonare::automation::AutomationLane* out) {
  if (out == nullptr || target_param_id == 0 || target_kind > SONARE_AUTOMATION_TARGET_TRACK_PAN) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (point_count > 0 && point_data == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (point_count > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<sonare::automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    const SonareAutomationPoint& p = point_data[i];
    if (!finite_non_negative(p.ppq) || !std::isfinite(p.value)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (p.curve_to_next < 0 || static_cast<uint32_t>(p.curve_to_next) >
                                   static_cast<uint32_t>(sonare::AutomationCurve::SCurve)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::automation::Breakpoint bp;
    bp.ppq = p.ppq;
    bp.value = p.value;
    bp.curve_to_next = static_cast<sonare::AutomationCurve>(p.curve_to_next);
    breakpoints.push_back(bp);
  }
  out->set_target_param_id(target_param_id);
  out->set_target_kind(static_cast<sonare::automation::AutomationTargetKind>(target_kind));
  out->set_points(std::move(breakpoints));
  return SONARE_OK;
}

SonareError automation_lane_from_desc(const SonareAutomationLaneDesc* desc,
                                      sonare::automation::AutomationLane* out) {
  if (desc == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return automation_lane_from_values(desc->target_param_id, SONARE_AUTOMATION_TARGET_OPAQUE,
                                     desc->points, desc->point_count, out);
}

SonareError automation_lane_from_desc_ex(const SonareAutomationLaneDescEx* desc,
                                         sonare::automation::AutomationLane* out) {
  if (desc == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return automation_lane_from_values(desc->target_param_id,
                                     static_cast<uint32_t>(desc->target_kind), desc->points,
                                     desc->point_count, out);
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// Edit
// ============================================================================

SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_track_id) *out_track_id = 0;
  if (!project || !desc || !out_track_id) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->kind < SONARE_TRACK_AUDIO || desc->kind > SONARE_TRACK_AUX) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::Track track;
  track.kind = static_cast<arr::Track::Kind>(desc->kind);
  if (desc->name) track.name = desc->name;
  auto command = std::make_unique<arr::AddTrack>(std::move(track));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  // The command may be evicted immediately when the byte cap is tiny. Read
  // the committed model instead of dereferencing a command that may already
  // have been destroyed.
  if (project->history.project().tracks().empty()) return SONARE_ERROR_INVALID_STATE;
  *out_track_id = project->history.project().tracks().back().id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_track_id);
#endif
}

SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || overlap_policy > SONARE_PROJECT_OVERLAP_ALLOW) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command =
      std::make_unique<arr::SetOverlapPolicy>(static_cast<arr::OverlapPolicy>(overlap_policy));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, overlap_policy);
#endif
}

SonareError sonare_project_set_tempo_segments(SonareProject* project,
                                              const SonareProjectTempoSegment* segments,
                                              size_t segment_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || (segment_count > 0 && !segments) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<sonare::transport::TempoSegment> out;
  out.reserve(segment_count);
  double previous_start_ppq = -1.0;
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTempoSegment& in = segments[i];
    if (!finite_non_negative(in.start_ppq) || !finite_positive(in.bpm) ||
        !std::isfinite(in.end_bpm) || in.end_bpm < 0.0 || in.start_ppq <= previous_start_ppq) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::transport::TempoSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.bpm = in.bpm;
    seg.start_sample = 0.0;
    seg.end_bpm = in.end_bpm;
    out.push_back(seg);
    previous_start_ppq = in.start_ppq;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTempoSegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, segments, segment_count);
#endif
}

SonareError sonare_project_set_time_signatures(SonareProject* project,
                                               const SonareProjectTimeSignatureSegment* segments,
                                               size_t segment_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || (segment_count > 0 && !segments) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<sonare::transport::TimeSignatureSegment> out;
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTimeSignatureSegment& in = segments[i];
    if (!finite_non_negative(in.start_ppq) || in.numerator <= 0 || in.denominator <= 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::transport::TimeSignatureSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.time_sig.numerator = in.numerator;
    seg.time_sig.denominator = in.denominator;
    out.push_back(seg);
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTimeSignatureSegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, segments, segment_count);
#endif
}

SonareError sonare_project_set_marker(SonareProject* project, uint32_t marker_id, double ppq,
                                      const char* name, uint32_t* out_marker_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_marker_id) *out_marker_id = 0;
  if (!project || !out_marker_id || !finite_non_negative(ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetMarker>(marker_id, ppq, name ? name : "");
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  // Read the committed model, not the command: history can evict the entry that
  // owned it. An allocating SetMarker always appends, so the tail is the new
  // marker; guard anyway so a future no-append path cannot read an empty vector.
  if (marker_id == 0 && project->history.project().markers().empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_marker_id = marker_id != 0 ? marker_id : project->history.project().markers().back().id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, marker_id, ppq, name, out_marker_id);
#endif
}

SonareError sonare_project_set_marker_ex(SonareProject* project, const SonareProjectMarker* marker,
                                         uint32_t* out_marker_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_marker_id) *out_marker_id = 0;
  if (!project || !out_marker_id || !marker || !finite_non_negative(marker->ppq) ||
      marker->kind > SONARE_MARKER_KIND_KEY_SIGNATURE) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Key signatures encode the SMF `sf` byte, which is constrained to -7..7
  // (7 flats to 7 sharps). Reject out-of-range fifths so the marker cannot
  // serialize to a non-conformant SMF key signature.
  if (marker->kind == SONARE_MARKER_KIND_KEY_SIGNATURE &&
      (marker->key_fifths < -7 || marker->key_fifths > 7)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const char* name_end = std::find(marker->name, marker->name + sizeof(marker->name), '\0');
  std::string name(marker->name, name_end);
  auto command =
      std::make_unique<arr::SetMarker>(marker->id, marker->ppq, std::move(name), marker->kind,
                                       marker->key_fifths, marker->key_minor != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (marker->id == 0 && project->history.project().markers().empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_marker_id = marker->id != 0 ? marker->id : project->history.project().markers().back().id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, marker, out_marker_id);
#endif
}

SonareError sonare_project_set_marker_ex_name(SonareProject* project,
                                              const SonareProjectMarker* marker, const char* name,
                                              uint32_t* out_marker_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_marker_id) *out_marker_id = 0;
  if (!project || !out_marker_id || !marker || !name || !finite_non_negative(marker->ppq) ||
      marker->kind > SONARE_MARKER_KIND_KEY_SIGNATURE)
    return SONARE_ERROR_INVALID_PARAMETER;
  if (marker->kind == SONARE_MARKER_KIND_KEY_SIGNATURE &&
      (marker->key_fifths < -7 || marker->key_fifths > 7))
    return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetMarker>(marker->id, marker->ppq, name, marker->kind,
                                                  marker->key_fifths, marker->key_minor != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (marker->id == 0 && project->history.project().markers().empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_marker_id = marker->id != 0 ? marker->id : project->history.project().markers().back().id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, marker, name, out_marker_id);
#endif
}

SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* scene_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  // mixing::api::Scene / scene_from_json / arr::SetScene are all control-plane
  // data types available regardless of BUILD_MIXING (arrangement::Project
  // unconditionally holds a Scene member; see project_serializer.h's "Mixer
  // topology" section) — this used to require SONARE_WITH_MIXING too, which
  // made a documented mixing-OFF-available API unreachable in exactly that
  // build.
  if (!project || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto scene = sonare::mixing::api::scene_from_json(scene_json);
  auto command = std::make_unique<arr::SetScene>(std::move(scene));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, scene_json);
#endif
}

SonareError sonare_project_set_track_kind(SonareProject* project, uint32_t track_id,
                                          uint32_t kind) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || kind > SONARE_TRACK_AUX) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackKind>(track_id, static_cast<arr::Track::Kind>(kind));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, kind);
#endif
}

SonareError sonare_project_set_warp_map(SonareProject* project,
                                        const SonareProjectWarpMapDesc* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !desc || desc->id == 0 || desc->anchor_count < 2 || !desc->anchors ||
      desc->anchor_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<arr::WarpAnchorRef> anchors;
  anchors.reserve(desc->anchor_count);
  for (size_t i = 0; i < desc->anchor_count; ++i) {
    const SonareProjectWarpAnchor& in = desc->anchors[i];
    if (!finite_non_negative(in.warp_sample) || !finite_non_negative(in.source_sample)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!anchors.empty()) {
      const auto& prev = anchors.back();
      if (!(in.warp_sample > prev.warp_sample && in.source_sample > prev.source_sample)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }
    anchors.push_back(arr::WarpAnchorRef{in.warp_sample, in.source_sample});
  }

  SONARE_C_TRY
  arr::WarpMapRef map;
  map.id = desc->id;
  map.name = desc->name ? desc->name : "";
  map.anchors = std::move(anchors);
  auto command = std::make_unique<arr::SetWarpMap>(std::move(map));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc);
#endif
}

SonareError sonare_project_remove_warp_map(SonareProject* project, uint32_t warp_ref_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || warp_ref_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_warp_map(warp_ref_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveWarpMap>(warp_ref_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, warp_ref_id);
#endif
}

SonareError sonare_project_set_track_midi_destination(SonareProject* project, uint32_t track_id,
                                                      uint32_t destination_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackMidiDestination>(track_id, destination_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, destination_id);
#endif
}

SonareError sonare_project_set_track_gain(SonareProject* project, uint32_t track_id, float gain) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || !std::isfinite(gain) || gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackGain>(track_id, gain);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, gain);
#endif
}

SonareError sonare_project_set_track_mute(SonareProject* project, uint32_t track_id, int mute) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackMute>(track_id, mute != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, mute);
#endif
}

SonareError sonare_project_set_track_solo(SonareProject* project, uint32_t track_id, int solo) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackSolo>(track_id, solo != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, solo);
#endif
}

SonareError sonare_project_set_track_pan(SonareProject* project, uint32_t track_id, float pan) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || !std::isfinite(pan)) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackPan>(track_id, pan);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, pan);
#endif
}

SonareError sonare_project_remove_track(SonareProject* project, uint32_t track_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Project& edit_project = project->history.project();
  if (!edit_project.has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const std::vector<arr::SourceId> orphaned_sources =
      sonare_project_collect_orphaned_sources_for_track(edit_project, track_id);
  std::vector<arr::SourceId> orphaned_audio_contents;
  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(1 + orphaned_sources.size() + 1);
  commands.push_back(std::make_unique<arr::RemoveTrack>(track_id));
  for (const arr::SourceId source_id : orphaned_sources) {
    commands.push_back(std::make_unique<arr::RemoveSourceInternal>(source_id));
    if (project->audio.sources.find(source_id) != project->audio.sources.end()) {
      orphaned_audio_contents.push_back(source_id);
    }
  }
  if (!orphaned_audio_contents.empty()) {
    commands.push_back(sonare_project_make_remove_audio_content_command(
        &project->audio, std::move(orphaned_audio_contents)));
  }
  if (!project->history.apply_transaction(std::move(commands))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id);
#endif
}

SonareError sonare_project_rename_track(SonareProject* project, uint32_t track_id,
                                        const char* name) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command =
      std::make_unique<arr::RenameTrack>(track_id, name ? std::string(name) : std::string());
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, name);
#endif
}

SonareError sonare_project_set_track_route(SonareProject* project, uint32_t track_id,
                                           const char* channel_strip_ref,
                                           const char* output_target) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackRoute>(
      track_id, channel_strip_ref ? std::string(channel_strip_ref) : std::string(),
      output_target ? std::string(output_target) : std::string());
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, channel_strip_ref, output_target);
#endif
}

SonareError sonare_project_add_automation_lane(SonareProject* project, uint32_t track_id,
                                               const SonareAutomationLaneDesc* desc,
                                               uint32_t* out_target_param_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_target_param_id) *out_target_param_id = 0;
  if (!project || track_id == 0 || !desc) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc(desc, &lane);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::AddAutomationLane>(track_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_target_param_id) *out_target_param_id = desc->target_param_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, desc, out_target_param_id);
#endif
}

SonareError sonare_project_add_automation_lane_ex(SonareProject* project, uint32_t track_id,
                                                  const SonareAutomationLaneDescEx* desc,
                                                  uint32_t* out_target_param_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_target_param_id) *out_target_param_id = 0;
  if (!project || track_id == 0 || !desc) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc_ex(desc, &lane);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::AddAutomationLane>(track_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_target_param_id) *out_target_param_id = desc->target_param_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, desc, out_target_param_id);
#endif
}

SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                uint32_t target_param_id,
                                                const SonareAutomationLaneDesc* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || target_param_id == 0 || !desc) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::Track* track = project->history.project().find_track(track_id);
  if (track == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const auto found = std::find_if(
      track->automation_lanes.begin(), track->automation_lanes.end(),
      [target_param_id](const auto& lane) { return lane.target_param_id() == target_param_id; });
  if (found == track->automation_lanes.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc(desc, &lane);
  if (err != SONARE_OK) return err;
  if (lane.target_param_id() != target_param_id) return SONARE_ERROR_INVALID_PARAMETER;
  // The legacy descriptor has no target kind. Preserve the lane's existing
  // classification so editing points cannot silently turn a typed lane into
  // an opaque lane.
  lane.set_target_kind(found->target_kind());
  auto command =
      std::make_unique<arr::EditAutomationLane>(track_id, target_param_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, target_param_id, desc);
#endif
}

SonareError sonare_project_edit_automation_lane_ex(SonareProject* project, uint32_t track_id,
                                                   uint32_t target_param_id,
                                                   const SonareAutomationLaneDescEx* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || target_param_id == 0 || !desc) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::Track* track = project->history.project().find_track(track_id);
  if (track == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const auto found = std::find_if(
      track->automation_lanes.begin(), track->automation_lanes.end(),
      [target_param_id](const auto& lane) { return lane.target_param_id() == target_param_id; });
  if (found == track->automation_lanes.end()) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc_ex(desc, &lane);
  if (err != SONARE_OK) return err;
  if (lane.target_param_id() != target_param_id) return SONARE_ERROR_INVALID_PARAMETER;
  auto command =
      std::make_unique<arr::EditAutomationLane>(track_id, target_param_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, target_param_id, desc);
#endif
}

SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  uint32_t target_param_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || target_param_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Track* track = project->history.project().find_track(track_id);
  if (track == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const auto found = std::find_if(
      track->automation_lanes.begin(), track->automation_lanes.end(),
      [target_param_id](const auto& lane) { return lane.target_param_id() == target_param_id; });
  if (found == track->automation_lanes.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveAutomationLane>(track_id, target_param_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, target_param_id);
#endif
}

SonareError sonare_project_undo(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return project->history.undo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

SonareError sonare_project_redo(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return project->history.redo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

SonareError sonare_project_clear_history(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  project->history.clear_history();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

SonareError sonare_project_set_max_undo_depth(SonareProject* project, size_t depth) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  project->history.set_max_undo_depth(depth);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, depth);
#endif
}

SonareError sonare_project_set_max_history_bytes(SonareProject* project, size_t bytes) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  project->history.set_max_history_bytes(bytes);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, bytes);
#endif
}
