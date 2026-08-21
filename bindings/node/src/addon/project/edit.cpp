#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

bool ClipFadeFromObject(Napi::Env env, const Napi::Object& obj, SonareProjectClipFade* out) {
  if (out == nullptr) return false;
  SonareProjectClipFade fade{};
  const Napi::Value length = obj.Get("lengthPpq");
  fade.length_ppq = length.IsNumber() ? length.As<Napi::Number>().DoubleValue() : 0.0;
  const Napi::Value curve = obj.Get("curve");
  if (curve.IsString()) {
    const std::string name = curve.As<Napi::String>().Utf8Value();
    const SonareError err = sonare_project_fade_curve_from_name(name.c_str(), &fade.curve);
    if (err != SONARE_OK) {
      sonare_node::ThrowSonareError(env, err, "Invalid project fade curve: ");
      return false;
    }
  } else {
    fade.curve = static_cast<uint32_t>(IntProperty(obj, "curve", SONARE_FADE_CURVE_LINEAR));
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
  takes->reserve(input.Length());
  name_storage->reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value entry = input.Get(i);
    if (env.IsExceptionPending()) return false;
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "clip take must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareProjectClipTake take{};
    if (!RequiredUint32Property(env, obj, "id", &take.id)) return false;
    take.source_id = static_cast<uint32_t>(IntProperty(obj, "sourceId", 0));
    if (env.IsExceptionPending()) return false;
    const Napi::Value source_offset = obj.Get("sourceOffsetPpq");
    if (env.IsExceptionPending()) return false;
    take.source_offset_ppq = source_offset.IsUndefined() || source_offset.IsNull()
                                 ? 0.0
                                 : source_offset.As<Napi::Number>().DoubleValue();
    if (env.IsExceptionPending()) return false;
    const Napi::Value name = obj.Get("name");
    if (env.IsExceptionPending()) return false;
    if (name.IsString()) {
      name_storage->push_back(name.As<Napi::String>().Utf8Value());
      if (env.IsExceptionPending()) return false;
      take.name = name_storage->back().empty() ? nullptr : name_storage->back().c_str();
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
  segments->reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
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
    segment.take_id = static_cast<uint32_t>(IntProperty(obj, "takeId", 0));
    if (env.IsExceptionPending()) return false;
    segments->push_back(segment);
  }
  return true;
}

// Parses a JS automation-point array ([{ppq, value, curve}]) into the flat
// SonareAutomationPoint vector marshalled by the lane edit commands. `curve`
// (alias `curveToNext`) is the SonareProjectAutomationCurve ordinal, default 0
// (Linear). Throws a JS exception (returns false) on a non-object entry.
bool ParseAutomationPoints(Napi::Env env, const Napi::Value& value,
                           std::vector<SonareAutomationPoint>* points) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "automation lane points must be an array")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  points->reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value entry = input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "automation point must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareAutomationPoint point{};
    if (!RequiredDoubleProperty(env, obj, "ppq", &point.ppq)) return false;
    if (!RequiredFloatProperty(env, obj, "value", &point.value)) return false;
    Napi::Value curve = obj.Get("curve");
    if (env.IsExceptionPending()) return false;
    if (curve.IsUndefined() || curve.IsNull()) curve = obj.Get("curveToNext");
    point.curve_to_next =
        curve.IsUndefined() || curve.IsNull() ? 0 : curve.As<Napi::Number>().Int32Value();
    if (env.IsExceptionPending()) return false;
    points->push_back(point);
  }
  return true;
}

// Reads the optional target-kind property while retaining whether it was
// supplied. The legacy descriptor has no target-kind field, so that distinction
// selects the legacy C ABI versus its additive `_ex` form.
bool ParseAutomationTargetKind(Napi::Env env, const Napi::Value& value, uint32_t* out) {
  if (out == nullptr) return false;
  if (value.IsString()) {
    const std::string name = value.As<Napi::String>().Utf8Value();
    if (name == "opaque") {
      *out = SONARE_AUTOMATION_TARGET_OPAQUE;
      return true;
    }
    if (name == "track-fader-db") {
      *out = SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
      return true;
    }
    if (name == "track-pan") {
      *out = SONARE_AUTOMATION_TARGET_TRACK_PAN;
      return true;
    }
    Napi::RangeError::New(env, "Invalid automation target kind: " + name)
        .ThrowAsJavaScriptException();
    return false;
  }
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, "automation target kind must be a string or number")
        .ThrowAsJavaScriptException();
    return false;
  }
  const double ordinal = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(ordinal) || std::trunc(ordinal) != ordinal || ordinal < 0.0 || ordinal > 2.0) {
    Napi::RangeError::New(env, "Invalid automation target kind: " + std::to_string(ordinal))
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<uint32_t>(ordinal);
  return true;
}

