#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

// Pin the C fade-curve / loop-mode / automation-curve ordinals to their C++
// enums so reordering a C++ enum is caught at compile time (these flat-POD
// ordinals are part of the project ABI).
static_assert(static_cast<int>(arr::FadeCurve::kLinear) == SONARE_FADE_CURVE_LINEAR,
              "SonareProjectFadeCurve linear ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kEqualPower) == SONARE_FADE_CURVE_EQUAL_POWER,
              "SonareProjectFadeCurve equal-power ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kExponential) == SONARE_FADE_CURVE_EXPONENTIAL,
              "SonareProjectFadeCurve exponential ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kLogarithmic) == SONARE_FADE_CURVE_LOGARITHMIC,
              "SonareProjectFadeCurve logarithmic ordinal drift");
static_assert(static_cast<int>(arr::LoopMode::kOff) == SONARE_LOOP_MODE_OFF,
              "SonareProjectLoopMode off ordinal drift");
static_assert(static_cast<int>(arr::LoopMode::kLoop) == SONARE_LOOP_MODE_LOOP,
              "SonareProjectLoopMode loop ordinal drift");
static_assert(static_cast<uint32_t>(arr::OverlapPolicy::kDisallow) ==
                  SONARE_PROJECT_OVERLAP_DISALLOW,
              "SonareProjectOverlapPolicy disallow ordinal drift");
static_assert(static_cast<uint32_t>(arr::OverlapPolicy::kAllow) == SONARE_PROJECT_OVERLAP_ALLOW,
              "SonareProjectOverlapPolicy allow ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::Linear) == SONARE_CURVE_LINEAR,
              "SonareProjectAutomationCurve linear ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::Exponential) == SONARE_CURVE_EXPONENTIAL,
              "SonareProjectAutomationCurve exponential ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::Hold) == SONARE_CURVE_HOLD,
              "SonareProjectAutomationCurve hold ordinal drift");
static_assert(static_cast<int>(sonare::AutomationCurve::SCurve) == SONARE_CURVE_SCURVE,
              "SonareProjectAutomationCurve scurve ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kAudio) == SONARE_TRACK_AUDIO,
              "SonareProjectTrackKind audio ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kMidi) == SONARE_TRACK_MIDI,
              "SonareProjectTrackKind midi ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kAux) == SONARE_TRACK_AUX,
              "SonareProjectTrackKind aux ordinal drift");

