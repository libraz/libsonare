#include <algorithm>

#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"
#include "mixing/panner.h"
#include "mixing/stereo_width.h"
#include "util/parameter_checks.h"
#if defined(SONARE_WITH_MIXING)
#include "engine/mixing_runtime.h"
#endif

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
static_assert(static_cast<int>(arr::MarkerKind::kMarker) == SONARE_MARKER_KIND_MARKER,
              "SonareMarkerKind marker ordinal drift");
static_assert(static_cast<int>(arr::MarkerKind::kText) == SONARE_MARKER_KIND_TEXT,
              "SonareMarkerKind text ordinal drift");
static_assert(static_cast<int>(arr::MarkerKind::kLyric) == SONARE_MARKER_KIND_LYRIC,
              "SonareMarkerKind lyric ordinal drift");
static_assert(static_cast<int>(arr::MarkerKind::kCuePoint) == SONARE_MARKER_KIND_CUE_POINT,
              "SonareMarkerKind cue point ordinal drift");
static_assert(static_cast<int>(arr::MarkerKind::kKeySignature) == SONARE_MARKER_KIND_KEY_SIGNATURE,
              "SonareMarkerKind key signature ordinal drift");

namespace {

// Refuses @p value unless it is inside the domain of the pan or width parameter the lane drives.
void check_lane_value(sonare::automation::AutomationTargetKind kind, uint32_t param_id, float value,
                      const char* subject) {
  namespace check = sonare::util::check;
  if (kind == sonare::automation::AutomationTargetKind::kTrackPan) {
    check::in_range(value, sonare::mixing::kPanMin, sonare::mixing::kPanMax, subject);
    return;
  }
#if defined(SONARE_WITH_MIXING)
  if (kind == sonare::automation::AutomationTargetKind::kOpaque) {
    if (param_id == sonare::engine::MixingRuntime::kPan) {
      check::in_range(value, sonare::mixing::kPanMin, sonare::mixing::kPanMax, subject);
      return;
    }
    if (param_id == sonare::engine::MixingRuntime::kWidth) {
      check::in_range(value, sonare::mixing::kWidthMin, sonare::mixing::kWidthMax, subject);
      return;
    }
  }
#else
  (void)param_id;
#endif
  check::finite_float(value, subject);
}

// Validates lane descriptor fields and builds an automation::AutomationLane, refusing by throwing.
// The target parameter id must be non-zero; each breakpoint's ppq must be finite and >= 0, its
// value finite (and inside the pan or width domain for such lanes), and its curve ordinal in range.
// A non-null @p current_kind is the edited lane's kind, which SONARE_AUTOMATION_TARGET_UNCHANGED
// keeps.
void automation_lane_from_values(uint32_t target_param_id, int32_t target_kind,
                                 const SonareAutomationPoint* point_data, size_t point_count,
                                 const sonare::automation::AutomationTargetKind* current_kind,
                                 sonare::automation::AutomationLane* out) {
  namespace check = sonare::util::check;
  check::integer_within(target_param_id, kAutomationTargetParamIdDomain, "desc.target_param_id");
  sonare::automation::AutomationTargetKind kind{};
  if (current_kind != nullptr && target_kind == SONARE_AUTOMATION_TARGET_UNCHANGED) {
    kind = *current_kind;
  } else if (!ordinal_to_enum(target_kind, 0, SONARE_AUTOMATION_TARGET_TRACK_PAN, &kind)) {
    check::refuse("desc.target_kind", sonare::util::refusal::kDeclaredValuePredicate);
  }
  check::non_null_when_count(point_data, point_count, "desc.points", "desc.point_count");
  check::integer_in_range(static_cast<long long>(point_count), 0,
                          static_cast<long long>(kMaxBufferSize), "desc.point_count");
  std::vector<sonare::automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    const SonareAutomationPoint& p = point_data[i];
    check::at_least(p.ppq, 0.0, check::indexed_subject("desc.points", i, "ppq").c_str());
    check_lane_value(kind, target_param_id, p.value,
                     check::indexed_subject("desc.points", i, "value").c_str());
    sonare::automation::Breakpoint bp;
    if (!ordinal_to_enum(p.curve_to_next, 0, static_cast<int>(sonare::AutomationCurve::SCurve),
                         &bp.curve_to_next)) {
      check::refuse(check::indexed_subject("desc.points", i, "curve_to_next").c_str(),
                    sonare::util::refusal::kDeclaredValuePredicate);
    }
    bp.ppq = p.ppq;
    bp.value = p.value;
    breakpoints.push_back(bp);
  }
  out->set_target_param_id(target_param_id);
  out->set_target_kind(kind);
  out->set_points(std::move(breakpoints));
}

