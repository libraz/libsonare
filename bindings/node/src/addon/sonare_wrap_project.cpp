#include "sonare_wrap_project.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

Napi::FunctionReference ProjectWrap::constructor;

Napi::Object ProjectWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Project",
      {
          InstanceMethod<&ProjectWrap::ToJson>("toJson"),
          InstanceMethod<&ProjectWrap::SetSampleRate>("setSampleRate"),
          InstanceMethod<&ProjectWrap::AddTrack>("addTrack"),
          InstanceMethod<&ProjectWrap::AddClip>("addClip"),
          InstanceMethod<&ProjectWrap::AddLoopRecordingTakes>("addLoopRecordingTakes"),
          InstanceMethod<&ProjectWrap::AddMidiClip>("addMidiClip"),
          InstanceMethod<&ProjectWrap::SplitClip>("splitClip"),
          InstanceMethod<&ProjectWrap::TrimClip>("trimClip"),
          InstanceMethod<&ProjectWrap::MoveClip>("moveClip"),
          InstanceMethod<&ProjectWrap::SetTrackKind>("setTrackKind"),
          InstanceMethod<&ProjectWrap::SetClipWarpRef>("setClipWarpRef"),
          InstanceMethod<&ProjectWrap::SetClipWarpMode>("setClipWarpMode"),
          InstanceMethod<&ProjectWrap::SetWarpMap>("setWarpMap"),
          InstanceMethod<&ProjectWrap::RemoveWarpMap>("removeWarpMap"),
          InstanceMethod<&ProjectWrap::SetTrackMidiDestination>("setTrackMidiDestination"),
          InstanceMethod<&ProjectWrap::SetTrackGain>("setTrackGain"),
          InstanceMethod<&ProjectWrap::SetTrackMute>("setTrackMute"),
          InstanceMethod<&ProjectWrap::SetTrackSolo>("setTrackSolo"),
          InstanceMethod<&ProjectWrap::SetTrackPan>("setTrackPan"),
          InstanceMethod<&ProjectWrap::RemoveClip>("removeClip"),
          InstanceMethod<&ProjectWrap::SetClipGain>("setClipGain"),
          InstanceMethod<&ProjectWrap::SetClipFade>("setClipFade"),
          InstanceMethod<&ProjectWrap::SetClipTakes>("setClipTakes"),
          InstanceMethod<&ProjectWrap::SetClipCompSegments>("setClipCompSegments"),
          InstanceMethod<&ProjectWrap::SetClipLoop>("setClipLoop"),
          InstanceMethod<&ProjectWrap::SetClipSource>("setClipSource"),
          InstanceMethod<&ProjectWrap::DuplicateClip>("duplicateClip"),
          InstanceMethod<&ProjectWrap::RemoveTrack>("removeTrack"),
          InstanceMethod<&ProjectWrap::RenameTrack>("renameTrack"),
          InstanceMethod<&ProjectWrap::SetTrackRoute>("setTrackRoute"),
          InstanceMethod<&ProjectWrap::AddAutomationLane>("addAutomationLane"),
          InstanceMethod<&ProjectWrap::EditAutomationLane>("editAutomationLane"),
          InstanceMethod<&ProjectWrap::RemoveAutomationLane>("removeAutomationLane"),
          InstanceMethod<&ProjectWrap::Undo>("undo"),
          InstanceMethod<&ProjectWrap::Redo>("redo"),
          InstanceMethod<&ProjectWrap::SetMidiEvents>("setMidiEvents"),
          InstanceMethod<&ProjectWrap::ImportSmf>("importSmf"),
          InstanceMethod<&ProjectWrap::ExportSmf>("exportSmf"),
          InstanceMethod<&ProjectWrap::ImportClipFile>("importClipFile"),
          InstanceMethod<&ProjectWrap::ExportClipFile>("exportClipFile"),
          InstanceMethod<&ProjectWrap::SetProgram>("setProgram"),
          InstanceMethod<&ProjectWrap::SetProgramOnChannel>("setProgramOnChannel"),
          InstanceMethod<&ProjectWrap::BakeMidiFx>("bakeMidiFx"),
          InstanceMethod<&ProjectWrap::SetMidiFx>("setMidiFx"),
          InstanceMethod<&ProjectWrap::ValidateMidiNotes>("validateMidiNotes"),
          InstanceMethod<&ProjectWrap::AutoTempo>("autoTempo"),
          InstanceMethod<&ProjectWrap::AnalyzeTempo>("analyzeTempo"),
          InstanceMethod<&ProjectWrap::SnapToGrid>("snapToGrid"),
          InstanceMethod<&ProjectWrap::AnnotateKeys>("annotateKeys"),
          InstanceMethod<&ProjectWrap::AnnotateChords>("annotateChords"),
          InstanceMethod<&ProjectWrap::SetAssistSidecar>("setAssistSidecar"),
          InstanceMethod<&ProjectWrap::AssistSidecarCount>("assistSidecarCount"),
          InstanceMethod<&ProjectWrap::GetAssistSidecar>("getAssistSidecar"),
          InstanceMethod<&ProjectWrap::AssistSidecars>("assistSidecars"),
          InstanceMethod<&ProjectWrap::Compile>("compile"),
          InstanceMethod<&ProjectWrap::LastBounceCompileResult>("lastBounceCompileResult"),
          InstanceMethod<&ProjectWrap::Bounce>("bounce"),
          InstanceMethod<&ProjectWrap::BounceWithBuiltinInstruments>(
              "bounceWithBuiltinInstruments"),
          InstanceMethod<&ProjectWrap::BounceWithSynthInstruments>("bounceWithSynthInstruments"),
          InstanceMethod<&ProjectWrap::LoadSoundFont>("loadSoundFont"),
          InstanceMethod<&ProjectWrap::ClearSoundFont>("clearSoundFont"),
          InstanceMethod<&ProjectWrap::SoundFontPresetCount>("soundFontPresetCount"),
          InstanceMethod<&ProjectWrap::SoundFontManifest>("soundFontManifest"),
          InstanceMethod<&ProjectWrap::BounceWithSf2Instruments>("bounceWithSf2Instruments"),
          InstanceMethod<&ProjectWrap::GetSampleRate>("getSampleRate"),
          InstanceMethod<&ProjectWrap::SetOverlapPolicy>("setOverlapPolicy"),
          InstanceMethod<&ProjectWrap::GetOverlapPolicy>("getOverlapPolicy"),
          InstanceMethod<&ProjectWrap::SetMixerSceneJson>("setMixerSceneJson"),
          InstanceMethod<&ProjectWrap::SetMarker>("setMarker"),
          InstanceMethod<&ProjectWrap::SetMarkerEx>("setMarkerEx"),
          InstanceMethod<&ProjectWrap::MarkerByIndex>("markerByIndex"),
          InstanceMethod<&ProjectWrap::SetTempoSegments>("setTempoSegments"),
          InstanceMethod<&ProjectWrap::SetTimeSignatures>("setTimeSignatures"),
          InstanceMethod<&ProjectWrap::TrackCount>("trackCount"),
          InstanceMethod<&ProjectWrap::ClipCount>("clipCount"),
          InstanceMethod<&ProjectWrap::SourceCount>("sourceCount"),
          InstanceMethod<&ProjectWrap::TempoSegmentCount>("tempoSegmentCount"),
          InstanceMethod<&ProjectWrap::TimeSignatureCount>("timeSignatureCount"),
          InstanceMethod<&ProjectWrap::MarkerCount>("markerCount"),
          InstanceMethod<&ProjectWrap::Destroy>("destroy"),
          StaticMethod<&ProjectWrap::FromJson>("fromJson"),
          StaticMethod<&ProjectWrap::FromJsonWithDiagnostics>("fromJsonWithDiagnostics"),
      });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("Project", func);
  return exports;
}