namespace {

// Validates a fade desc and copies it into an arrangement ClipFade. Returns
// SONARE_OK on success; the fade length must be finite and >= 0 and the curve
// ordinal in range.
SonareError clip_fade_from_desc(const SonareProjectClipFade* desc, arr::ClipFade* out) {
  if (desc == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!finite_non_negative(desc->length_ppq)) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->curve > static_cast<uint32_t>(arr::FadeCurve::kLogarithmic)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->length_ppq = desc->length_ppq;
  out->curve = static_cast<arr::FadeCurve>(desc->curve);
  return SONARE_OK;
}

// Validates a lane desc and builds an automation::AutomationLane from it. Each
// breakpoint's ppq must be finite and >= 0, its value finite, and its curve
// ordinal in range.
SonareError automation_lane_from_desc(const SonareAutomationLaneDesc* desc,
                                      sonare::automation::AutomationLane* out) {
  if (desc == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->point_count > 0 && desc->points == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->point_count > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<sonare::automation::Breakpoint> points;
  points.reserve(desc->point_count);
  for (size_t i = 0; i < desc->point_count; ++i) {
    const SonareAutomationPoint& p = desc->points[i];
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
    points.push_back(bp);
  }
  out->set_target_param_id(desc->target_param_id);
  out->set_points(std::move(points));
  return SONARE_OK;
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// Edit
// ============================================================================

SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id) {
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
  arr::AddTrack* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  *out_track_id = raw->allocated_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_track_id);
#endif
}

SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !desc || !out_clip_id) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->track_id == 0 || !finite_positive(desc->length_ppq) ||
      !finite_non_negative(desc->start_ppq) || !finite_non_negative(desc->source_offset_ppq) ||
      !std::isfinite(desc->gain) || desc->gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_track(desc->track_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const SonareError audio_err =
      desc->is_midi == 0 ? validate_audio_clip_payload(desc, nullptr) : SONARE_OK;
  if (audio_err != SONARE_OK) return audio_err;
  SONARE_C_TRY
  const size_t rollback_depth = project->history.undo_depth();
  // Register the clip source (audio or MIDI) through a command, then add the
  // clip. Both mutations go through EditHistory so they participate in undo.
  arr::SourceId source_id = 0;
  if (desc->is_midi != 0) {
    arr::MidiSourceRef ref;
    auto attach = std::make_unique<arr::AttachMidiSource>(ref);
    arr::AttachMidiSource* raw = attach.get();
    if (!project->history.apply(std::move(attach))) return SONARE_ERROR_INVALID_STATE;
    source_id = raw->allocated_id();
  } else {
    arr::AudioSourceRef ref;
    if (desc->source_uri) ref.uri = desc->source_uri;
    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      ref.channel_count = static_cast<uint32_t>(desc->audio_channels);
      ref.sample_rate_hint = static_cast<double>(desc->audio_sample_rate);
    }
    auto attach = std::make_unique<arr::AttachAudioSource>(ref);
    arr::AttachAudioSource* raw = attach.get();
    if (!project->history.apply(std::move(attach))) return SONARE_ERROR_INVALID_STATE;
    source_id = raw->allocated_id();

    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      arr::AudioSourceSamples samples;
      samples.sample_rate = static_cast<double>(desc->audio_sample_rate);
      samples.channels =
          deinterleave(desc->audio_interleaved, desc->audio_frames, desc->audio_channels);
      project->audio.sources[source_id] = std::move(samples);
    }
  }

  arr::EditClip clip;
  clip.track_id = desc->track_id;
  clip.source_id = source_id;
  clip.start_ppq = desc->start_ppq;
  clip.length_ppq = desc->length_ppq;
  clip.source_offset_ppq = desc->source_offset_ppq;
  // Pass the gain through literally (validated finite and >= 0 above): a gain of
  // 0 is a legitimately silent clip, not a request for unity. No coercion here.
  clip.gain = desc->gain;
  auto command = std::make_unique<arr::AddClip>(clip);
  arr::AddClip* raw = command.get();
  if (!project->history.apply(std::move(command)) || raw->allocated_id() == 0) {
    project->audio.sources.erase(source_id);
    rollback_to_depth(project, rollback_depth);
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = raw->allocated_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_clip_id);
#endif
}

SonareError sonare_project_add_midi_clip(SonareProject* project, double start_ppq,
                                         double length_ppq, uint32_t* out_track_id,
                                         uint32_t* out_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_track_id) *out_track_id = 0;
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !out_track_id || !out_clip_id || !finite_non_negative(start_ppq) ||
      !finite_positive(length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t rollback_depth = project->history.undo_depth();

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_MIDI;
  track_desc.name = "midi";
  SonareError err = sonare_project_add_track(project, &track_desc, out_track_id);
  if (err != SONARE_OK) {
    rollback_to_depth(project, rollback_depth);
    return err;
  }

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = *out_track_id;
  clip_desc.is_midi = 1;
  clip_desc.start_ppq = start_ppq;
  clip_desc.length_ppq = length_ppq;
  err = sonare_project_add_clip(project, &clip_desc, out_clip_id);
  if (err != SONARE_OK) {
    rollback_to_depth(project, rollback_depth);
  }
  return err;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, start_ppq, length_ppq, out_track_id, out_clip_id);
#endif
}

SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy) {
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
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || (segment_count > 0 && !segments) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<sonare::transport::TempoSegment> out;
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTempoSegment& in = segments[i];
    if (!finite_non_negative(in.start_ppq) || !finite_non_negative(in.start_sample) ||
        !finite_positive(in.bpm) || !std::isfinite(in.end_bpm) || in.end_bpm < 0.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::transport::TempoSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.bpm = in.bpm;
    seg.start_sample = in.start_sample;
    seg.end_bpm = in.end_bpm;
    out.push_back(seg);
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
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_marker_id) *out_marker_id = 0;
  if (!project || !out_marker_id || !finite_non_negative(ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetMarker>(marker_id, ppq, name ? name : "");
  arr::SetMarker* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  *out_marker_id = raw->allocated_id() != 0 ? raw->allocated_id() : marker_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, marker_id, ppq, name, out_marker_id);
#endif
}

SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* scene_json) {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
  if (!project || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto scene = sonare::mixing::api::scene_from_json(scene_json);
  auto command = std::make_unique<arr::SetScene>(std::move(scene));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#elif defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_STUB_NOT_SUPPORTED(project, scene_json);
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, scene_json);
#endif
}

SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project || clip_id == 0 || !std::isfinite(split_ppq)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SplitClip>(clip_id, split_ppq);
  arr::SplitClip* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_new_clip_id) *out_new_clip_id = raw->new_clip_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, split_ppq, out_new_clip_id);
#endif
}

SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq) ||
      !finite_positive(new_length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::TrimClip>(clip_id, new_start_ppq, new_length_ppq);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_length_ppq);
#endif
}

SonareError sonare_project_move_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     uint32_t new_track_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::MoveClip>(clip_id, new_start_ppq, new_track_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_track_id);
#endif
}

SonareError sonare_project_set_track_kind(SonareProject* project, uint32_t track_id,
                                          uint32_t kind) {
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

SonareError sonare_project_set_clip_warp_ref(SonareProject* project, uint32_t clip_id,
                                             uint32_t warp_ref_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipWarpRef>(clip_id, warp_ref_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, warp_ref_id);
#endif
}

SonareError sonare_project_set_warp_map(SonareProject* project,
                                        const SonareProjectWarpMapDesc* desc) {
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

SonareError sonare_project_remove_clip(SonareProject* project, uint32_t clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveClip>(clip_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id);
#endif
}

SonareError sonare_project_set_clip_gain(SonareProject* project, uint32_t clip_id, float gain) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !std::isfinite(gain) || gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipGain>(clip_id, gain);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, gain);
#endif
}

SonareError sonare_project_set_clip_fade(SonareProject* project, uint32_t clip_id,
                                         const SonareProjectClipFade* fade_in,
                                         const SonareProjectClipFade* fade_out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !fade_in || !fade_out) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  arr::ClipFade in_fade;
  arr::ClipFade out_fade;
  SonareError err = clip_fade_from_desc(fade_in, &in_fade);
  if (err != SONARE_OK) return err;
  err = clip_fade_from_desc(fade_out, &out_fade);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipFade>(clip_id, in_fade, out_fade);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, fade_in, fade_out);
#endif
}

SonareError sonare_project_set_clip_loop(SonareProject* project, uint32_t clip_id, int loop_mode,
                                         double loop_length_ppq) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || loop_mode < SONARE_LOOP_MODE_OFF ||
      loop_mode > SONARE_LOOP_MODE_LOOP) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const bool looping = loop_mode == SONARE_LOOP_MODE_LOOP;
  if (looping ? !finite_positive(loop_length_ppq) : !finite_non_negative(loop_length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipLoop>(clip_id, static_cast<arr::LoopMode>(loop_mode),
                                                    loop_length_ppq);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, loop_mode, loop_length_ppq);
#endif
}

SonareError sonare_project_set_clip_source(SonareProject* project, uint32_t clip_id,
                                           uint32_t source_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || source_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr ||
      !project->history.project().has_source(source_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipSource>(clip_id, source_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, source_id);
#endif
}

SonareError sonare_project_duplicate_clip(SonareProject* project, uint32_t clip_id,
                                          double new_start_ppq, uint32_t* out_new_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::DuplicateClip>(clip_id, new_start_ppq);
  arr::DuplicateClip* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_new_clip_id) *out_new_clip_id = raw->new_clip_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, out_new_clip_id);
#endif
}

SonareError sonare_project_remove_track(SonareProject* project, uint32_t track_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveTrack>(track_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id);
#endif
}

SonareError sonare_project_rename_track(SonareProject* project, uint32_t track_id,
                                        const char* name) {
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
                                               size_t* out_lane_index) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_lane_index) *out_lane_index = 0;
  if (!project || track_id == 0 || !desc) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project->history.project().has_track(track_id)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc(desc, &lane);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::AddAutomationLane>(track_id, std::move(lane));
  arr::AddAutomationLane* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_lane_index) *out_lane_index = raw->lane_index();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, desc, out_lane_index);
#endif
}

SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                size_t lane_index,
                                                const SonareAutomationLaneDesc* desc) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0 || !desc) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Track* track = project->history.project().find_track(track_id);
  if (track == nullptr || lane_index >= track->automation_lanes.size()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  const SonareError err = automation_lane_from_desc(desc, &lane);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::EditAutomationLane>(track_id, lane_index, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, lane_index, desc);
#endif
}

SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  size_t lane_index) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Track* track = project->history.project().find_track(track_id);
  if (track == nullptr || lane_index >= track->automation_lanes.size()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveAutomationLane>(track_id, lane_index);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, track_id, lane_index);
#endif
}

SonareError sonare_project_undo(SonareProject* project) {
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
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return project->history.redo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}