void automation_lane_from_desc(const SonareAutomationLaneDesc* desc,
                               const sonare::automation::AutomationTargetKind* current_kind,
                               sonare::automation::AutomationLane* out) {
  automation_lane_from_values(desc->target_param_id, static_cast<int32_t>(desc->target_kind),
                              desc->points, desc->point_count, current_kind, out);
}

// Refuses a track_id that names no track of the project.
SonareError require_track(const SonareProject* project, uint32_t track_id) {
  if (track_id != 0 && project->history.project().has_track(track_id)) return SONARE_OK;
  return run_validation(
      [] { sonare::util::check::refuse_unknown_id("track_id", "a track", "project"); });
}

// Refuses a target_param_id that names no automation lane of @p track.
void require_lane(const arr::Track& track, uint32_t target_param_id) {
  if (target_param_id != 0 &&
      std::any_of(track.automation_lanes.begin(), track.automation_lanes.end(),
                  [target_param_id](const auto& lane) {
                    return lane.target_param_id() == target_param_id;
                  })) {
    return;
  }
  sonare::util::check::refuse_unknown_id("target_param_id", "an automation lane", "track");
}

// Refuses an entity id the arrangement reserves as its exhausted-counter sentinel.
void check_assignable_entity_id(uint32_t id, const char* subject) {
  sonare::util::check::integer_within(id, kAssignableEntityIdDomain, subject);
}

// Refuses a marker whose kind, position, id or key signature the project cannot store.
void check_marker(const SonareProjectMarker* marker) {
  namespace check = sonare::util::check;
  check::at_least(marker->ppq, 0.0, "marker.ppq");
  if (marker->kind > SONARE_MARKER_KIND_KEY_SIGNATURE) {
    check::refuse("marker.kind", sonare::util::refusal::kDeclaredValuePredicate);
  }
  check_assignable_entity_id(marker->id, "marker.id");
  // Key signatures encode the SMF `sf` byte, which is constrained to -7..7
  // (7 flats to 7 sharps), so an out-of-range value would not serialize to a conformant SMF.
  if (marker->kind == SONARE_MARKER_KIND_KEY_SIGNATURE) {
    check::integer_within(marker->key_fifths, kMarkerKeyFifthsDomain, "marker.key_fifths");
  }
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
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  SONARE_C_REQUIRE_NONNULL(out_track_id);
  arr::Track track;
  if (!ordinal_to_enum(desc->kind, SONARE_TRACK_AUDIO, SONARE_TRACK_AUX, &track.kind)) {
    return run_validation([] {
      sonare::util::check::refuse("desc.kind", sonare::util::refusal::kDeclaredValuePredicate);
    });
  }
  SONARE_C_TRY
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
  if (out_track_id) *out_track_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 desc, out_track_id);
#endif
}

SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  arr::OverlapPolicy policy{};
  if (!ordinal_to_enum(overlap_policy, 0, SONARE_PROJECT_OVERLAP_ALLOW, &policy)) {
    return run_validation([] {
      sonare::util::check::refuse("overlap_policy", sonare::util::refusal::kDeclaredValuePredicate);
    });
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetOverlapPolicy>(policy);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 overlap_policy);
#endif
}

