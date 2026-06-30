/// @file project_edit.cpp
/// @brief Embind project facade: undoable EditHistory-routed clip/track edits
/// and automation lanes.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

void ProjectWasm::removeClip(uint32_t clip_id) {
  const SonareError err = sonare_project_remove_clip(project_.get(), clip_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove clip");
  }
}

void ProjectWasm::setClipGain(uint32_t clip_id, float gain) {
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
      fade.curve = (s == "equalPower" || s == "equal-power" || s == "equal_power")
                       ? SONARE_FADE_CURVE_EQUAL_POWER
                   : s == "exponential" ? SONARE_FADE_CURVE_EXPONENTIAL
                   : s == "logarithmic" ? SONARE_FADE_CURVE_LOGARITHMIC
                                        : SONARE_FADE_CURVE_LINEAR;
    } else {
      fade.curve = curve.as<uint32_t>();
    }
  }
  return fade;
}

void ProjectWasm::setClipFade(uint32_t clip_id, val fade_in, val fade_out) {
  SonareProjectClipFade in = clipFadeFromVal(fade_in);
  SonareProjectClipFade out = clipFadeFromVal(fade_out);
  const SonareError err = sonare_project_set_clip_fade(project_.get(), clip_id, &in, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip fade");
  }
}

void ProjectWasm::setClipTakes(uint32_t clip_id, val takes_val, uint32_t active_take_id) {
  if (!val::global("Array").call<bool>("isArray", takes_val)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "clip takes must be an array");
  }
  const size_t count = takes_val["length"].as<size_t>();
  std::vector<SonareProjectClipTake> takes;
  std::vector<std::string> name_storage;
  takes.reserve(count);
  name_storage.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = takes_val[static_cast<unsigned>(i)];
    SonareProjectClipTake take{};
    take.id = entry["id"].as<uint32_t>();
    if (hasProperty(entry, "sourceId")) {
      take.source_id = entry["sourceId"].as<uint32_t>();
    }
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

void ProjectWasm::setClipCompSegments(uint32_t clip_id, val segments_val) {
  if (!val::global("Array").call<bool>("isArray", segments_val)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "clip comp segments must be an array");
  }
  const size_t count = segments_val["length"].as<size_t>();
  std::vector<SonareProjectClipCompSegment> segments;
  segments.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = segments_val[static_cast<unsigned>(i)];
    SonareProjectClipCompSegment segment{};
    segment.start_ppq = entry["startPpq"].as<double>();
    segment.end_ppq = entry["endPpq"].as<double>();
    if (hasProperty(entry, "takeId")) {
      segment.take_id = entry["takeId"].as<uint32_t>();
    }
    segments.push_back(segment);
  }
  const SonareError err = sonare_project_set_clip_comp_segments(
      project_.get(), clip_id, segments.empty() ? nullptr : segments.data(), segments.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip comp segments");
  }
}

void ProjectWasm::setClipLoop(uint32_t clip_id, int loop_mode, double loop_length_ppq,
                              double loop_crossfade_ppq) {
  const SonareError err = sonare_project_set_clip_loop(project_.get(), clip_id, loop_mode,
                                                       loop_length_ppq, loop_crossfade_ppq);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip loop");
  }
}

void ProjectWasm::setClipSource(uint32_t clip_id, uint32_t source_id) {
  const SonareError err = sonare_project_set_clip_source(project_.get(), clip_id, source_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip source");
  }
}

uint32_t ProjectWasm::duplicateClip(uint32_t clip_id, double new_start_ppq) {
  uint32_t out = 0;
  const SonareError err =
      sonare_project_duplicate_clip(project_.get(), clip_id, new_start_ppq, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to duplicate clip");
  }
  return out;
}

void ProjectWasm::removeTrack(uint32_t track_id) {
  const SonareError err = sonare_project_remove_track(project_.get(), track_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove track");
  }
}

void ProjectWasm::renameTrack(uint32_t track_id, const std::string& name) {
  const SonareError err =
      sonare_project_rename_track(project_.get(), track_id, name.empty() ? nullptr : name.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to rename track");
  }
}

void ProjectWasm::setTrackRoute(uint32_t track_id, const std::string& channel_strip_ref,
                                const std::string& output_target) {
  const SonareError err = sonare_project_set_track_route(
      project_.get(), track_id, channel_strip_ref.empty() ? nullptr : channel_strip_ref.c_str(),
      output_target.empty() ? nullptr : output_target.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track route");
  }
}

std::vector<SonareAutomationPoint> ProjectWasm::automationPointsFromVal(val points) {
  std::vector<SonareAutomationPoint> out;
  if (points.isUndefined() || points.isNull()) {
    return out;
  }
  if (!val::global("Array").call<bool>("isArray", points)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "automation points must be an array");
  }
  const size_t count = points["length"].as<size_t>();
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val point = points[i];
    SonareAutomationPoint p{};
    p.ppq = hasProperty(point, "ppq") ? point["ppq"].as<double>() : 0.0;
    p.value = hasProperty(point, "value") ? point["value"].as<float>() : 0.0f;
    if (hasProperty(point, "curve")) {
      val curve = point["curve"];
      if (curve.typeOf().as<std::string>() == "string") {
        const std::string s = curve.as<std::string>();
        p.curve_to_next = s == "exponential" ? SONARE_CURVE_EXPONENTIAL
                          : s == "hold"      ? SONARE_CURVE_HOLD
                          : s == "scurve"    ? SONARE_CURVE_SCURVE
                                             : SONARE_CURVE_LINEAR;
      } else {
        p.curve_to_next = curve.as<int>();
      }
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
  d.target_param_id = hasProperty(desc, "targetParamId") ? desc["targetParamId"].as<uint32_t>() : 0;
  *storage = automationPointsFromVal(hasProperty(desc, "points") ? desc["points"] : val::array());
  d.points = storage->empty() ? nullptr : storage->data();
  d.point_count = storage->size();
  return d;
}

double ProjectWasm::addAutomationLane(uint32_t track_id, val desc) {
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  size_t out = 0;
  const SonareError err = sonare_project_add_automation_lane(project_.get(), track_id, &d, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add automation lane");
  }
  return static_cast<double>(out);
}

void ProjectWasm::editAutomationLane(uint32_t track_id, double lane_index, val desc) {
  std::vector<SonareAutomationPoint> storage;
  SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
  const SonareError err = sonare_project_edit_automation_lane(project_.get(), track_id,
                                                              static_cast<size_t>(lane_index), &d);
  if (err != SONARE_OK) {
    throwCError(err, "failed to edit automation lane");
  }
}

void ProjectWasm::removeAutomationLane(uint32_t track_id, double lane_index) {
  const SonareError err = sonare_project_remove_automation_lane(project_.get(), track_id,
                                                                static_cast<size_t>(lane_index));
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove automation lane");
  }
}

void registerProjectEdit(class_<ProjectWasm>& cls) {
  cls.function("removeClip", &ProjectWasm::removeClip)
      .function("setClipGain", &ProjectWasm::setClipGain)
      .function("setClipFade", &ProjectWasm::setClipFade)
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