bool ReadAutomationTargetKind(Napi::Env env, const Napi::Object& obj, bool* present,
                              uint32_t* out) {
  if (present == nullptr || out == nullptr) return false;
  *present = false;
  const Napi::Value value = obj.Get("targetKind");
  if (env.IsExceptionPending()) return false;
  if (value.IsUndefined()) return true;
  *present = true;
  return ParseAutomationTargetKind(env, value, out);
}

// Fills a SonareAutomationLaneDesc from a JS object {targetParamId, points}.
// The breakpoints are stored in `points` (which must outlive the C call).
bool FillAutomationLaneDesc(Napi::Env env, const Napi::Object& obj,
                            std::vector<SonareAutomationPoint>* points,
                            SonareAutomationLaneDesc* desc) {
  desc->target_param_id = static_cast<uint32_t>(IntProperty(obj, "targetParamId", 0));
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
  if (!RequiredUint32Property(env, obj, "id", &desc->id)) return false;
  desc->name = nullptr;
  Napi::Value value = obj.Get("anchors");
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "warp map anchors must be an array").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array input = value.As<Napi::Array>();
  anchors->reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value entry = input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "warp map anchor must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object anchor = entry.As<Napi::Object>();
    SonareProjectWarpAnchor out{};
    if (!RequiredDoubleProperty(env, anchor, "warpSample", &out.warp_sample)) return false;
    if (!RequiredDoubleProperty(env, anchor, "sourceSample", &out.source_sample)) return false;
    anchors->push_back(out);
  }
  desc->anchors = anchors->empty() ? nullptr : anchors->data();
  desc->anchor_count = anchors->size();
  return true;
}

}  // namespace