SonareError sonare_project_set_tempo_segments(SonareProject* project,
                                              const SonareProjectTempoSegment* segments,
                                              size_t segment_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  namespace check = sonare::util::check;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(segments, segment_count);
  check::integer_in_range(static_cast<long long>(segment_count), 0,
                          static_cast<long long>(kMaxBufferSize), "segment_count");
  std::vector<sonare::transport::TempoSegment> out;
  // The staging vector is sized from a caller-supplied count, so its allocation
  // has to sit inside the try or a bad_alloc crosses the extern "C" boundary.
  out.reserve(segment_count);
  double previous_start_ppq = -1.0;
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTempoSegment& in = segments[i];
    // Same admissible set as the engine's setter (sonare_engine_set_tempo_segments):
    // both positions and tempi are bounded, not merely finite. The project side
    // used to take any finite positive BPM, and the two consumers that build a
    // raw TempoMap from the stored segments (sonare_project_set_loop_from_audio,
    // sonare_project_snap_to_grid) then computed silently clamped, meaningless
    // sample positions from it.
    const std::string prefix = "segments[" + std::to_string(i) + "].";
    check::in_range(in.start_ppq, 0.0, sonare::transport::kMaxPublicPpq,
                    (prefix + "start_ppq").c_str());
    check::in_interval(in.bpm, 0.0, false, sonare::transport::kMaxPublicTempoBpm, true,
                       (prefix + "bpm").c_str());
    check::in_range(in.end_bpm, 0.0, sonare::transport::kMaxPublicTempoBpm,
                    (prefix + "end_bpm").c_str());
    if (in.start_ppq <= previous_start_ppq) {
      check::refuse_relation(
          {prefix + "start_ppq", "segments[" + std::to_string(i - 1) + "].start_ppq"},
          "{0} must be greater than {1}");
    }
    sonare::transport::TempoSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.bpm = in.bpm;
    seg.end_bpm = in.end_bpm;
    out.push_back(seg);
    previous_start_ppq = in.start_ppq;
  }
  auto command = std::make_unique<arr::SetTempoSegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 segments, segment_count);
#endif
}

SonareError sonare_project_set_time_signatures(SonareProject* project,
                                               const SonareProjectTimeSignatureSegment* segments,
                                               size_t segment_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  namespace check = sonare::util::check;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(segments, segment_count);
  check::integer_in_range(static_cast<long long>(segment_count), 0,
                          static_cast<long long>(kMaxBufferSize), "segment_count");
  std::vector<sonare::transport::TimeSignatureSegment> out;
  // Same reason as sonare_project_set_tempo_segments: a caller-sized allocation
  // must not throw outside the try.
  out.reserve(segment_count);
  double previous_start_ppq = -1.0;
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTimeSignatureSegment& in = segments[i];
    // The two setters are documented as a pair, so they take the same shape of
    // input: bounded positions, and strictly increasing starts. Segments are
    // stored verbatim and read back "in stored order", so an out-of-order list
    // survives a round trip and only the compiler's own sort hides it at
    // playback time.
    const std::string prefix = "segments[" + std::to_string(i) + "].";
    check::in_range(in.start_ppq, 0.0, sonare::transport::kMaxPublicPpq,
                    (prefix + "start_ppq").c_str());
    check::integer_within(in.numerator, kTimeSignatureNumeratorDomain,
                          (prefix + "numerator").c_str());
    check::integer_within(in.denominator, kTimeSignatureDenominatorDomain,
                          (prefix + "denominator").c_str());
    if (in.start_ppq <= previous_start_ppq) {
      check::refuse_relation(
          {prefix + "start_ppq", "segments[" + std::to_string(i - 1) + "].start_ppq"},
          "{0} must be greater than {1}");
    }
    sonare::transport::TimeSignatureSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.time_sig.numerator = in.numerator;
    seg.time_sig.denominator = in.denominator;
    out.push_back(seg);
    previous_start_ppq = in.start_ppq;
  }
  auto command = std::make_unique<arr::SetTimeSignatureSegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 segments, segment_count);
#endif
}

