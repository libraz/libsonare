/// @file project_edit.cpp
/// @brief Embind project facade: undoable EditHistory-routed clip/track edits
/// and automation lanes.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>

#include "c_api/project_input_domains.h"
#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

void ProjectWasm::removeClip(const val& clip_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  checkCodedCError(sonare_project_remove_clip(project_.get(), clip_id));
}

void ProjectWasm::setClipGain(const val& clip_id_val, const val& gain_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const float gain = checkedFloatFromVal(gain_val, "gain");
  checkCodedCError(sonare_project_set_clip_gain(project_.get(), clip_id, gain));
}

SonareProjectClipFade ProjectWasm::clipFadeFromVal(val desc) {
  SonareProjectClipFade fade{};
  if (desc.isUndefined() || desc.isNull()) {
    return fade;
  }
  if (hasProperty(desc, "lengthPpq")) {
    fade.length_ppq = numberFromVal(desc["lengthPpq"], "lengthPpq");
  }
  if (hasProperty(desc, "curve")) {
    val curve = desc["curve"];
    if (curve.typeOf().as<std::string>() == "string") {
      const std::string s = curve.as<std::string>();
      if (sonare_project_fade_curve_from_name(wasmCString(s, "curve"), &fade.curve) != SONARE_OK) {
        throw WasmRangeError("unknown fade curve: " + s);
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
  checkCodedCError(sonare_project_set_clip_fade(project_.get(), clip_id, &in, &out));
}

val ProjectWasm::unresolvedAudioSourceIds() const {
  size_t count = 0;
  const SonareError err = sonare_project_unresolved_audio_source_count(project_.get(), &count);
  if (err != SONARE_OK) throwCodedCError(err);
  val ids = val::array();
  for (size_t i = 0; i < count; ++i) {
    uint32_t source_id = 0;
    const SonareError id_err =
        sonare_project_unresolved_audio_source_id_by_index(project_.get(), i, &source_id);
    if (id_err != SONARE_OK) throwCodedCError(id_err);
    ids.set(static_cast<unsigned>(i), source_id);
  }
  return ids;
}

val ProjectWasm::unresolvedAudioSources() const {
  size_t count = 0;
  const SonareError err = sonare_project_unresolved_audio_source_count(project_.get(), &count);
  if (err != SONARE_OK) throwCodedCError(err);
  val sources = val::array();
  for (size_t i = 0; i < count; ++i) {
    SonareProjectSource source{};
    const SonareError source_err =
        sonare_project_unresolved_audio_source_by_index(project_.get(), i, &source);
    if (source_err != SONARE_OK) throwCodedCError(source_err);
    char* uri = nullptr;
    const SonareError uri_err =
        sonare_project_get_audio_source_uri(project_.get(), source.id, &uri);
    if (uri_err != SONARE_OK) throwCodedCError(uri_err);
    const std::unique_ptr<char, decltype(&sonare_free_string)> owned(uri, &sonare_free_string);
    sources.set(static_cast<unsigned>(i), sourceToVal(source, uri));
  }
  return sources;
}

void ProjectWasm::setSourceAudio(const val& source_id_val, val audio, const val& channels_val,
                                 const val& sample_rate_val) {
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  const int channels = checkedIntFromVal(channels_val, "channels");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<float> samples = float32ArrayToVector(audio, "audio");
  if (channels <= 0 || samples.size() % static_cast<size_t>(channels) != 0) {
    throw WasmRangeError("audio length must be a multiple of channels");
  }
  checkCodedCError(sonare_project_set_source_audio(project_.get(), source_id, elementData(samples),
                                                   static_cast<int64_t>(samples.size() / channels),
                                                   channels, sample_rate));
}

void ProjectWasm::setAudioSourceMetadata(const val& source_id_val, const std::string& content_hash,
                                         const std::string& external_stem_role) {
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  checkCodedCError(sonare_project_set_audio_source_metadata(
      project_.get(), source_id, wasmCString(content_hash, "contentHash"),
      wasmCString(external_stem_role, "externalStemRole")));
}

void ProjectWasm::setClipTakes(const val& clip_id_val, val takes_val,
                               const val& active_take_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t active_take_id = checkedUintFromVal(active_take_id_val, "activeTakeId");
  if (!val::global("Array").call<bool>("isArray", takes_val)) {
    throw WasmTypeError("clip takes must be an array");
  }
  const size_t count = wasmArrayLength(takes_val, "takes");
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
      take.source_offset_ppq = numberFromVal(entry["sourceOffsetPpq"], "sourceOffsetPpq");
    }
    if (hasProperty(entry, "name")) {
      name_storage.push_back(entry["name"].as<std::string>());
      take.name = name_storage.back().empty() ? nullptr : wasmCString(name_storage.back(), "name");
    }
    takes.push_back(take);
  }
  checkCodedCError(sonare_project_set_clip_takes(project_.get(), clip_id,
                                                 takes.empty() ? nullptr : takes.data(),
                                                 takes.size(), active_take_id));
}

void ProjectWasm::setClipCompSegments(const val& clip_id_val, val segments_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  if (!val::global("Array").call<bool>("isArray", segments_val)) {
    throw WasmTypeError("clip comp segments must be an array");
  }
  const size_t count = wasmArrayLength(segments_val, "segments");
  std::vector<SonareProjectClipCompSegment> segments;
  segments.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = segments_val[static_cast<unsigned>(i)];
    SonareProjectClipCompSegment segment{};
    segment.start_ppq = numberFromVal(entry["startPpq"], "startPpq");
    segment.end_ppq = numberFromVal(entry["endPpq"], "endPpq");
    segment.take_id = uintProperty(entry, "takeId", segment.take_id);
    segment.crossfade_ppq = doubleProperty(entry, "crossfadePpq", segment.crossfade_ppq);
    segments.push_back(segment);
  }
  checkCodedCError(sonare_project_set_clip_comp_segments(
      project_.get(), clip_id, segments.empty() ? nullptr : segments.data(), segments.size()));
}