Napi::Value ProjectWrap::AddTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectTrackDesc desc{};
  std::string name;
  bool has_name = false;
  if (info.Length() > 0 && info[0].IsObject()) {
    Napi::Object obj = info[0].As<Napi::Object>();
    desc.kind = IntProperty(obj, "kind", SONARE_TRACK_AUDIO);
    Napi::Value name_value = obj.Get("name");
    if (!name_value.IsUndefined() && !name_value.IsNull()) {
      name = name_value.As<Napi::String>().Utf8Value();
      has_name = true;
    }
  } else {
    desc.kind = static_cast<int>(Uint32Arg(info, 0, SONARE_TRACK_AUDIO));
  }
  desc.name = has_name ? name.c_str() : nullptr;
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_add_track(project_, &desc, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::AddClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "addClip expects a clip descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectClipDesc desc{};
  desc.track_id = static_cast<uint32_t>(IntProperty(obj, "trackId", 0));
  desc.is_midi = obj.Get("isMidi").ToBoolean().Value() ? 1 : 0;
  desc.start_ppq = obj.Get("startPpq").IsUndefined()
                       ? 0.0
                       : obj.Get("startPpq").As<Napi::Number>().DoubleValue();
  desc.length_ppq = obj.Get("lengthPpq").IsUndefined()
                        ? 0.0
                        : obj.Get("lengthPpq").As<Napi::Number>().DoubleValue();
  desc.source_offset_ppq = obj.Get("sourceOffsetPpq").IsUndefined()
                               ? 0.0
                               : obj.Get("sourceOffsetPpq").As<Napi::Number>().DoubleValue();
  desc.gain =
      obj.Get("gain").IsUndefined() ? 1.0f : obj.Get("gain").As<Napi::Number>().FloatValue();
  desc.audio_channels = IntProperty(obj, "audioChannels", 0);
  desc.audio_sample_rate = IntProperty(obj, "audioSampleRate", 0);

  // Keep the interleaved samples alive (as a copy) for the duration of the call.
  std::vector<float> audio;
  Napi::Value audio_value = obj.Get("audio");
  if (sonare_node::IsFloat32Array(audio_value)) {
    if (desc.audio_channels == 0) desc.audio_channels = 1;
    Napi::Float32Array array = audio_value.As<Napi::Float32Array>();
    if (desc.audio_channels <= 0 ||
        array.ElementLength() % static_cast<size_t>(desc.audio_channels) != 0) {
      Napi::TypeError::New(env, "audio length must be a multiple of audioChannels")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    audio.assign(array.Data(), array.Data() + array.ElementLength());
    desc.audio_interleaved = audio.data();
    desc.audio_frames = static_cast<int64_t>(audio.size()) / desc.audio_channels;
  }

  std::string source_uri;
  Napi::Value uri_value = obj.Get("sourceUri");
  if (!uri_value.IsUndefined() && !uri_value.IsNull()) {
    source_uri = uri_value.As<Napi::String>().Utf8Value();
    desc.source_uri = source_uri.c_str();
  }

  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_add_clip(project_, &desc, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::AddLoopRecordingTakes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "addLoopRecordingTakes expects a descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectLoopRecordingDesc desc{};
  desc.track_id = static_cast<uint32_t>(IntProperty(obj, "trackId", 0));
  desc.start_ppq = obj.Get("startPpq").IsUndefined()
                       ? 0.0
                       : obj.Get("startPpq").As<Napi::Number>().DoubleValue();
  desc.loop_length_ppq = obj.Get("loopLengthPpq").IsUndefined()
                             ? 0.0
                             : obj.Get("loopLengthPpq").As<Napi::Number>().DoubleValue();
  desc.audio_channels = IntProperty(obj, "audioChannels", 1);
  desc.audio_sample_rate = IntProperty(obj, "audioSampleRate", 48000);

  std::vector<float> audio;
  Napi::Value audio_value = obj.Get("audio");
  if (sonare_node::IsFloat32Array(audio_value)) {
    Napi::Float32Array array = audio_value.As<Napi::Float32Array>();
    if (desc.audio_channels <= 0 ||
        array.ElementLength() % static_cast<size_t>(desc.audio_channels) != 0) {
      Napi::TypeError::New(env, "audio length must be a multiple of audioChannels")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    audio.assign(array.Data(), array.Data() + array.ElementLength());
    desc.audio_interleaved = audio.data();
    desc.audio_frames = static_cast<int64_t>(audio.size()) / desc.audio_channels;
  }

  uint32_t out_clip_id = 0;
  size_t out_take_count = 0;
  ThrowIfError(
      env, sonare_project_add_loop_recording_takes(project_, &desc, &out_clip_id, &out_take_count));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, out_clip_id));
  out.Set("takeCount", Napi::Number::New(env, static_cast<double>(out_take_count)));
  return out;
}

Napi::Value ProjectWrap::AddMidiClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint32_t out_track = 0;
  uint32_t out_clip = 0;
  ThrowIfError(env, sonare_project_add_midi_clip(project_, NumberArg(info, 0, 0.0),
                                                 NumberArg(info, 1, 0.0), &out_track, &out_clip));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("trackId", Napi::Number::New(env, out_track));
  out.Set("clipId", Napi::Number::New(env, out_clip));
  return out;
}