SonareError sonare_project_set_marker(SonareProject* project, const SonareProjectMarker* marker,
                                      const char* name, uint32_t* out_marker_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_marker_id) *out_marker_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(marker);
  SONARE_C_REQUIRE_NONNULL(out_marker_id);
  if (const SonareError err = run_validation([&] { check_marker(marker); }); err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  auto command =
      std::make_unique<arr::SetMarker>(marker->id, marker->ppq, name != nullptr ? name : "",
                                       marker->kind, marker->key_fifths, marker->key_minor != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (marker->id == 0 && project->history.project().markers().empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_marker_id = marker->id != 0 ? marker->id : project->history.project().markers().back().id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_marker_id) *out_marker_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 marker, name, out_marker_id);
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
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(scene_json);
  SONARE_C_TRY
  sonare::mixing::api::Scene scene;
  sonare_c_detail::clear_last_diagnostics();
  try {
    scene = sonare::mixing::api::scene_from_json(scene_json);
  } catch (const sonare::util::json::JsonError& ex) {
    // Malformed JSON is INVALID_FORMAT at every other C-ABI JSON entry point;
    // without this arm it reached SONARE_C_CATCH's std::exception tail as
    // SONARE_ERROR_UNKNOWN.
    set_last_error(SONARE_ERROR_INVALID_FORMAT, ex.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto command = std::make_unique<arr::SetScene>(std::move(scene));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 scene_json);
#endif
}

SonareError sonare_project_set_track_kind(SonareProject* project, uint32_t track_id,
                                          uint32_t kind) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  arr::Track::Kind track_kind{};
  if (!ordinal_to_enum(kind, 0, SONARE_TRACK_AUX, &track_kind)) {
    return run_validation([] {
      sonare::util::check::refuse("kind", sonare::util::refusal::kDeclaredValuePredicate);
    });
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackKind>(track_id, track_kind);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, kind);
#endif
}

SonareError sonare_project_set_warp_map(SonareProject* project,
                                        const SonareProjectWarpMapDesc* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  namespace check = sonare::util::check;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  check::integer_within(desc->id, kWarpMapIdDomain, "desc.id");
  check::integer_in_range(static_cast<long long>(desc->anchor_count), 2,
                          static_cast<long long>(kMaxBufferSize), "desc.anchor_count");
  check::non_null(desc->anchors, "desc.anchors");
  std::vector<arr::WarpAnchorRef> anchors;
  // Same reason as the two segment setters: anchor_count is caller-supplied.
  anchors.reserve(desc->anchor_count);
  for (size_t i = 0; i < desc->anchor_count; ++i) {
    const SonareProjectWarpAnchor& in = desc->anchors[i];
    check::at_least(in.warp_sample, 0.0,
                    check::indexed_subject("desc.anchors", i, "warp_sample").c_str());
    check::at_least(in.source_sample, 0.0,
                    check::indexed_subject("desc.anchors", i, "source_sample").c_str());
    if (!anchors.empty()) {
      const auto& prev = anchors.back();
      if (!(in.warp_sample > prev.warp_sample)) {
        check::refuse_relation({check::indexed_subject("desc.anchors", i, "warp_sample"),
                                check::indexed_subject("desc.anchors", i - 1, "warp_sample")},
                               "{0} must be greater than {1}");
      }
      if (!(in.source_sample > prev.source_sample)) {
        check::refuse_relation({check::indexed_subject("desc.anchors", i, "source_sample"),
                                check::indexed_subject("desc.anchors", i - 1, "source_sample")},
                               "{0} must be greater than {1}");
      }
    }
    anchors.push_back(arr::WarpAnchorRef{in.warp_sample, in.source_sample});
  }

  arr::WarpMapRef map;
  map.id = desc->id;
  map.name = desc->name ? desc->name : "";
  map.anchors = std::move(anchors);
  auto command = std::make_unique<arr::SetWarpMap>(std::move(map));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 desc);
#endif
}