void ProjectWasm::setClipLoop(const val& clip_id_val, const val& loop_mode_val,
                              const val& loop_length_ppq_val, const val& loop_crossfade_ppq_val) {
  const double loop_length_ppq = checkedDoubleFromVal(loop_length_ppq_val, "loopLengthPpq");
  const double loop_crossfade_ppq =
      checkedDoubleFromVal(loop_crossfade_ppq_val, "loopCrossfadePpq");
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const int loop_mode = checkedIntFromVal(loop_mode_val, "loopMode");
  checkCodedCError(sonare_project_set_clip_loop(project_.get(), clip_id, loop_mode, loop_length_ppq,
                                                loop_crossfade_ppq));
}

void ProjectWasm::setClipSource(const val& clip_id_val, const val& source_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  checkCodedCError(sonare_project_set_clip_source(project_.get(), clip_id, source_id));
}

uint32_t ProjectWasm::duplicateClip(const val& clip_id_val, const val& new_start_ppq_val) {
  const double new_start_ppq = checkedDoubleFromVal(new_start_ppq_val, "newStartPpq");
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  uint32_t out = 0;
  checkCodedCError(sonare_project_duplicate_clip(project_.get(), clip_id, new_start_ppq, &out));
  return out;
}

void ProjectWasm::removeTrack(const val& track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(sonare_project_remove_track(project_.get(), track_id));
}

void ProjectWasm::renameTrack(const val& track_id_val, const std::string& name) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(sonare_project_rename_track(project_.get(), track_id,
                                               name.empty() ? nullptr : wasmCString(name, "name")));
}

void ProjectWasm::setTrackRoute(const val& track_id_val, const std::string& channel_strip_ref,
                                const std::string& output_target) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(sonare_project_set_track_route(
      project_.get(), track_id,
      channel_strip_ref.empty() ? nullptr : wasmCString(channel_strip_ref, "channelStripRef"),
      output_target.empty() ? nullptr : wasmCString(output_target, "outputTarget")));
}

namespace {

SonareAutomationTargetKind automationTargetKindFromVal(val desc,
                                                       SonareAutomationTargetKind omitted) {
  if (!hasProperty(desc, "targetKind")) return omitted;
  const val target_kind = desc["targetKind"];
  if (target_kind.typeOf().as<std::string>() != "string") {
    throw WasmTypeError("targetKind must be a string token");
  }
  const std::string name = target_kind.as<std::string>();
  const std::vector<std::string> tokens = projectAutomationTargetKindTokens();
  const auto found = std::find(tokens.begin(), tokens.end(), name);
  if (found == tokens.end()) {
    throw WasmRangeError("unknown automation target kind token: " + name);
  }
  return static_cast<SonareAutomationTargetKind>(std::distance(tokens.begin(), found));
}

struct ProjectAutomationLaneListGuard {
  SonareProjectAutomationLaneList value{};

