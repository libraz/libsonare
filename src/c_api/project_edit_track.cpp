#include <algorithm>
#include <limits>

#include "c_api/ppq_domains.h"
#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"
#include "engine/track_mixer.h"
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
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kTrackSendDb) ==
                  SONARE_AUTOMATION_TARGET_TRACK_SEND_DB,
              "SonareAutomationTargetKind send ordinal drift");
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kTrackWidth) ==
                  SONARE_AUTOMATION_TARGET_TRACK_WIDTH,
              "SonareAutomationTargetKind width ordinal drift");
static_assert(static_cast<int>(sonare::automation::AutomationTargetKind::kTrackInsertParam) ==
                  SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM,
              "SonareAutomationTargetKind insert ordinal drift");
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
  if (kind == sonare::automation::AutomationTargetKind::kTrackWidth) {
    check::in_range(value, sonare::mixing::kWidthMin, sonare::mixing::kWidthMax, subject);
    return;
  }
  if (kind == sonare::automation::AutomationTargetKind::kTrackSendDb) {
    check::in_range(value, sonare::constants::kFloorDb,
                    sonare::engine::TrackMixerRuntime::kMaxGainDb, subject);
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
// A non-null @p current preserves the target identity when the descriptor uses UNCHANGED.
void automation_lane_from_desc(const SonareAutomationLaneDesc* desc,
                               const sonare::automation::AutomationLane* current,
                               sonare::automation::AutomationLane* out) {
  namespace check = sonare::util::check;
  sonare_c_detail::require_struct_size(desc->struct_size, sizeof(SonareAutomationLaneDesc), "desc",
                                       "sonare_automation_lane_desc_init");
  if (desc->reserved != 0) check::refuse("desc.reserved", "must be 0");

  sonare::automation::AutomationTargetKind kind{};
  uint32_t target_param_id = desc->target_param_id;
  sonare::automation::AutomationTargetIdentity identity;
  const bool unchanged =
      current != nullptr && desc->target_kind == SONARE_AUTOMATION_TARGET_UNCHANGED;
  if (unchanged) {
    kind = current->target_kind();
    target_param_id = current->target_param_id();
    identity = current->target_identity();
  } else if (!ordinal_to_enum(static_cast<int32_t>(desc->target_kind), 0,
                              SONARE_AUTOMATION_TARGET_TRACK_INSERT_PARAM, &kind)) {
    check::refuse("desc.target_kind", sonare::util::refusal::kDeclaredValuePredicate);
  }

  if (!unchanged) {
    switch (kind) {
      case sonare::automation::AutomationTargetKind::kOpaque:
        check::integer_within(target_param_id, kAutomationTargetParamIdDomain,
                              "desc.target_param_id");
        if (desc->send_id != nullptr) {
          check::refuse("desc.send_id", "must be NULL for an opaque target");
        }
        if (desc->insert_index != SONARE_AUTOMATION_INSERT_INDEX_NONE) {
          check::refuse("desc.insert_index", "must be NONE for an opaque target");
        }
        if (desc->processor != nullptr) {
          check::refuse("desc.processor", "must be NULL for an opaque target");
        }
        if (desc->param_name != nullptr) {
          check::refuse("desc.param_name", "must be NULL for an opaque target");
        }
        break;
      case sonare::automation::AutomationTargetKind::kTrackFaderDb:
      case sonare::automation::AutomationTargetKind::kTrackPan:
      case sonare::automation::AutomationTargetKind::kTrackWidth:
        if (target_param_id != 0) {
          check::refuse("desc.target_param_id", "must be 0 for a typed target");
        }
        if (desc->send_id != nullptr) {
          check::refuse("desc.send_id", "must be NULL for this target kind");
        }
        if (desc->insert_index != SONARE_AUTOMATION_INSERT_INDEX_NONE) {
          check::refuse("desc.insert_index", "must be NONE for this target kind");
        }
        if (desc->processor != nullptr) {
          check::refuse("desc.processor", "must be NULL for this target kind");
        }
        if (desc->param_name != nullptr) {
          check::refuse("desc.param_name", "must be NULL for this target kind");
        }
        break;
      case sonare::automation::AutomationTargetKind::kTrackSendDb:
        if (target_param_id != 0) {
          check::refuse("desc.target_param_id", "must be 0 for a typed target");
        }
        check::non_null(desc->send_id, "desc.send_id");
        check::non_empty(desc->send_id, "desc.send_id");
        if (desc->insert_index != SONARE_AUTOMATION_INSERT_INDEX_NONE) {
          check::refuse("desc.insert_index", "must be NONE for a send target");
        }
        if (desc->processor != nullptr) {
          check::refuse("desc.processor", "must be NULL for a send target");
        }
        if (desc->param_name != nullptr) {
          check::refuse("desc.param_name", "must be NULL for a send target");
        }
        identity.send_id = desc->send_id;
        break;
      case sonare::automation::AutomationTargetKind::kTrackInsertParam:
        if (target_param_id != 0) {
          check::refuse("desc.target_param_id", "must be 0 for a typed target");
        }
        if (desc->send_id != nullptr) {
          check::refuse("desc.send_id", "must be NULL for an insert target");
        }
        check::integer_in_range(desc->insert_index, 0, std::numeric_limits<int32_t>::max(),
                                "desc.insert_index");
        check::non_null(desc->processor, "desc.processor");
        check::non_empty(desc->processor, "desc.processor");
        check::non_null(desc->param_name, "desc.param_name");
        check::non_empty(desc->param_name, "desc.param_name");
        identity.insert_index = static_cast<size_t>(desc->insert_index);
        identity.processor = desc->processor;
        identity.param_name = desc->param_name;
        break;
    }
  }

  check::non_null_when_count(desc->points, desc->point_count, "desc.points", "desc.point_count");
  check::integer_in_range(static_cast<long long>(desc->point_count), 0,
                          static_cast<long long>(kMaxBufferSize), "desc.point_count");
  std::vector<sonare::automation::Breakpoint> breakpoints;
  breakpoints.reserve(desc->point_count);
  for (size_t i = 0; i < desc->point_count; ++i) {
    const SonareAutomationPoint& p = desc->points[i];
    check::within(p.ppq, sonare_c_detail::kPublicPpqDomain,
                  check::indexed_subject("desc.points", i, "ppq").c_str());
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
  sonare::automation::AutomationLane lane(target_param_id, kind, std::move(identity));
  if (current != nullptr) lane.set_lane_id(current->lane_id());
  lane.set_points(std::move(breakpoints));
  *out = std::move(lane);
}

// Refuses a track_id that names no track of the project.
SonareError require_track(const SonareProject* project, uint32_t track_id) {
  if (track_id != 0 && project->history.project().has_track(track_id)) return SONARE_OK;
  return run_validation(
      [] { sonare::util::check::refuse_unknown_id("track_id", "a track", "project"); });
}

// Refuses a lane_id that names no automation lane of @p track.
const sonare::automation::AutomationLane& require_lane(const arr::Track& track, uint32_t lane_id) {
  const auto found =
      std::find_if(track.automation_lanes.begin(), track.automation_lanes.end(),
                   [lane_id](const auto& lane) { return lane.lane_id() == lane_id; });
  if (lane_id != 0 && found != track.automation_lanes.end()) return *found;
  sonare::util::check::refuse_unknown_id("lane_id", "an automation lane", "track");
}

// Refuses an entity id the arrangement reserves as its exhausted-counter sentinel.
void check_assignable_entity_id(uint32_t id, const char* subject) {
  sonare::util::check::integer_within(id, kAssignableEntityIdDomain, subject);
}

// Refuses a marker whose kind, position, id or key signature the project cannot store.
void check_marker(const SonareProjectMarker* marker) {
  namespace check = sonare::util::check;
  check::within(marker->ppq, sonare_c_detail::kPublicPpqDomain, "marker.ppq");
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
    check::within(in.start_ppq, sonare_c_detail::kPublicPpqDomain, (prefix + "start_ppq").c_str());
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
    check::within(in.start_ppq, sonare_c_detail::kPublicPpqDomain, (prefix + "start_ppq").c_str());
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

SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* document) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  // mixing::api::Scene / scene_from_json / arr::SetScene are all control-plane
  // data types available regardless of BUILD_MIXING (arrangement::Project
  // unconditionally holds a Scene member; see project_serializer.h's "Mixer
  // topology" section) — this used to require SONARE_WITH_MIXING too, which
  // made a documented mixing-OFF-available API unreachable in exactly that
  // build.
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  sonare_c_detail::clear_last_diagnostics();
  if (document == nullptr) {
    auto command = std::make_unique<arr::SetScene>(std::nullopt);
    if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
    return SONARE_OK;
  }
  sonare::mixing::api::Scene scene;
  try {
    scene = sonare::mixing::api::scene_from_json(document);
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
                                                 document);
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

void sonare_automation_lane_desc_init(SonareAutomationLaneDesc* desc) {
  if (desc == nullptr) return;
  *desc = {};
  desc->struct_size = sizeof(SonareAutomationLaneDesc);
  desc->target_kind = SONARE_AUTOMATION_TARGET_OPAQUE;
  desc->insert_index = SONARE_AUTOMATION_INSERT_INDEX_NONE;
}

void sonare_free_project_automation_lane_list(SonareProjectAutomationLaneList* list) {
  if (list == nullptr) return;
  if (list->items != nullptr) {
    for (size_t i = 0; i < list->count; ++i) {
      SonareAutomationLaneDesc& desc = list->items[i].desc;
      delete[] desc.points;
      delete[] desc.send_id;
      delete[] desc.processor;
      delete[] desc.param_name;
    }
  }
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_automation_lanes(const SonareProject* project,
                                            SonareProjectAutomationLaneList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const arr::Project& model = project->history.project();
  struct LaneRef {
    uint32_t track_id;
    const sonare::automation::AutomationLane* lane;
  };
  std::vector<LaneRef> ordered;
  for (const arr::Track& track : model.tracks()) {
    for (const sonare::automation::AutomationLane& lane : track.automation_lanes) {
      ordered.push_back({track.id, &lane});
    }
  }
  std::sort(ordered.begin(), ordered.end(), [](const LaneRef& left, const LaneRef& right) {
    if (left.track_id != right.track_id) return left.track_id < right.track_id;
    const auto left_kind = static_cast<uint32_t>(left.lane->target_kind());
    const auto right_kind = static_cast<uint32_t>(right.lane->target_kind());
    if (left_kind != right_kind) return left_kind < right_kind;
    const auto& left_identity = left.lane->target_identity();
    const auto& right_identity = right.lane->target_identity();
    if (left_identity.send_id != right_identity.send_id) {
      return left_identity.send_id < right_identity.send_id;
    }
    if (left_identity.insert_index != right_identity.insert_index) {
      return left_identity.insert_index < right_identity.insert_index;
    }
    if (left_identity.processor != right_identity.processor) {
      return left_identity.processor < right_identity.processor;
    }
    if (left_identity.param_name != right_identity.param_name) {
      return left_identity.param_name < right_identity.param_name;
    }
    return left.lane->target_param_id() < right.lane->target_param_id();
  });

  StagedOutput<SonareProjectAutomationLaneList, &sonare_free_project_automation_lane_list> staged;
  if (!ordered.empty()) {
    staged->items = new SonareProjectAutomationLane[ordered.size()]{};
    staged->count = ordered.size();
  }
  for (size_t output_index = 0; output_index < ordered.size(); ++output_index) {
    const LaneRef& entry = ordered[output_index];
    const sonare::automation::AutomationLane& lane = *entry.lane;
    SonareProjectAutomationLane& row = staged->items[output_index];
    row.track_id = entry.track_id;
    row.lane_id = lane.lane_id();
    sonare_automation_lane_desc_init(&row.desc);
    row.desc.target_param_id = lane.target_param_id();
    row.desc.target_kind = static_cast<SonareAutomationTargetKind>(lane.target_kind());

    const sonare::automation::AutomationTargetIdentity& identity = lane.target_identity();
    if (!identity.send_id.empty()) row.desc.send_id = copy_string(identity.send_id);
    if (identity.insert_index.has_value()) {
      if (*identity.insert_index > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "automation lane insert index exceeds int32 range");
      }
      row.desc.insert_index = static_cast<int32_t>(*identity.insert_index);
    }
    if (!identity.processor.empty()) row.desc.processor = copy_string(identity.processor);
    if (!identity.param_name.empty()) row.desc.param_name = copy_string(identity.param_name);

    const std::vector<sonare::automation::Breakpoint>& points = lane.points();
    if (!points.empty()) {
      std::unique_ptr<SonareAutomationPoint[]> copied(new SonareAutomationPoint[points.size()]{});
      row.desc.point_count = points.size();
      for (size_t i = 0; i < points.size(); ++i) {
        copied[i].ppq = points[i].ppq;
        copied[i].value = points[i].value;
        copied[i].curve_to_next = static_cast<int>(points[i].curve_to_next);
      }
      row.desc.points = copied.release();
    }
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

const char* sonare_automation_target_kind_names(void) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  static const std::vector<std::string> values = {
      "opaque", "trackFaderDb", "trackPan", "trackSendDb", "trackWidth", "trackInsertParam"};
  static thread_local std::string names;
  return sonare_c_detail::join_names(values, names);
  SONARE_C_CATCH_RETURN(nullptr)
#else
  return sonare_c_detail::refuse_not_compiled_in_handle<const char>(
      sonare_c_detail::Feature::kArrangement);
#endif
}

SonareError sonare_project_add_automation_lane(SonareProject* project, uint32_t track_id,
                                               const SonareAutomationLaneDesc* desc,
                                               uint32_t* out_lane_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_lane_id) *out_lane_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  sonare::automation::AutomationLane lane;
  automation_lane_from_desc(desc, nullptr, &lane);
  const uint32_t lane_id = project->history.project().next_automation_lane_id();
  auto command = std::make_unique<arr::AddAutomationLane>(track_id, std::move(lane));
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "desc target conflicts with an automation lane already on this track");
    return SONARE_ERROR_INVALID_STATE;
  }
  if (out_lane_id) *out_lane_id = lane_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_lane_id) *out_lane_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, desc, out_lane_id);
#endif
}

SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                uint32_t lane_id,
                                                const SonareAutomationLaneDesc* desc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(desc);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  const arr::Track* track = project->history.project().find_track(track_id);
  const sonare::automation::AutomationLane& current = require_lane(*track, lane_id);
  sonare::automation::AutomationLane lane;
  automation_lane_from_desc(desc, &current, &lane);
  if (lane.target_param_id() == current.target_param_id() && lane.same_target_identity(current) &&
      lane.points() == current.points()) {
    return SONARE_OK;
  }
  auto command = std::make_unique<arr::EditAutomationLane>(track_id, lane_id, std::move(lane));
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "desc target conflicts with an automation lane already on this track");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, lane_id, desc);
#endif
}

SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  uint32_t lane_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = require_track(project, track_id); err != SONARE_OK) return err;
  SONARE_C_TRY
  require_lane(*project->history.project().find_track(track_id), lane_id);
  auto command = std::make_unique<arr::RemoveAutomationLane>(track_id, lane_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 track_id, lane_id);
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