Napi::Object ProjectWrap::Wrap(Napi::Env env, SonareProject* handle) {
  // Inject the already-created native handle through an External so the
  // constructor adopts it instead of allocating a fresh empty project.
  Napi::External<SonareProject> external = Napi::External<SonareProject>::New(env, handle);
  return constructor.New({external});
}

ProjectWrap::ProjectWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ProjectWrap>(info) {
  Napi::Env env = info.Env();

  // Adoption path: fromJson() / Wrap() pass an External wrapping an existing
  // SonareProject* that was created (and ABI-checked) by the static factory.
  if (info.Length() > 0 && info[0].IsExternal()) {
    project_ = info[0].As<Napi::External<SonareProject>>().Data();
    return;
  }

  const uint32_t abi_version = sonare_project_abi_version();
  if (abi_version != kExpectedProjectAbiVersion) {
    Napi::Error::New(env, "libsonare project ABI mismatch: native binary reports version " +
                              std::to_string(abi_version) + ", expected " +
                              std::to_string(kExpectedProjectAbiVersion) +
                              " (0 = arrangement support not compiled in). The prebuilt addon is "
                              "incompatible with this binding.")
        .ThrowAsJavaScriptException();
    return;
  }
  ThrowIfError(env, sonare_project_create(&project_));
}