  ~ProjectAutomationLaneListGuard() { sonare_free_project_automation_lane_list(&value); }
};

/// Resolve one breakpoint curve ordinal, the form the TS facade hands embind.
///
/// An out-of-range, fractional or non-finite ordinal is refused rather than
/// coerced onto a neighbouring curve; the facade resolves the public tokens.
int automationCurveFromVal(val curve) {
  const int ordinal = checkedIntFromVal(curve, "automation curve");
  requireEnumOrdinal(ordinal, SONARE_CURVE_SCURVE + 1, "automation curve");
  return ordinal;
}

}  // namespace

std::vector<SonareAutomationPoint> ProjectWasm::automationPointsFromVal(val points) {
  std::vector<SonareAutomationPoint> out;
  if (points.isUndefined() || points.isNull()) {
    return out;
  }
  if (!val::global("Array").call<bool>("isArray", points)) {
    throw WasmTypeError("automation points must be an array");
  }
  // The point count comes from an untrusted JS `.length`: validate it through
  // the shared safe-integer + budget guard, and cap the pre-reserve so a
  // fabricated length cannot allocate storage the array does not back.
  const size_t count = wasmArrayLength(points, "points");
  out.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (size_t i = 0; i < count; ++i) {
    const val point = requiredElementObject(points, i, "points");
    const std::string at = elementAt("points", i);
    SonareAutomationPoint p{};
    p.ppq = checkedDoubleFromVal(requiredNumberElementProperty(point, "ppq", at),
                                 elementField(at, "ppq").c_str());
    p.value = checkedFloatFromVal(requiredNumberElementProperty(point, "value", at),
                                  elementField(at, "value").c_str());
    p.curve_to_next = hasProperty(point, "curveToNext")
                          ? automationCurveFromVal(point["curveToNext"])
                          : SONARE_CURVE_LINEAR;
    out.push_back(p);
  }
  return out;
}

SonareAutomationLaneDesc ProjectWasm::automationLaneDescFromVal(
    val desc, std::vector<SonareAutomationPoint>* storage, std::vector<std::string>* string_storage,
    SonareAutomationTargetKind target_kind) {
  SonareAutomationLaneDesc d{};
  if (desc.isUndefined() || desc.isNull()) {
    throw WasmTypeError("desc must be given");
  }
  sonare_automation_lane_desc_init(&d);
  d.target_kind = target_kind;
  if (target_kind == SONARE_AUTOMATION_TARGET_UNCHANGED) {
    for (const char* field : {"targetParamId", "sendId", "processor", "paramName", "insertIndex"}) {
      if (hasProperty(desc, field)) {
        throw WasmRangeError(std::string(field) +
                             " requires an explicit targetKind when editing a lane");
      }
    }
  } else if (target_kind == SONARE_AUTOMATION_TARGET_OPAQUE) {
    d.target_param_id = domainNumberFromVal<uint32_t>(
        requiredDomainIntProperty(desc, "targetParamId", ""), "targetParamId",
        sonare_c_detail::kAutomationTargetParamIdDomain);
  } else if (hasProperty(desc, "targetParamId")) {
    throw WasmRangeError("targetParamId must be omitted for a typed target");
  }
  string_storage->reserve(4);
  if (hasProperty(desc, "sendId")) {
    string_storage->push_back(stringFromVal(desc["sendId"], "sendId"));
    d.send_id = wasmCString(string_storage->back(), "sendId");
  }
  if (hasProperty(desc, "processor")) {
    string_storage->push_back(stringFromVal(desc["processor"], "processor"));
    d.processor = wasmCString(string_storage->back(), "processor");
  }
  if (hasProperty(desc, "paramName")) {
    string_storage->push_back(stringFromVal(desc["paramName"], "paramName"));
    d.param_name = wasmCString(string_storage->back(), "paramName");
  }
  if (hasProperty(desc, "insertIndex")) {
    d.insert_index = checkedIntFromVal(desc["insertIndex"], "insertIndex");
    if (d.insert_index < 0) throw WasmRangeError("insertIndex must be non-negative");
  }
  *storage = automationPointsFromVal(hasProperty(desc, "points") ? desc["points"] : val::array());
  d.points = storage->empty() ? nullptr : storage->data();
  d.point_count = storage->size();
  return d;
}

