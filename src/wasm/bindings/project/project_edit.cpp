/// @file project_edit.cpp
/// @brief Embind project facade: undoable EditHistory-routed clip/track edits
/// and automation lanes.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

void ProjectWasm::removeClip(const val& clip_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const SonareError err = sonare_project_remove_clip(project_.get(), clip_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove clip");
  }
}

void ProjectWasm::setClipGain(const val& clip_id_val, const val& gain_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const float gain = checkedFloatFromVal(gain_val, "gain");
  const SonareError err = sonare_project_set_clip_gain(project_.get(), clip_id, gain);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip gain");
  }
}

SonareProjectClipFade ProjectWasm::clipFadeFromVal(val desc) {
  SonareProjectClipFade fade{};
  if (desc.isUndefined() || desc.isNull()) {
    return fade;
  }
  if (hasProperty(desc, "lengthPpq")) {
    fade.length_ppq = desc["lengthPpq"].as<double>();
  }
  if (hasProperty(desc, "curve")) {
    val curve = desc["curve"];
    if (curve.typeOf().as<std::string>() == "string") {
      const std::string s = curve.as<std::string>();
      if (sonare_project_fade_curve_from_name(s.c_str(), &fade.curve) != SONARE_OK) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "unknown fade curve: " + s);
      }
    } else {
      fade.curve = checkedUintFromVal(curve, "curve");
    }
  }
  return fade;
}

void ProjectWasm::setClipFade(const val& clip_id_val, val fade_in, val fade_out) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  SonareProjectClipFade in = clipFadeFromVal(fade_in);
  SonareProjectClipFade out = clipFadeFromVal(fade_out);
  const SonareError err = sonare_project_set_clip_fade(project_.get(), clip_id, &in, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip fade");
  }
}

val ProjectWasm::unresolvedAudioSourceIds() const {
  size_t count = 0;
  const SonareError err = sonare_project_unresolved_audio_source_count(project_.get(), &count);
  if (err != SONARE_OK) throwCError(err, "failed to enumerate unresolved audio sources");
  val ids = val::array();
  for (size_t i = 0; i < count; ++i) {
    uint32_t source_id = 0;
    const SonareError id_err =
        sonare_project_unresolved_audio_source_id_by_index(project_.get(), i, &source_id);
    if (id_err != SONARE_OK) throwCError(id_err, "failed to read unresolved audio source");
    ids.set(static_cast<unsigned>(i), source_id);
  }
  return ids;
}

void ProjectWasm::setSourceAudio(const val& source_id_val, val audio, const val& channels_val,
                                 const val& sample_rate_val) {
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  const int channels = checkedIntFromVal(channels_val, "channels");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> samples = float32ArrayToVector(audio);
  if (channels <= 0 || samples.size() % static_cast<size_t>(channels) != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "audio length must be a multiple of channels");
  }
  const SonareError err = sonare_project_set_source_audio(
      project_.get(), source_id, samples.data(), static_cast<int64_t>(samples.size() / channels),
      channels, sample_rate);
  if (err != SONARE_OK) throwCError(err, "failed to set source audio");
}

void ProjectWasm::setAudioSourceMetadata(const val& source_id_val, const std::string& content_hash,
                                         const std::string& external_stem_role) {
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  const SonareError err = sonare_project_set_audio_source_metadata(
      project_.get(), source_id, content_hash.c_str(), external_stem_role.c_str());
  if (err != SONARE_OK) throwCError(err, "failed to set audio source metadata");
}

void ProjectWasm::setClipTakes(const val& clip_id_val, val takes_val,
                               const val& active_take_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t active_take_id = checkedUintFromVal(active_take_id_val, "activeTakeId");
  if (!val::global("Array").call<bool>("isArray", takes_val)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "clip takes must be an array");
  }
  const size_t count = wasmArrayLikeLength(takes_val, "takes");
  std::vector<SonareProjectClipTake> takes;
  std::vector<std::string> name_storage;
  takes.reserve(count);
  name_storage.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = takes_val[static_cast<unsigned>(i)];
    SonareProjectClipTake take{};
    take.id = checkedUintFromVal(entry["id"], "id");
    take.source_id = uintProperty(entry, "sourceId", take.source_id);
    if (hasProperty(entry, "sourceOffsetPpq")) {
      take.source_offset_ppq = entry["sourceOffsetPpq"].as<double>();
    }
    if (hasProperty(entry, "name")) {
      name_storage.push_back(entry["name"].as<std::string>());
      take.name = name_storage.back().empty() ? nullptr : name_storage.back().c_str();
    }
    takes.push_back(take);
  }
  const SonareError err =
      sonare_project_set_clip_takes(project_.get(), clip_id, takes.empty() ? nullptr : takes.data(),
                                    takes.size(), active_take_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip takes");
  }
}