ProjectWrap::~ProjectWrap() {
  if (project_ != nullptr) {
    sonare_project_destroy(project_);
    project_ = nullptr;
  }
}

Napi::Value ProjectWrap::ToJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (project_ == nullptr) {
    Napi::Error::New(env, "Project is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_serialize(project_, &json, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::String out = Napi::String::New(env, json != nullptr ? json : "", len);
  if (json != nullptr) sonare_free_string(json);
  return out;
}

Napi::Value ProjectWrap::FromJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "fromJson expects a JSON string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint32_t abi_version = sonare_project_abi_version();
  if (abi_version != kExpectedProjectAbiVersion) {
    Napi::Error::New(env, "libsonare project ABI mismatch: native binary reports version " +
                              std::to_string(abi_version) + ", expected " +
                              std::to_string(kExpectedProjectAbiVersion) +
                              " (0 = arrangement support not compiled in).")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string json = info[0].As<Napi::String>().Utf8Value();
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
  if (err != SONARE_OK) {
    std::string detail = diag != nullptr ? diag : "";
    if (diag != nullptr) sonare_free_string(diag);
    Napi::Error::New(env, detail.empty() ? "failed to deserialize project JSON" : detail)
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (diag != nullptr) sonare_free_string(diag);
  return ProjectWrap::Wrap(env, handle);
}

Napi::Value ProjectWrap::FromJsonWithDiagnostics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "fromJsonWithDiagnostics expects a JSON string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint32_t abi_version = sonare_project_abi_version();
  if (abi_version != kExpectedProjectAbiVersion) {
    Napi::Error::New(env, "libsonare project ABI mismatch: native binary reports version " +
                              std::to_string(abi_version) + ", expected " +
                              std::to_string(kExpectedProjectAbiVersion) +
                              " (0 = arrangement support not compiled in).")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string json = info[0].As<Napi::String>().Utf8Value();
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
  std::string diagnostics = diag != nullptr ? diag : "";
  if (diag != nullptr) sonare_free_string(diag);
  if (err != SONARE_OK) {
    sonare_project_destroy(handle);
    Napi::Error::New(env, diagnostics.empty() ? "failed to deserialize project JSON" : diagnostics)
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("project", ProjectWrap::Wrap(env, handle));
  out.Set("diagnostics", diagnostics);
  return out;
}

Napi::Value ProjectWrap::SetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_sample_rate(project_, NumberArg(info, 0, 0.0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::GetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  double out = 0.0;
  ThrowIfError(env, sonare_project_get_sample_rate(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
}

Napi::Value ProjectWrap::SetOverlapPolicy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_overlap_policy(project_, Uint32Arg(info, 0, 0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::GetOverlapPolicy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint32_t out = 0;
  ThrowIfError(env, sonare_project_get_overlap_policy(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
}

Napi::Value ProjectWrap::SetMixerSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string scene = info.Length() > 0 && info[0].IsString()
                          ? info[0].As<Napi::String>().Utf8Value()
                          : std::string();
  ThrowIfError(env, sonare_project_set_mixer_scene_json(project_, scene.c_str()));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetMarker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t marker_id = Uint32Arg(info, 0, 0);
  const double ppq = NumberArg(info, 1, 0.0);
  std::string name = info.Length() > 2 && info[2].IsString()
                         ? info[2].As<Napi::String>().Utf8Value()
                         : std::string();
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_set_marker(project_, marker_id, ppq, name.c_str(), &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::SetMarkerEx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a marker object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectMarker marker{};
  marker.id = static_cast<uint32_t>(IntProperty(obj, "id", 0));
  marker.kind = static_cast<uint8_t>(IntProperty(obj, "kind", SONARE_MARKER_KIND_MARKER));
  marker.key_fifths = static_cast<int8_t>(IntProperty(obj, "keyFifths", 0));
  marker.key_minor = BoolProperty(obj, "keyMinor", false) ? 1 : 0;
  marker.ppq = DoubleProperty(obj, "ppq", 0.0);
  const std::string name = obj.Has("name") && !obj.Get("name").IsUndefined()
                               ? obj.Get("name").As<Napi::String>().Utf8Value()
                               : std::string();
  std::strncpy(marker.name, name.c_str(), sizeof(marker.name) - 1);
  marker.name[sizeof(marker.name) - 1] = '\0';
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_set_marker_ex(project_, &marker, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::MarkerByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t index = static_cast<size_t>(NumberArg(info, 0, 0.0));
  SonareProjectMarker marker{};
  ThrowIfError(env, sonare_project_marker_by_index(project_, index, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, marker.id));
  out.Set("ppq", Napi::Number::New(env, marker.ppq));
  out.Set("name", Napi::String::New(env, marker.name));
  out.Set("kind", Napi::Number::New(env, marker.kind));
  out.Set("keyFifths", Napi::Number::New(env, marker.key_fifths));
  out.Set("keyMinor", Napi::Boolean::New(env, marker.key_minor != 0));
  return out;
}

Napi::Value ProjectWrap::SetTempoSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareProjectTempoSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    segments.reserve(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "tempo segment must be an object").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectTempoSegment seg{};
      seg.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
      seg.bpm = obj.Get("bpm").As<Napi::Number>().DoubleValue();
      Napi::Value end_bpm = obj.Get("endBpm");
      seg.end_bpm = end_bpm.IsUndefined() ? 0.0 : end_bpm.As<Napi::Number>().DoubleValue();
      segments.push_back(seg);
    }
  } else if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "setTempoSegments expects an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_tempo_segments(
                        project_, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetTimeSignatures(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareProjectTimeSignatureSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    segments.reserve(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "time-signature segment must be an object")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectTimeSignatureSegment seg{};
      seg.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
      seg.numerator = obj.Get("numerator").As<Napi::Number>().Int32Value();
      seg.denominator = obj.Get("denominator").As<Napi::Number>().Int32Value();
      segments.push_back(seg);
    }
  } else if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "setTimeSignatures expects an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_time_signatures(
                        project_, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::TrackCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_track_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_clip_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::SourceCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_source_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::TempoSegmentCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_tempo_segment_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::TimeSignatureCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_time_signature_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::MarkerCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_marker_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

void ProjectWrap::Destroy(const Napi::CallbackInfo& info) {
  (void)info;
  if (project_ != nullptr) {
    sonare_project_destroy(project_);
    project_ = nullptr;
  }
}
