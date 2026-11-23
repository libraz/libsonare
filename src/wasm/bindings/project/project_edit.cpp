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

/// Resolve one breakpoint curve ordinal, the form the TS facade hands embind.
///
/// An out-of-range, fractional or non-finite ordinal is refused rather than
/// coerced onto a neighbouring curve; the facade resolves the public tokens.
int automationCurveFromVal(val curve) {
  const int ordinal = checkedIntFromVal(curve, "automation curve");
  requireOrdinalInRange(ordinal, SONARE_CURVE_LINEAR, SONARE_CURVE_SCURVE, "automation curve");
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
    val point = points[i];
    SonareAutomationPoint p{};
    p.ppq = doubleProperty(point, "ppq", 0.0);
    p.value = floatProperty(point, "value", 0.0f);
    p.curve_to_next = hasProperty(point, "curveToNext")
                          ? automationCurveFromVal(point["curveToNext"])
                          : SONARE_CURVE_LINEAR;
    out.push_back(p);
  }
  return out;
}

SonareAutomationLaneDesc ProjectWasm::automationLaneDescFromVal(
    val desc, std::vector<SonareAutomationPoint>* storage) {
  SonareAutomationLaneDesc d{};
  if (desc.isUndefined() || desc.isNull()) {
    throw WasmTypeError("desc must be given");
  }
  d.target_param_id = optionalDomainValue<uint32_t>(desc["targetParamId"], "targetParamId",
                                                    sonare_c_detail::kAutomationTargetParamIdDomain)
                          .value_or(0);
  *storage = automationPointsFromVal(hasProperty(desc, "points") ? desc["points"] : val::array());
  d.points = storage->empty() ? nullptr : storage->data();
  d.point_count = storage->size();
  return d;
}

namespace {

SonareAutomationTargetKind automationTargetKindFromVal(val desc,
                                                       SonareAutomationTargetKind omitted) {
  if (!hasProperty(desc, "targetKind")) {
    return omitted;
  }

  const val target_kind = desc["targetKind"];
  const std::string type = target_kind.typeOf().as<std::string>();
  if (type == "number") {
    const int ordinal = checkedIntFromVal(target_kind, "automation target kind");
    requireOrdinalInRange(ordinal, SONARE_AUTOMATION_TARGET_OPAQUE,
                          SONARE_AUTOMATION_TARGET_TRACK_PAN, "automation target kind");
    return static_cast<SonareAutomationTargetKind>(ordinal);
  }
  if (type == "string") {
    const std::string name = target_kind.as<std::string>();
    if (name == "opaque") return SONARE_AUTOMATION_TARGET_OPAQUE;
    if (name == "track-fader-db") return SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
    if (name == "track-pan") return SONARE_AUTOMATION_TARGET_TRACK_PAN;
  }
  throw WasmRangeError("invalid automation target kind");
}

}  // namespace

double ProjectWasm::addAutomationLane(const val& track_id_val, val desc) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  uint32_t out = 0;
  d.target_kind = automationTargetKindFromVal(desc, SONARE_AUTOMATION_TARGET_OPAQUE);
  checkCodedCErrorFlattening(sonare_project_add_automation_lane(project_.get(), track_id, &d, &out),
                             "desc");
  return static_cast<double>(out);
}

void ProjectWasm::editAutomationLane(const val& track_id_val, const val& target_param_id_val,
                                     val desc) {
  const uint32_t target_param_id = checkedUintFromVal(target_param_id_val, "targetParamId");
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  d.target_kind = automationTargetKindFromVal(desc, SONARE_AUTOMATION_TARGET_UNCHANGED);
  checkCodedCError(
      sonare_project_edit_automation_lane(project_.get(), track_id, target_param_id, &d));
}

void ProjectWasm::removeAutomationLane(const val& track_id_val, const val& target_param_id_val) {
  const uint32_t target_param_id = checkedUintFromVal(target_param_id_val, "targetParamId");
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(
      sonare_project_remove_automation_lane(project_.get(), track_id, target_param_id));
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
      .function("removeAutomationLane", &ProjectWasm::removeAutomationLane);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
