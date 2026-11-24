#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "c_api/project_input_domains.h"
#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

bool ClipFadeFromObject(Napi::Env env, const Napi::Object& obj, SonareProjectClipFade* out) {
  if (out == nullptr) return false;
  SonareProjectClipFade fade{};
  fade.length_ppq = DoubleProperty(obj, "lengthPpq", 0.0);
  const Napi::Value curve = obj.Get("curve");
  if (curve.IsString()) {
    const std::string name = sonare_node::node_narrow_string(env, curve, "curve");
    const SonareError err = sonare_project_fade_curve_from_name(name.c_str(), &fade.curve);
    if (err != SONARE_OK) {
      sonare_node::ThrowSonareErrorMessage(
          env, err,
          std::string("Invalid project fade curve: ") + sonare_node::ErrorMessageForCode(err));
      return false;
    }
  } else {
    if (!RequiredUint32Property(env, obj, "curve", &fade.curve)) return false;
  }
  if (env.IsExceptionPending()) return false;
  *out = fade;
  return true;
}

bool ParseClipTakes(Napi::Env env, const Napi::Value& value,
                    std::vector<SonareProjectClipTake>* takes,
                    std::vector<std::string>* name_storage) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "clip takes must be an array").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  const uint32_t input_count = sonare_node::node_list_length(input, "clip takes");
  takes->reserve(input_count);
  name_storage->reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Value entry = input.Get(i);
    if (env.IsExceptionPending()) return false;
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "clip take must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareProjectClipTake take{};
    if (!RequiredUint32Property(env, obj, "id", &take.id)) return false;
    take.source_id = Uint32Property(obj, "sourceId", kZeroIsSentinel);
    if (env.IsExceptionPending()) return false;
    take.source_offset_ppq = DoubleProperty(obj, "sourceOffsetPpq", take.source_offset_ppq);
    std::string name = StringProperty(obj, "name", "");
    if (env.IsExceptionPending()) return false;
    if (!name.empty()) {
      name_storage->push_back(std::move(name));
      take.name = name_storage->back().c_str();
    }
    takes->push_back(take);
  }
  return true;
}

bool ParseClipCompSegments(Napi::Env env, const Napi::Value& value,
                           std::vector<SonareProjectClipCompSegment>* segments) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "clip comp segments must be an array").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  const uint32_t input_count = sonare_node::node_list_length(input, "clip comp segments");
  segments->reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Value entry = input.Get(i);
    if (env.IsExceptionPending()) return false;
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "clip comp segment must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareProjectClipCompSegment segment{};
    if (!RequiredDoubleProperty(env, obj, "startPpq", &segment.start_ppq)) return false;
    if (!RequiredDoubleProperty(env, obj, "endPpq", &segment.end_ppq)) return false;
    segment.take_id = Uint32Property(obj, "takeId", kZeroIsSentinel);
    if (env.IsExceptionPending()) return false;
    segment.crossfade_ppq = DoubleProperty(obj, "crossfadePpq", segment.crossfade_ppq);
    if (env.IsExceptionPending()) return false;
    segments->push_back(segment);
  }
  return true;
}

// Parses a JS automation-point array ([{ppq, value, curveToNext}]) into the flat
// SonareAutomationPoint vector marshalled by the lane edit commands.
// `curveToNext` is the SonareProjectAutomationCurve ordinal, default 0 (Linear).
// Throws a JS exception (returns false) on a non-object entry.
bool ParseAutomationPoints(Napi::Env env, const Napi::Value& value,
                           std::vector<SonareAutomationPoint>* points) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "automation lane points must be an array")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  const uint32_t input_count = sonare_node::node_list_length(input, "automation lane points");
  points->reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Object obj;
    std::string at;
    SonareAutomationPoint point{};
    if (!sonare_node::RequiredElementObject(env, input, i, "points", &obj, &at) ||
        !sonare_node::RequiredDoubleValue(env, obj.Get("ppq"), at + ".ppq", &point.ppq) ||
        !sonare_node::RequiredFloatValue(env, obj.Get("value"), at + ".value", &point.value)) {
      return false;
    }
    const Napi::Value curve = obj.Get("curveToNext");
    if (env.IsExceptionPending()) return false;
    point.curve_to_next =
        curve.IsUndefined() || curve.IsNull() ? 0 : node_narrow_int(env, curve, "curveToNext");
    if (env.IsExceptionPending()) return false;
    points->push_back(point);
  }
  return true;
}