SonareError sonare_project_remove_warp_map(SonareProject* project, uint32_t warp_ref_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (warp_ref_id == 0 || !project->history.project().has_warp_map(warp_ref_id)) {
    return run_validation(
        [] { sonare::util::check::refuse_unknown_id("warp_ref_id", "a warp map", "project"); });
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveWarpMap>(warp_ref_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 warp_ref_id);
#endif
}

SonareError sonare_project_set_track_midi_destination(SonareProject* project, uint32_t track_id,
                                                      uint32_t destination_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackMidiDestination>(track_id, destination_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, destination_id);
#endif
}

SonareError sonare_project_set_track_gain(SonareProject* project, uint32_t track_id, float gain) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  if (const SonareError err =
          run_validation([&] { sonare::util::check::at_least(gain, 0.0, "gain"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackGain>(track_id, gain);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, gain);
#endif
}

SonareError sonare_project_set_track_mute(SonareProject* project, uint32_t track_id, int mute) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackMute>(track_id, mute != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, mute);
#endif
}

SonareError sonare_project_set_track_solo(SonareProject* project, uint32_t track_id, int solo) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackSolo>(track_id, solo != 0);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, solo);
#endif
}

SonareError sonare_project_set_track_pan(SonareProject* project, uint32_t track_id, float pan) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  if (const SonareError err = run_validation([&] {
        sonare::util::check::in_range(pan, sonare::mixing::kPanMin, sonare::mixing::kPanMax, "pan");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackPan>(track_id, pan);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, pan);
#endif
}

SonareError sonare_project_remove_track(SonareProject* project, uint32_t track_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  const arr::Project& edit_project = project->history.project();
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id);
#endif
}

SonareError sonare_project_rename_track(SonareProject* project, uint32_t track_id,
                                        const char* name) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command =
      std::make_unique<arr::RenameTrack>(track_id, name ? std::string(name) : std::string());
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, name);
#endif
}

SonareError sonare_project_set_track_route(SonareProject* project, uint32_t track_id,
                                           const char* channel_strip_ref,
                                           const char* output_target) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetTrackRoute>(
      track_id, channel_strip_ref ? std::string(channel_strip_ref) : std::string(),
      output_target ? std::string(output_target) : std::string());
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, channel_strip_ref, output_target);
#endif
}

SonareError sonare_project_add_automation_lane(SonareProject* project, uint32_t track_id,
                                               const SonareAutomationLaneDesc* desc,
                                               uint32_t* out_target_param_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_target_param_id) *out_target_param_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  automation_lane_from_desc(desc, nullptr, &lane);
  auto command = std::make_unique<arr::AddAutomationLane>(track_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_target_param_id) *out_target_param_id = desc->target_param_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_target_param_id) *out_target_param_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, desc, out_target_param_id);
#endif
}

SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                uint32_t target_param_id,
                                                const SonareAutomationLaneDesc* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  const arr::Track* track = project->history.project().find_track(track_id);
  require_lane(*track, target_param_id);
  const auto current = std::find_if(
      track->automation_lanes.begin(), track->automation_lanes.end(),
      [target_param_id](const auto& l) { return l.target_param_id() == target_param_id; });
  const sonare::automation::AutomationTargetKind current_kind = current->target_kind();
  sonare::automation::AutomationLane lane;
  automation_lane_from_desc(desc, &current_kind, &lane);
  if (lane.target_param_id() != target_param_id) {
    sonare::util::check::refuse_relation({"target_param_id", "desc.target_param_id"},
                                         "{0} must equal {1}");
  }
  auto command =
      std::make_unique<arr::EditAutomationLane>(track_id, target_param_id, std::move(lane));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, target_param_id, desc);
#endif
}

SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  uint32_t target_param_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  require_lane(*project->history.project().find_track(track_id), target_param_id);
  auto command = std::make_unique<arr::RemoveAutomationLane>(track_id, target_param_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, target_param_id);
#endif
}

SonareError sonare_project_undo(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  return project->history.undo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project);
#endif
}

SonareError sonare_project_redo(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  return project->history.redo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project);
#endif
}

SonareError sonare_project_clear_history(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  project->history.clear_history();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project);
#endif
}

SonareError sonare_project_set_max_undo_depth(SonareProject* project, size_t depth) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  project->history.set_max_undo_depth(depth);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 depth);
#endif
}

SonareError sonare_project_set_max_history_bytes(SonareProject* project, size_t bytes) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  project->history.set_max_history_bytes(bytes);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 bytes);
#endif
}