double ProjectWasm::addAutomationLane(const val& track_id_val, val desc) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareAutomationTargetKind target_kind =
      automationTargetKindFromVal(desc, SONARE_AUTOMATION_TARGET_OPAQUE);
  std::vector<SonareAutomationPoint> storage;
  std::vector<std::string> string_storage;
  SonareAutomationLaneDesc d =
      automationLaneDescFromVal(desc, &storage, &string_storage, target_kind);
  uint32_t out = 0;
  checkCodedCErrorFlattening(sonare_project_add_automation_lane(project_.get(), track_id, &d, &out),
                             "desc", CErrorReasonMode::kCurrentEntry);
  return static_cast<double>(out);
}

void ProjectWasm::editAutomationLane(const val& track_id_val, const val& lane_id_val, val desc) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t lane_id = checkedUintFromVal(lane_id_val, "laneId");
  const SonareAutomationTargetKind target_kind =
      automationTargetKindFromVal(desc, SONARE_AUTOMATION_TARGET_UNCHANGED);
  std::vector<SonareAutomationPoint> storage;
  std::vector<std::string> string_storage;
  SonareAutomationLaneDesc d =
      automationLaneDescFromVal(desc, &storage, &string_storage, target_kind);
  checkCodedCErrorFlattening(
      sonare_project_edit_automation_lane(project_.get(), track_id, lane_id, &d), "desc",
      CErrorReasonMode::kCurrentEntry);
}

void ProjectWasm::removeAutomationLane(const val& track_id_val, const val& lane_id_val) {
  const uint32_t lane_id = checkedUintFromVal(lane_id_val, "laneId");
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const SonareError err = sonare_project_remove_automation_lane(project_.get(), track_id, lane_id);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
}

val ProjectWasm::automationLanes() const {
  ProjectAutomationLaneListGuard list;
  const SonareError err = sonare_project_automation_lanes(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  if (list.value.count != 0 && list.value.items == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "automationLanes returned a missing lane array");
  }
  const std::vector<std::string> tokens = projectAutomationTargetKindTokens();
  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareProjectAutomationLane& lane = list.value.items[i];
    const int kind = static_cast<int>(lane.desc.target_kind);
    if (kind < 0 || static_cast<size_t>(kind) >= tokens.size()) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidFormat,
          "unknown automation target kind ordinal: " + std::to_string(kind));
    }
    if (lane.desc.point_count != 0 && lane.desc.points == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "automationLanes returned missing breakpoint data");
    }
    val row = val::object();
    row.set("trackId", lane.track_id);
    row.set("laneId", lane.lane_id);
    row.set("targetKind", tokens[static_cast<size_t>(kind)]);
    if (kind == SONARE_AUTOMATION_TARGET_OPAQUE) {
      row.set("targetParamId", lane.desc.target_param_id);
    }
    if (lane.desc.send_id != nullptr) row.set("sendId", std::string(lane.desc.send_id));
    if (lane.desc.processor != nullptr) row.set("processor", std::string(lane.desc.processor));
    if (lane.desc.param_name != nullptr) row.set("paramName", std::string(lane.desc.param_name));
    if (lane.desc.insert_index != SONARE_AUTOMATION_INSERT_INDEX_NONE) {
      row.set("insertIndex", lane.desc.insert_index);
    }
    val points = val::array();
    for (size_t point_index = 0; point_index < lane.desc.point_count; ++point_index) {
      const SonareAutomationPoint& point = lane.desc.points[point_index];
      val output_point = val::object();
      output_point.set("ppq", point.ppq);
      output_point.set("value", point.value);
      output_point.set("curveToNext", static_cast<int>(point.curve_to_next));
      points.set(static_cast<unsigned>(point_index), output_point);
    }
    row.set("points", points);
    out.set(static_cast<unsigned>(i), row);
  }
  return out;
}

void registerProjectEdit(class_<ProjectWasm>& cls) {
  cls.function("removeClip", &ProjectWasm::removeClip)
      .function("setClipGain", &ProjectWasm::setClipGain)
      .function("setClipFade", &ProjectWasm::setClipFade)
      .function("unresolvedAudioSourceIds", &ProjectWasm::unresolvedAudioSourceIds)
      .function("unresolvedAudioSources", &ProjectWasm::unresolvedAudioSources)
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
      .function("removeAutomationLane", &ProjectWasm::removeAutomationLane)
      .function("automationLanes", &ProjectWasm::automationLanes);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