std::vector<std::string> AutomationTargetKindTokens(Napi::Env env) {
  const char* joined = sonare_automation_target_kind_names();
  if (joined == nullptr) {
    sonare_node::ThrowLastSonareError(env, "", SONARE_ERROR_UNKNOWN);
    return {};
  }
  const std::string_view names(joined);
  std::vector<std::string> tokens;
  size_t start = 0;
  for (;;) {
    const size_t end = names.find('\n', start);
    const auto token = names.substr(start, end == std::string_view::npos ? end : end - start);
    if (token.empty()) {
      sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                           "automation target names contain an empty token");
      return {};
    }
    tokens.emplace_back(token);
    if (end == std::string_view::npos) return tokens;
    start = end + 1;
  }
}

// The C token table excludes the edit-only "unchanged" value.
bool ParseAutomationTargetKind(Napi::Env env, const Napi::Value& value,
                               SonareAutomationTargetKind* out) {
  if (out == nullptr) return false;
  if (!value.IsString()) {
    Napi::TypeError::New(env, "targetKind must be a string").ThrowAsJavaScriptException();
    return false;
  }
  const std::string name = sonare_node::node_narrow_string(env, value, "targetKind");
  const auto tokens = AutomationTargetKindTokens(env);
  if (env.IsExceptionPending()) return false;
  const auto token = std::find(tokens.begin(), tokens.end(), name);
  if (token == tokens.end()) {
    Napi::RangeError::New(env, "targetKind is not a declared token: " + name)
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<SonareAutomationTargetKind>(token - tokens.begin());
  return true;
}

// Reads the optional targetKind property into @p out, leaving @p absent_kind when it is
// omitted: opaque for a new lane, SONARE_AUTOMATION_TARGET_UNCHANGED for an edit.
bool ReadAutomationTargetKind(Napi::Env env, const Napi::Object& obj,
                              SonareAutomationTargetKind absent_kind,
                              SonareAutomationTargetKind* out) {
  *out = absent_kind;
  const Napi::Value value = obj.Get("targetKind");
  if (env.IsExceptionPending()) return false;
  if (value.IsUndefined() || value.IsNull()) return true;
  return ParseAutomationTargetKind(env, value, out);
}

bool RejectAutomationIdentityField(Napi::Env env, const Napi::Value& value, const char* field) {
  if (env.IsExceptionPending()) return false;
  if (value.IsUndefined() || value.IsNull()) return true;
  Napi::RangeError::New(env, std::string(field) + " must be omitted for this targetKind")
      .ThrowAsJavaScriptException();
  return false;
}

bool CheckAutomationTargetIdentity(Napi::Env env, const Napi::Object& obj,
                                   SonareAutomationTargetKind kind, bool editing) {
  if (editing && kind == SONARE_AUTOMATION_TARGET_UNCHANGED) {
    for (const char* field : {"targetParamId", "sendId", "insertIndex", "processor", "paramName"}) {
      if (!RejectAutomationIdentityField(env, obj.Get(field), field)) return false;
    }
    return true;
  }
  if (kind != SONARE_AUTOMATION_TARGET_OPAQUE &&
      !RejectAutomationIdentityField(env, obj.Get("targetParamId"), "targetParamId")) {
    return false;
  }
  return true;
}

// Fills a SonareAutomationLaneDesc from the caller's lane descriptor.
// The breakpoints are stored in `points` (which must outlive the C call).
bool FillAutomationLaneDesc(Napi::Env env, const Napi::Object& obj,
                            std::vector<SonareAutomationPoint>* points,
                            std::vector<std::string>* identity_strings,
                            SonareAutomationLaneDesc* desc) {
  const Napi::Value target_param_id = obj.Get("targetParamId");
  if (env.IsExceptionPending()) return false;
  if (!target_param_id.IsUndefined() && !target_param_id.IsNull() &&
      !RequiredDomainValue(env, target_param_id, "targetParamId",
                           sonare_c_detail::kAutomationTargetParamIdDomain,
                           &desc->target_param_id)) {
    return false;
  }
  const auto read_string = [&](const char* key, const char** out) {
    const Napi::Value value = obj.Get(key);
    if (env.IsExceptionPending()) return false;
    if (value.IsUndefined() || value.IsNull()) return true;
    identity_strings->push_back(sonare_node::node_narrow_string(env, value, key));
    if (env.IsExceptionPending()) return false;
    *out = identity_strings->back().c_str();
    return true;
  };
  if (!read_string("sendId", &desc->send_id) || !read_string("processor", &desc->processor) ||
      !read_string("paramName", &desc->param_name)) {
    return false;
  }
  if (!sonare_node::Int32Property(env, obj, "insertIndex", SONARE_AUTOMATION_INSERT_INDEX_NONE,
                                  &desc->insert_index)) {
    return false;
  }
  if (!ParseAutomationPoints(env, obj.Get("points"), points)) {
    return false;
  }
  desc->points = points->empty() ? nullptr : points->data();
  desc->point_count = points->size();
  return true;
}

bool FillWarpMapDesc(Napi::Env env, const Napi::Object& obj,
                     std::vector<SonareProjectWarpAnchor>* anchors,
                     SonareProjectWarpMapDesc* desc) {
  const Napi::Value warp_map_id = obj.Get("id");
  if (!RequiredDomainValue(env, warp_map_id, "id", sonare_c_detail::kWarpMapIdDomain, &desc->id)) {
    return false;
  }
  desc->name = nullptr;
  Napi::Value value = obj.Get("anchors");
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "warp map anchors must be an array").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  const uint32_t input_count = sonare_node::node_list_length(input, "warp map anchors");
  anchors->reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Object anchor;
    std::string at;
    SonareProjectWarpAnchor out{};
    if (!sonare_node::RequiredElementObject(env, input, i, "anchors", &anchor, &at) ||
        !sonare_node::RequiredDoubleValue(env, anchor.Get("warpSample"), at + ".warpSample",
                                          &out.warp_sample) ||
        !sonare_node::RequiredDoubleValue(env, anchor.Get("sourceSample"), at + ".sourceSample",
                                          &out.source_sample)) {
      return false;
    }
    anchors->push_back(out);
  }
  desc->anchors = anchors->empty() ? nullptr : anchors->data();
  desc->anchor_count = anchors->size();
  return true;
}

}  // namespace