void ProjectWasm::setClipCompSegments(const val& clip_id_val, val segments_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  if (!val::global("Array").call<bool>("isArray", segments_val)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "clip comp segments must be an array");
  }
  const size_t count = wasmArrayLikeLength(segments_val, "segments");
  std::vector<SonareProjectClipCompSegment> segments;
  segments.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = segments_val[static_cast<unsigned>(i)];
    SonareProjectClipCompSegment segment{};
    segment.start_ppq = entry["startPpq"].as<double>();
    segment.end_ppq = entry["endPpq"].as<double>();
    segment.take_id = uintProperty(entry, "takeId", segment.take_id);
    segments.push_back(segment);
  }
  const SonareError err = sonare_project_set_clip_comp_segments(
      project_.get(), clip_id, segments.empty() ? nullptr : segments.data(), segments.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip comp segments");
  }
}

void ProjectWasm::setClipLoop(const val& clip_id_val, const val& loop_mode_val,
                              double loop_length_ppq, double loop_crossfade_ppq) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const int loop_mode = checkedIntFromVal(loop_mode_val, "loopMode");
  const SonareError err = sonare_project_set_clip_loop(project_.get(), clip_id, loop_mode,
                                                       loop_length_ppq, loop_crossfade_ppq);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip loop");
  }
}

void ProjectWasm::setClipSource(const val& clip_id_val, const val& source_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  const SonareError err = sonare_project_set_clip_source(project_.get(), clip_id, source_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip source");
  }
}

uint32_t ProjectWasm::duplicateClip(const val& clip_id_val, double new_start_ppq) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  uint32_t out = 0;
  const SonareError err =
      sonare_project_duplicate_clip(project_.get(), clip_id, new_start_ppq, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to duplicate clip");
  }
  return out;
}

void ProjectWasm::removeTrack(const val& track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareError err = sonare_project_remove_track(project_.get(), track_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove track");
  }
}

void ProjectWasm::renameTrack(const val& track_id_val, const std::string& name) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareError err =
      sonare_project_rename_track(project_.get(), track_id, name.empty() ? nullptr : name.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to rename track");
  }
}

void ProjectWasm::setTrackRoute(const val& track_id_val, const std::string& channel_strip_ref,
                                const std::string& output_target) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareError err = sonare_project_set_track_route(
      project_.get(), track_id, channel_strip_ref.empty() ? nullptr : channel_strip_ref.c_str(),
      output_target.empty() ? nullptr : output_target.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track route");
  }
}

namespace {

/// Resolve one breakpoint curve spelling to its SonareAutomationCurve ordinal.
///
/// The TS facade normalizes `curve` to an ordinal before it reaches embind, so
/// this only runs for a caller that drives the embind class directly. It still
/// accepts the same spellings the facade documents — the canonical 's-curve'
/// plus the legacy 'scurve' — because a spelling the facade takes must not
/// become a different curve here. Neither spelling may name a curve outside the
/// enum: an unrecognised name and an out-of-range, fractional or non-finite
/// ordinal are all rejected rather than silently coerced to a neighbouring
/// curve, which had quietly changed the curve shape.
int automationCurveFromVal(val curve) {
  if (curve.typeOf().as<std::string>() != "string") {
    // The numeric path is the one the direct caller above actually takes, and it
    // had no check of its own: 1.5 truncated onto a neighbouring curve and NaN
    // onto Linear, both reporting success. 2^31 was refused only because it
    // saturated past the enum, which is the domain check catching a narrowing by
    // accident -- it says nothing about the fractions in between.
    const int ordinal = checkedIntFromVal(curve, "automation curve");
    requireOrdinalInRange(ordinal, SONARE_CURVE_LINEAR, SONARE_CURVE_SCURVE, "automation curve");
    return ordinal;
  }
  const std::string s = curve.as<std::string>();
  if (s == "linear") {
    return SONARE_CURVE_LINEAR;
  }
  if (s == "exponential") {
    return SONARE_CURVE_EXPONENTIAL;
  }
  if (s == "hold") {
    return SONARE_CURVE_HOLD;
  }
  if (s == "s-curve" || s == "scurve") {
    return SONARE_CURVE_SCURVE;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown automation curve: " + s);
}

}  // namespace

std::vector<SonareAutomationPoint> ProjectWasm::automationPointsFromVal(val points) {
  std::vector<SonareAutomationPoint> out;
  if (points.isUndefined() || points.isNull()) {
    return out;
  }
  if (!val::global("Array").call<bool>("isArray", points)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "automation points must be an array");
  }
  // The point count comes from an untrusted JS `.length`: validate it through
  // the shared safe-integer + budget guard, and cap the pre-reserve so a
  // fabricated length cannot allocate storage the array does not back.
  const size_t count = wasmArrayLikeLength(points, "automation points");
  out.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (size_t i = 0; i < count; ++i) {
    val point = points[i];
    SonareAutomationPoint p{};
    p.ppq = hasProperty(point, "ppq") ? point["ppq"].as<double>() : 0.0;
    p.value = hasProperty(point, "value") ? point["value"].as<float>() : 0.0f;
    // 'curveToNext' is the Node engine's spelling of the same field and the
    // Project types document it as an alias, so reading only 'curve' turned a
    // breakpoint Node renders as an S-curve into a silent Linear here.
    if (hasProperty(point, "curve")) {
      p.curve_to_next = automationCurveFromVal(point["curve"]);
    } else if (hasProperty(point, "curveToNext")) {
      p.curve_to_next = automationCurveFromVal(point["curveToNext"]);
    } else {
      p.curve_to_next = SONARE_CURVE_LINEAR;
    }
    out.push_back(p);
  }
  return out;
}