Napi::Value ProjectWrap::SplitClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_split_clip(project_, Uint32Arg(info, 0, 0),
                                              NumberArg(info, 1, 0.0), &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::TrimClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_trim_clip(project_, Uint32Arg(info, 0, 0),
                                             NumberArg(info, 1, 0.0), NumberArg(info, 2, 0.0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::MoveClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_move_clip(project_, Uint32Arg(info, 0, 0),
                                             NumberArg(info, 1, 0.0), Uint32Arg(info, 2, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackKind(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(
      env, sonare_project_set_track_kind(project_, Uint32Arg(info, 0, 0), Uint32Arg(info, 1, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipWarpRef(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_clip_warp_ref(project_, Uint32Arg(info, 0, 0),
                                                     Uint32Arg(info, 1, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipWarpMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_clip_warp_mode(
                        project_, Uint32Arg(info, 0, 0),
                        static_cast<SonareProjectWarpMode>(Uint32Arg(info, 1, 0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetWarpMap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected warp map object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareProjectWarpAnchor> anchors;
  SonareProjectWarpMapDesc desc{};
  std::string name_storage;
  Napi::Object obj = info[0].As<Napi::Object>();
  Napi::Value name = obj.Get("name");
  if (name.IsString()) name_storage = name.As<Napi::String>().Utf8Value();
  if (!FillWarpMapDesc(env, obj, &anchors, &desc)) return env.Undefined();
  desc.name = name_storage.empty() ? nullptr : name_storage.c_str();
  ThrowIfError(env, sonare_project_set_warp_map(project_, &desc));
  return env.Undefined();
}

Napi::Value ProjectWrap::RemoveWarpMap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_remove_warp_map(project_, Uint32Arg(info, 0, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackMidiDestination(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_track_midi_destination(project_, Uint32Arg(info, 0, 0),
                                                              Uint32Arg(info, 1, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_track_gain(project_, Uint32Arg(info, 0, 0),
                                                  static_cast<float>(NumberArg(info, 1, 1.0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackMute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int mute = info.Length() > 1 && info[1].ToBoolean().Value() ? 1 : 0;
  ThrowIfError(env, sonare_project_set_track_mute(project_, Uint32Arg(info, 0, 0), mute));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackSolo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int solo = info.Length() > 1 && info[1].ToBoolean().Value() ? 1 : 0;
  ThrowIfError(env, sonare_project_set_track_solo(project_, Uint32Arg(info, 0, 0), solo));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_track_pan(project_, Uint32Arg(info, 0, 0),
                                                 static_cast<float>(NumberArg(info, 1, 0.0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::RemoveClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_remove_clip(project_, Uint32Arg(info, 0, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_clip_gain(project_, Uint32Arg(info, 0, 0),
                                                 static_cast<float>(NumberArg(info, 1, 1.0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipFade(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
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
}

Napi::Value ProjectWrap::SetClipTakes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  std::vector<SonareProjectClipTake> takes;
  std::vector<std::string> name_storage;
  if (!ParseClipTakes(env, info.Length() > 1 ? info[1] : env.Undefined(), &takes, &name_storage)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_project_set_clip_takes(project_, clip_id, takes.empty() ? nullptr : takes.data(),
                                         takes.size(), Uint32Arg(info, 2, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipCompSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  std::vector<SonareProjectClipCompSegment> segments;
  if (!ParseClipCompSegments(env, info.Length() > 1 ? info[1] : env.Undefined(), &segments)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_project_set_clip_comp_segments(
               project_, clip_id, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_clip_loop(project_, Uint32Arg(info, 0, 0),
                                                 static_cast<int>(NumberArg(info, 1, 0.0)),
                                                 NumberArg(info, 2, 0.0), NumberArg(info, 3, 0.0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(
      env, sonare_project_set_clip_source(project_, Uint32Arg(info, 0, 0), Uint32Arg(info, 1, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetAudioSourceMetadata(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsString() || !info[2].IsString()) {
    Napi::TypeError::New(env,
                         "setAudioSourceMetadata expects (sourceId, contentHash, externalStemRole)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint32_t source_id = Uint32Arg(info, 0, 0);
  const std::string content_hash = info[1].As<Napi::String>().Utf8Value();
  const std::string external_stem_role = info[2].As<Napi::String>().Utf8Value();
  ThrowIfError(env, sonare_project_set_audio_source_metadata(
                        project_, source_id, content_hash.c_str(), external_stem_role.c_str()));
  return env.Undefined();
}

Napi::Value ProjectWrap::DuplicateClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_duplicate_clip(project_, Uint32Arg(info, 0, 0),
                                                  NumberArg(info, 1, 0.0), &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::RemoveTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_remove_track(project_, Uint32Arg(info, 0, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::RenameTrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = Uint32Arg(info, 0, 0);
  std::string name;
  const char* name_ptr = nullptr;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull()) {
    name = info[1].As<Napi::String>().Utf8Value();
    name_ptr = name.c_str();
  }
  ThrowIfError(env, sonare_project_rename_track(project_, track_id, name_ptr));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTrackRoute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = Uint32Arg(info, 0, 0);
  std::string strip;
  std::string output;
  const char* strip_ptr = nullptr;
  const char* output_ptr = nullptr;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull()) {
    strip = info[1].As<Napi::String>().Utf8Value();
    strip_ptr = strip.c_str();
  }
  if (info.Length() > 2 && !info[2].IsUndefined() && !info[2].IsNull()) {
    output = info[2].As<Napi::String>().Utf8Value();
    output_ptr = output.c_str();
  }
  ThrowIfError(env, sonare_project_set_track_route(project_, track_id, strip_ptr, output_ptr));
  return env.Undefined();
}

Napi::Value ProjectWrap::AddAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = Uint32Arg(info, 0, 0);
  if (info.Length() < 2 || !info[1].IsObject()) {
    Napi::TypeError::New(env, "addAutomationLane expects a lane descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareAutomationPoint> points;
  SonareAutomationLaneDesc desc{};
  const Napi::Object input = info[1].As<Napi::Object>();
  if (!FillAutomationLaneDesc(env, input, &points, &desc)) {
    return env.Undefined();  // exception already pending
  }
  if (env.IsExceptionPending()) return env.Undefined();
  bool has_target_kind = false;
  uint32_t target_kind = SONARE_AUTOMATION_TARGET_OPAQUE;
  if (!ReadAutomationTargetKind(env, input, &has_target_kind, &target_kind)) {
    return env.Undefined();  // exception already pending
  }
  uint32_t out_target_param_id = 0;
  if (!has_target_kind) {
    ThrowIfError(
        env, sonare_project_add_automation_lane(project_, track_id, &desc, &out_target_param_id));
  } else {
    SonareAutomationLaneDescEx desc_ex{};
    desc_ex.target_param_id = desc.target_param_id;
    desc_ex.target_kind = static_cast<SonareAutomationTargetKind>(target_kind);
    desc_ex.points = desc.points;
    desc_ex.point_count = desc.point_count;
    ThrowIfError(env, sonare_project_add_automation_lane_ex(project_, track_id, &desc_ex,
                                                            &out_target_param_id));
  }
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_target_param_id));
}

Napi::Value ProjectWrap::EditAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = Uint32Arg(info, 0, 0);
  const uint32_t target_param_id = Uint32Arg(info, 1, 0);
  if (info.Length() < 3 || !info[2].IsObject()) {
    Napi::TypeError::New(env, "editAutomationLane expects a lane descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareAutomationPoint> points;
  SonareAutomationLaneDesc desc{};
  const Napi::Object input = info[2].As<Napi::Object>();
  if (!FillAutomationLaneDesc(env, input, &points, &desc)) {
    return env.Undefined();  // exception already pending
  }
  if (env.IsExceptionPending()) return env.Undefined();
  bool has_target_kind = false;
  uint32_t target_kind = SONARE_AUTOMATION_TARGET_OPAQUE;
  if (!ReadAutomationTargetKind(env, input, &has_target_kind, &target_kind)) {
    return env.Undefined();  // exception already pending
  }
  if (!has_target_kind) {
    ThrowIfError(env,
                 sonare_project_edit_automation_lane(project_, track_id, target_param_id, &desc));
  } else {
    SonareAutomationLaneDescEx desc_ex{};
    desc_ex.target_param_id = desc.target_param_id;
    desc_ex.target_kind = static_cast<SonareAutomationTargetKind>(target_kind);
    desc_ex.points = desc.points;
    desc_ex.point_count = desc.point_count;
    ThrowIfError(
        env, sonare_project_edit_automation_lane_ex(project_, track_id, target_param_id, &desc_ex));
  }
  return env.Undefined();
}

Napi::Value ProjectWrap::RemoveAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_remove_automation_lane(project_, Uint32Arg(info, 0, 0),
                                                          Uint32Arg(info, 1, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::Undo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_undo(project_));
  return env.Undefined();
}

Napi::Value ProjectWrap::Redo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_redo(project_));
  return env.Undefined();
}

Napi::Value ProjectWrap::ClearHistory(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_clear_history(project_));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetMaxUndoDepth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  // A raw double -> size_t cast is undefined for a negative or NaN depth and
  // lands on SIZE_MAX in practice, which disables the very cap this call
  // exists to install. The core only clamps the lower bound, so the domain
  // check has to happen here, before any native state is touched.
  size_t depth = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "depth", &depth)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_max_undo_depth(project_, depth));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetMaxHistoryBytes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t bytes = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "bytes", &bytes)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_max_history_bytes(project_, bytes));
  return env.Undefined();
}