Napi::Value ProjectWrap::AddTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  SonareProjectTrackDesc desc{};
  std::string name;
  bool has_name = false;
  if (info.Length() > 0 && info[0].IsObject()) {
    Napi::Object obj = info[0].As<Napi::Object>();
    if (!RequiredIntProperty(env, obj, "kind", &desc.kind)) return env.Undefined();
    Napi::Value name_value = obj.Get("name");
    if (!name_value.IsUndefined() && !name_value.IsNull()) {
      name = sonare_node::node_narrow_string(env, name_value, "name");
      has_name = true;
    }
  } else {
    uint32_t kind = 0;
    if (!sonare_node::RequiredUint32Arg(env, info, 0, "kind", &kind)) {
      return env.Undefined();
    }
    desc.kind = static_cast<int>(kind);
  }
  desc.name = has_name ? name.c_str() : nullptr;
  uint32_t out_id = 0;
  ThrowIfErrorFlattening(env, sonare_project_add_track(project_, &desc, &out_id), "desc");
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AddClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "desc must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectClipDesc desc{};
  desc.track_id = Uint32Property(obj, "trackId", 0);
  desc.is_midi = BoolProperty(obj, "isMidi", desc.is_midi != 0) ? 1 : 0;
  desc.start_ppq = DoubleProperty(obj, "startPpq", 0.0);
  desc.length_ppq = DoubleProperty(obj, "lengthPpq", 0.0);
  desc.source_offset_ppq = DoubleProperty(obj, "sourceOffsetPpq", 0.0);
  desc.gain = FloatProperty(obj, "gain", 1.0f);
  desc.audio_sample_rate = IntProperty(obj, "audioSampleRate", 0);

  // Keep the interleaved samples alive (as a copy) for the duration of the call.
  std::vector<float> audio;
  Napi::Value audio_value = obj.Get("audio");
  if (sonare_node::IsFloat32Array(audio_value)) {
    if (!sonare_node::RequiredIntProperty(env, obj, "audioChannels", &desc.audio_channels)) {
      return env.Undefined();
    }
    Napi::Float32Array array = audio_value.As<Napi::Float32Array>();
    if (desc.audio_channels <= 0 ||
        array.ElementLength() % static_cast<size_t>(desc.audio_channels) != 0) {
      Napi::RangeError::New(env, "audio length must be a multiple of audioChannels")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    audio.assign(sonare_node::ElementData(array),
                 sonare_node::ElementData(array) + array.ElementLength());
    desc.audio_interleaved = sonare_node::ElementData(audio);
    desc.audio_frames = static_cast<int64_t>(audio.size()) / desc.audio_channels;
  }

  std::string source_uri;
  Napi::Value uri_value = obj.Get("sourceUri");
  if (!uri_value.IsUndefined() && !uri_value.IsNull()) {
    source_uri = StringProperty(obj, "sourceUri", "");
    desc.source_uri = source_uri.c_str();
  }

  uint32_t out_id = 0;
  ThrowIfErrorFlattening(env, sonare_project_add_clip(project_, &desc, &out_id), "desc");
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AddLoopRecordingTakes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "desc must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectLoopRecordingDesc desc{};
  desc.track_id = Uint32Property(obj, "trackId", 0);
  desc.start_ppq = DoubleProperty(obj, "startPpq", 0.0);
  desc.loop_length_ppq = DoubleProperty(obj, "loopLengthPpq", 0.0);
  // The capture's format is a fact about the recording, so neither has a default.
  if (!RequiredIntProperty(env, obj, "audioChannels", &desc.audio_channels) ||
      !RequiredIntProperty(env, obj, "audioSampleRate", &desc.audio_sample_rate)) {
    return env.Undefined();
  }
  const std::string partial_tail = StringProperty(obj, "partialTail", "keep");
  if (partial_tail == "drop") {
    desc.flags = SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL;
  } else if (partial_tail != "keep") {
    Napi::RangeError::New(env, "partialTail must be one of keep, drop")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<float> audio;
  Napi::Value audio_value = obj.Get("audio");
  if (sonare_node::IsFloat32Array(audio_value)) {
    Napi::Float32Array array = audio_value.As<Napi::Float32Array>();
    if (desc.audio_channels <= 0 ||
        array.ElementLength() % static_cast<size_t>(desc.audio_channels) != 0) {
      Napi::RangeError::New(env, "audio length must be a multiple of audioChannels")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    audio.assign(sonare_node::ElementData(array),
                 sonare_node::ElementData(array) + array.ElementLength());
    desc.audio_interleaved = sonare_node::ElementData(audio);
    desc.audio_frames = static_cast<int64_t>(audio.size()) / desc.audio_channels;
  }

  uint32_t out_clip_id = 0;
  size_t out_take_count = 0;
  ThrowIfErrorFlattening(
      env, sonare_project_add_loop_recording_takes(project_, &desc, &out_clip_id, &out_take_count),
      "desc");
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, out_clip_id));
  out.Set("takeCount", Napi::Number::New(env, static_cast<double>(out_take_count)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AddMidiClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double start_ppq = 0.0;
  double length_ppq = 0.0;
  if (!OptionalDoubleArg(env, info, 0, "startPpq", 0.0, &start_ppq) ||
      !OptionalDoubleArg(env, info, 1, "lengthPpq", 0.0, &length_ppq)) {
    return env.Undefined();
  }
  uint32_t out_track = 0;
  uint32_t out_clip = 0;
  ThrowIfError(
      env, sonare_project_add_midi_clip(project_, start_ppq, length_ppq, &out_track, &out_clip));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("trackId", Napi::Number::New(env, out_track));
  out.Set("clipId", Napi::Number::New(env, out_clip));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SplitClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  double ppq = 0.0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalDoubleArg(env, info, 1, "splitPpq", 0.0, &ppq)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_split_clip(project_, clip_id, ppq, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TrimClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  double start_ppq = 0.0;
  double length_ppq = 0.0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalDoubleArg(env, info, 1, "newStartPpq", 0.0, &start_ppq) ||
      !OptionalDoubleArg(env, info, 2, "newLengthPpq", 0.0, &length_ppq)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_trim_clip(project_, clip_id, start_ppq, length_ppq));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::MoveClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  double start_ppq = 0.0;
  uint32_t track_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalDoubleArg(env, info, 1, "newStartPpq", 0.0, &start_ppq) ||
      !OptionalUint32Arg(env, info, 2, "newTrackId", 0, &track_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_move_clip(project_, clip_id, start_ppq, track_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackKind(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  uint32_t kind = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "kind", 0, &kind)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_kind(project_, track_id, kind));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipWarpRef(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  uint32_t warp_ref_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalUint32Arg(env, info, 1, "warpRefId", 0, &warp_ref_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_warp_ref(project_, clip_id, warp_ref_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipWarpMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  uint32_t mode = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalUint32Arg(env, info, 1, "mode", 0, &mode)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_warp_mode(project_, clip_id,
                                                      static_cast<SonareProjectWarpMode>(mode)));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetWarpMap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "map must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareProjectWarpAnchor> anchors;
  SonareProjectWarpMapDesc desc{};
  Napi::Object obj = info[0].As<Napi::Object>();
  const std::string name_storage = StringProperty(obj, "name", "");
  if (!FillWarpMapDesc(env, obj, &anchors, &desc)) return env.Undefined();
  desc.name = name_storage.empty() ? nullptr : name_storage.c_str();
  ThrowIfErrorFlattening(env, sonare_project_set_warp_map(project_, &desc), "desc");
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RemoveWarpMap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t warp_ref_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "warpRefId", 0, &warp_ref_id)) return env.Undefined();
  ThrowIfError(env, sonare_project_remove_warp_map(project_, warp_ref_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackMidiDestination(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "destinationId", 0, &destination_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_midi_destination(project_, track_id, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetPartRig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t destination_id = 0;
  uint8_t part = 0;
  int mode = SONARE_PART_RIG_BANK;
  std::string inserts_json;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !sonare_node::RequiredMidiByteValue(env, info[1], "part", &part) ||
      !sonare_node::SynthEnumValue(env, info[2], sonare_node::kPartRigModes,
                                   sonare_node::kPartRigModeCount, "mode", &mode) ||
      !sonare_node::OptionalStringArg(env, info, 3, "inserts", "", &inserts_json)) {
    return env.Undefined();
  }
  const char* json = info[3].IsString() ? inserts_json.c_str() : nullptr;
  ThrowIfError(env, sonare_project_set_part_rig(project_, destination_id, part, mode, json));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetPartRig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t destination_id = 0;
  uint8_t part = 0;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !sonare_node::RequiredMidiByteValue(env, info[1], "part", &part)) {
    return env.Undefined();
  }
  int mode = 0;
  int present = 0;
  char* inserts = nullptr;
  ThrowIfError(
      env, sonare_project_get_part_rig(project_, destination_id, part, &mode, &inserts, &present));
  std::string inserts_json = inserts != nullptr ? inserts : "";
  const bool has_inserts = inserts != nullptr;
  sonare_free_string(inserts);
  if (env.IsExceptionPending()) return env.Undefined();
  if (!present) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("mode", sonare_node::SynthEnumName(env, mode, sonare_node::kPartRigModes,
                                             sonare_node::kPartRigModeCount));
  if (has_inserts) out.Set("inserts", Napi::String::New(env, inserts_json));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ClearPartRig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t destination_id = 0;
  uint8_t part = 0;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !sonare_node::RequiredMidiByteValue(env, info[1], "part", &part)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_clear_part_rig(project_, destination_id, part));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  float gain = 1.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalFloatArg(env, info, 1, "gain", 1.0f, &gain)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_gain(project_, track_id, gain));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackMute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  bool mute = false;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !RequiredBoolArg(env, info, 1, "mute", &mute)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_mute(project_, track_id, mute ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackSolo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  bool solo = false;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !RequiredBoolArg(env, info, 1, "solo", &solo)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_solo(project_, track_id, solo ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  float pan = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalFloatArg(env, info, 1, "pan", 0.0f, &pan)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_track_pan(project_, track_id, pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RemoveClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  ThrowIfError(env, sonare_project_remove_clip(project_, clip_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  float gain = 1.0f;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalFloatArg(env, info, 1, "gain", 1.0f, &gain)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_gain(project_, clip_id, gain));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipFade(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  // fadeIn / fadeOut are optional {lengthPpq, curve} objects. Missing sides
  // map to a zero-length linear fade to match the WASM facade and the C API's
  // "both descriptors required" contract.
  SonareProjectClipFade fade_in{};
  SonareProjectClipFade fade_out{};
  if (info.Length() > 1 && info[1].IsObject()) {
    if (!ClipFadeFromObject(env, info[1].As<Napi::Object>(), &fade_in)) return env.Undefined();
  }
  if (info.Length() > 2 && info[2].IsObject()) {
    if (!ClipFadeFromObject(env, info[2].As<Napi::Object>(), &fade_out)) return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_fade(project_, clip_id, &fade_in, &fade_out));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipTakes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  uint32_t active_take_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalUint32Arg(env, info, 2, "activeTakeId", kZeroIsSentinel, &active_take_id)) {
    return env.Undefined();
  }
  std::vector<SonareProjectClipTake> takes;
  std::vector<std::string> name_storage;
  if (!ParseClipTakes(env, info.Length() > 1 ? info[1] : env.Undefined(), &takes, &name_storage)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_project_set_clip_takes(project_, clip_id, takes.empty() ? nullptr : takes.data(),
                                         takes.size(), active_take_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipCompSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::vector<SonareProjectClipCompSegment> segments;
  if (!ParseClipCompSegments(env, info.Length() > 1 ? info[1] : env.Undefined(), &segments)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_project_set_clip_comp_segments(
               project_, clip_id, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  int loop_mode = 0;
  double loop_length_ppq = 0.0;
  double loop_crossfade_ppq = 0.0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !Int32Arg(env, info, 1, "loopMode", 0, &loop_mode) ||
      !OptionalDoubleArg(env, info, 2, "loopLengthPpq", 0.0, &loop_length_ppq) ||
      !OptionalDoubleArg(env, info, 3, "loopCrossfadePpq", 0.0, &loop_crossfade_ppq)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_loop(project_, clip_id, loop_mode, loop_length_ppq,
                                                 loop_crossfade_ppq));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetClipSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  uint32_t source_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalUint32Arg(env, info, 1, "sourceId", 0, &source_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_clip_source(project_, clip_id, source_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetAudioSourceMetadata(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (!info[1].IsString()) {
    Napi::TypeError::New(env, "contentHash must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "externalStemRole must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t source_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "sourceId", 0, &source_id)) return env.Undefined();
  const std::string content_hash = sonare_node::node_narrow_string(env, info[1], "contentHash");
  const std::string external_stem_role =
      sonare_node::node_narrow_string(env, info[2], "externalStemRole");
  ThrowIfError(env, sonare_project_set_audio_source_metadata(
                        project_, source_id, content_hash.c_str(), external_stem_role.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::DuplicateClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  double start_ppq = 0.0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalDoubleArg(env, info, 1, "newStartPpq", 0.0, &start_ppq)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_duplicate_clip(project_, clip_id, start_ppq, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RemoveTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "trackId", &track_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_remove_track(project_, track_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RenameTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id)) return env.Undefined();
  // An omitted name goes to the C ABI as nullptr, which it reads as an empty name.
  const bool has_name = info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull();
  std::string name;
  if (has_name && !OptionalStringArg(env, info, 1, "name", "", &name)) return env.Undefined();
  const char* name_ptr = has_name ? name.c_str() : nullptr;
  ThrowIfError(env, sonare_project_rename_track(project_, track_id, name_ptr));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTrackRoute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "trackId", &track_id)) {
    return env.Undefined();
  }
  // An omitted field goes as nullptr to clear it; explicit empty strings reach C for refusal.
  const bool has_strip = info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull();
  const bool has_output = info.Length() > 2 && !info[2].IsUndefined() && !info[2].IsNull();
  std::string strip;
  std::string output;
  if ((has_strip && !OptionalStringArg(env, info, 1, "channelStripRef", "", &strip)) ||
      (has_output && !OptionalStringArg(env, info, 2, "outputTarget", "", &output))) {
    return env.Undefined();
  }
  const char* strip_ptr = has_strip ? strip.c_str() : nullptr;
  const char* output_ptr = has_output ? output.c_str() : nullptr;
  ThrowIfError(env, sonare_project_set_track_route(project_, track_id, strip_ptr, output_ptr));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AddAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id)) return env.Undefined();
  if (info.Length() < 2 || !info[1].IsObject()) {
    Napi::TypeError::New(env, "lane must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareAutomationPoint> points;
  std::vector<std::string> identity_strings;
  identity_strings.reserve(3);
  SonareAutomationLaneDesc desc{};
  sonare_automation_lane_desc_init(&desc);
  const Napi::Object input = info[1].As<Napi::Object>();
  if (!ReadAutomationTargetKind(env, input, SONARE_AUTOMATION_TARGET_OPAQUE, &desc.target_kind)) {
    return env.Undefined();  // exception already pending
  }
  if (!CheckAutomationTargetIdentity(env, input, desc.target_kind, false)) {
    return env.Undefined();  // exception already pending
  }
  if (!FillAutomationLaneDesc(env, input, &points, &identity_strings, &desc)) {
    return env.Undefined();  // exception already pending
  }
  if (env.IsExceptionPending()) return env.Undefined();
  uint32_t out_lane_id = 0;
  ThrowIfErrorFlattening(
      env, sonare_project_add_automation_lane(project_, track_id, &desc, &out_lane_id), "desc");
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_lane_id));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::EditAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  uint32_t lane_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "laneId", 0, &lane_id)) {
    return env.Undefined();
  }
  if (info.Length() < 3 || !info[2].IsObject()) {
    Napi::TypeError::New(env, "lane must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareAutomationPoint> points;
  std::vector<std::string> identity_strings;
  identity_strings.reserve(3);
  SonareAutomationLaneDesc desc{};
  sonare_automation_lane_desc_init(&desc);
  const Napi::Object input = info[2].As<Napi::Object>();
  if (!ReadAutomationTargetKind(env, input, SONARE_AUTOMATION_TARGET_UNCHANGED,
                                &desc.target_kind)) {
    return env.Undefined();  // exception already pending
  }
  if (!CheckAutomationTargetIdentity(env, input, desc.target_kind, true)) {
    return env.Undefined();  // exception already pending
  }
  if (!FillAutomationLaneDesc(env, input, &points, &identity_strings, &desc)) {
    return env.Undefined();  // exception already pending
  }
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfErrorFlattening(
      env, sonare_project_edit_automation_lane(project_, track_id, lane_id, &desc), "desc");
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RemoveAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t track_id = 0;
  uint32_t lane_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "laneId", 0, &lane_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_remove_automation_lane(project_, track_id, lane_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AutomationLanes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  SonareProjectAutomationLaneList lanes{};
  struct ListGuard {
    SonareProjectAutomationLaneList* value;
    ~ListGuard() { sonare_free_project_automation_lane_list(value); }
  } guard{&lanes};
  ThrowIfError(env, sonare_project_automation_lanes(project_, &lanes));
  if (env.IsExceptionPending()) return env.Undefined();
  const auto target_kinds = AutomationTargetKindTokens(env);
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, lanes.count);
  for (size_t i = 0; i < lanes.count; ++i) {
    const SonareProjectAutomationLane& lane = lanes.items[i];
    const SonareAutomationLaneDesc& desc = lane.desc;
    Napi::Object row = Napi::Object::New(env);
    row.Set("trackId", lane.track_id);
    row.Set("laneId", lane.lane_id);
    const int32_t ordinal = static_cast<int32_t>(desc.target_kind);
    if (ordinal < 0 || static_cast<size_t>(ordinal) >= target_kinds.size()) {
      sonare_node::ThrowSonareErrorMessage(
          env, SONARE_ERROR_INVALID_FORMAT,
          "unknown automation target kind ordinal: " + std::to_string(ordinal));
      return env.Undefined();
    }
    row.Set("targetKind", target_kinds[static_cast<size_t>(ordinal)]);
    if (desc.target_kind == SONARE_AUTOMATION_TARGET_OPAQUE) {
      row.Set("targetParamId", desc.target_param_id);
    }
    if (desc.send_id != nullptr) row.Set("sendId", desc.send_id);
    if (desc.insert_index != SONARE_AUTOMATION_INSERT_INDEX_NONE) {
      row.Set("insertIndex", desc.insert_index);
    }
    if (desc.processor != nullptr) row.Set("processor", desc.processor);
    if (desc.param_name != nullptr) row.Set("paramName", desc.param_name);
    Napi::Array points = Napi::Array::New(env, desc.point_count);
    for (size_t j = 0; j < desc.point_count; ++j) {
      Napi::Object point = Napi::Object::New(env);
      point.Set("ppq", desc.points[j].ppq);
      point.Set("value", desc.points[j].value);
      point.Set("curveToNext", desc.points[j].curve_to_next);
      points.Set(static_cast<uint32_t>(j), point);
    }
    row.Set("points", points);
    out.Set(static_cast<uint32_t>(i), row);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::Undo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  ThrowIfError(env, sonare_project_undo(project_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::Redo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  ThrowIfError(env, sonare_project_redo(project_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ClearHistory(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  ThrowIfError(env, sonare_project_clear_history(project_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetMaxUndoDepth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  // A raw double -> size_t cast is undefined for a negative or NaN depth and
  // lands on SIZE_MAX in practice, which disables the very cap this call
  // exists to install. Check representability here and leave the depth
  // domain to the core.
  size_t depth = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "depth", &depth)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_max_undo_depth(project_, depth));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetMaxHistoryBytes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t bytes = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "bytes", &bytes)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_max_history_bytes(project_, bytes));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}