SonareAutomationLaneDesc ProjectWasm::automationLaneDescFromVal(
    val desc, std::vector<SonareAutomationPoint>* storage) {
  SonareAutomationLaneDesc d{};
  if (desc.isUndefined() || desc.isNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "automation lane descriptor required");
  }
  d.target_param_id = uintProperty(desc, "targetParamId", 0);
  *storage = automationPointsFromVal(hasProperty(desc, "points") ? desc["points"] : val::array());
  d.points = storage->empty() ? nullptr : storage->data();
  d.point_count = storage->size();
  return d;
}

namespace {

uint32_t automationTargetKindFromVal(val desc) {
  if (!hasProperty(desc, "targetKind")) {
    return SONARE_AUTOMATION_TARGET_OPAQUE;
  }

  const val target_kind = desc["targetKind"];
  const std::string type = target_kind.typeOf().as<std::string>();
  if (type == "number") {
    const int ordinal = checkedIntFromVal(target_kind, "automation target kind");
    requireOrdinalInRange(ordinal, SONARE_AUTOMATION_TARGET_OPAQUE,
                          SONARE_AUTOMATION_TARGET_TRACK_PAN, "automation target kind");
    return static_cast<uint32_t>(ordinal);
  }
  if (type == "string") {
    const std::string name = target_kind.as<std::string>();
    if (name == "opaque") return SONARE_AUTOMATION_TARGET_OPAQUE;
    if (name == "track-fader-db") return SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
    if (name == "track-pan") return SONARE_AUTOMATION_TARGET_TRACK_PAN;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "invalid automation target kind");
}

}  // namespace

double ProjectWasm::addAutomationLane(const val& track_id_val, val desc) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  uint32_t out = 0;
  const bool has_target_kind = hasProperty(desc, "targetKind");
  SonareAutomationLaneDescEx d_ex{};
  if (has_target_kind) {
    d_ex.target_param_id = d.target_param_id;
    d_ex.target_kind = static_cast<SonareAutomationTargetKind>(automationTargetKindFromVal(desc));
    d_ex.points = d.points;
    d_ex.point_count = d.point_count;
  }
  const SonareError err =
      has_target_kind ? sonare_project_add_automation_lane_ex(project_.get(), track_id, &d_ex, &out)
                      : sonare_project_add_automation_lane(project_.get(), track_id, &d, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add automation lane");
  }
  return static_cast<double>(out);
}

void ProjectWasm::editAutomationLane(const val& track_id_val, double target_param_id, val desc) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  const bool has_target_kind = hasProperty(desc, "targetKind");
  SonareAutomationLaneDescEx d_ex{};
  if (has_target_kind) {
    d_ex.target_param_id = d.target_param_id;
    d_ex.target_kind = static_cast<SonareAutomationTargetKind>(automationTargetKindFromVal(desc));
    d_ex.points = d.points;
    d_ex.point_count = d.point_count;
  }
  const SonareError err =
      has_target_kind ? sonare_project_edit_automation_lane_ex(
                            project_.get(), track_id, static_cast<uint32_t>(target_param_id), &d_ex)
                      : sonare_project_edit_automation_lane(
                            project_.get(), track_id, static_cast<uint32_t>(target_param_id), &d);
  if (err != SONARE_OK) {
    throwCError(err, "failed to edit automation lane");
  }
}

void ProjectWasm::removeAutomationLane(const val& track_id_val, double target_param_id) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareError err = sonare_project_remove_automation_lane(
      project_.get(), track_id, static_cast<uint32_t>(target_param_id));
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove automation lane");
  }
}

void registerProjectEdit(class_<ProjectWasm>& cls) {
  cls.function("removeClip", &ProjectWasm::removeClip)
      .function("setClipGain", &ProjectWasm::setClipGain)
      .function("setClipFade", &ProjectWasm::setClipFade)
      .function("unresolvedAudioSourceIds", &ProjectWasm::unresolvedAudioSourceIds)
      .function("setSourceAudio", &ProjectWasm::setSourceAudio)
      .function("setAudioSourceMetadata", &ProjectWasm::setAudioSourceMetadata)
      .function("setClipTakes", &ProjectWasm::setClipTakes)
      .function("setClipCompSegments", &ProjectWasm::setClipCompSegments)
      .function("setClipLoop", &ProjectWasm::setClipLoop)
      .function("setClipSource", &ProjectWasm::setClipSource)
      .function("duplicateClip", &ProjectWasm::duplicateClip)
      .function("removeTrack", &ProjectWasm::removeTrack)
      .function("renameTrack", &ProjectWasm::renameTrack)
      .function("setTrackRoute", &ProjectWasm::setTrackRoute)
      .function("addAutomationLane", &ProjectWasm::addAutomationLane)
      .function("editAutomationLane", &ProjectWasm::editAutomationLane)
      .function("removeAutomationLane", &ProjectWasm::removeAutomationLane);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
