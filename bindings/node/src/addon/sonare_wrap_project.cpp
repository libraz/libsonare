#include "sonare_wrap_project.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap_utils.h"

namespace {

// Derived from the canonical C macro (via sonare_c.h) so this addon can never
// drift from SONARE_PROJECT_ABI_VERSION. A runtime mismatch means the loaded
// native binary lays out the flat project PODs differently than this addon
// expects (or arrangement support was compiled out -> runtime version 0).
constexpr uint32_t kExpectedProjectAbiVersion = SONARE_PROJECT_ABI_VERSION;

void ThrowIfError(Napi::Env env, SonareError err) {
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_node::ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  }
}

double NumberArg(const Napi::CallbackInfo& info, size_t index, double fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return info[index].As<Napi::Number>().DoubleValue();
}

uint32_t Uint32Arg(const Napi::CallbackInfo& info, size_t index, uint32_t fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return info[index].As<Napi::Number>().Uint32Value();
}

int IntProperty(const Napi::Object& obj, const char* key, int fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().Int32Value();
}

int64_t Int64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback
                             : static_cast<int64_t>(value.As<Napi::Number>().Int64Value());
}

float FloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().FloatValue();
}

// Fills `options` from a JS bounce-options object (zero-initialized on entry).
void FillBounceOptions(const Napi::Object& obj, SonareProjectBounceOptions* options) {
  options->total_frames = Int64Property(obj, "totalFrames", 0);
  options->block_size = IntProperty(obj, "blockSize", 0);
  options->num_channels = IntProperty(obj, "numChannels", 0);
  options->sample_rate = IntProperty(obj, "sampleRate", 0);
  options->instrument_latency_samples = IntProperty(obj, "instrumentLatencySamples", 0);
}

// Maps a waveform name to its SonareSynthWaveform enum value, or -1 if unknown.
int WaveformFromName(const std::string& name) {
  if (name == "sine") return SONARE_SYNTH_WAVEFORM_SINE;
  if (name == "saw" || name == "sawtooth") return SONARE_SYNTH_WAVEFORM_SAW;
  if (name == "square") return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (name == "triangle") return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

// Parses a JS instrument descriptor into a built-in synth binding. Throws a
// JS exception (and returns false) on an unknown waveform name. A zero-init
// config is the native default sine patch, so only present fields are set.
bool ParseBuiltinInstrument(Napi::Env env, const Napi::Object& obj,
                            SonareBuiltinInstrumentBinding* binding) {
  binding->destination_id = obj.Get("destinationId").IsUndefined()
                                ? 0u
                                : obj.Get("destinationId").As<Napi::Number>().Uint32Value();
  SonareBuiltinSynthConfig& config = binding->config;
  Napi::Value waveform = obj.Get("waveform");
  if (waveform.IsString()) {
    const std::string name = waveform.As<Napi::String>().Utf8Value();
    const int mapped = WaveformFromName(name);
    if (mapped < 0) {
      Napi::TypeError::New(env, "Unknown synth waveform name: '" + name +
                                    "' (expected sine, saw, square, or triangle)")
          .ThrowAsJavaScriptException();
      return false;
    }
    config.waveform = mapped;
  } else if (waveform.IsNumber()) {
    config.waveform = waveform.As<Napi::Number>().Int32Value();
  }
  config.gain = FloatProperty(obj, "gain", 0.0f);
  config.attack_ms = FloatProperty(obj, "attackMs", 0.0f);
  config.decay_ms = FloatProperty(obj, "decayMs", 0.0f);
  config.sustain = FloatProperty(obj, "sustain", 0.0f);
  config.release_ms = FloatProperty(obj, "releaseMs", 0.0f);
  config.polyphony = IntProperty(obj, "polyphony", 0);
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
    point.ppq = obj.Get("ppq").As<Napi::Number>().DoubleValue();
    point.value = obj.Get("value").As<Napi::Number>().FloatValue();
    Napi::Value curve = obj.Get("curve");
    if (curve.IsUndefined()) curve = obj.Get("curveToNext");
    point.curve_to_next = curve.IsUndefined() ? 0 : curve.As<Napi::Number>().Int32Value();
    points->push_back(point);
  }
  return true;
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
  desc->id = obj.Get("id").As<Napi::Number>().Uint32Value();
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
    out.warp_sample = anchor.Get("warpSample").As<Napi::Number>().DoubleValue();
    out.source_sample = anchor.Get("sourceSample").As<Napi::Number>().DoubleValue();
    anchors->push_back(out);
  }
  desc->anchors = anchors->empty() ? nullptr : anchors->data();
  desc->anchor_count = anchors->size();
  return true;
}

}  // namespace

Napi::FunctionReference ProjectWrap::constructor;

Napi::Object ProjectWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Project",
      {
          InstanceMethod<&ProjectWrap::ToJson>("toJson"),
          InstanceMethod<&ProjectWrap::SetSampleRate>("setSampleRate"),
          InstanceMethod<&ProjectWrap::AddTrack>("addTrack"),
          InstanceMethod<&ProjectWrap::AddClip>("addClip"),
          InstanceMethod<&ProjectWrap::AddMidiClip>("addMidiClip"),
          InstanceMethod<&ProjectWrap::SplitClip>("splitClip"),
          InstanceMethod<&ProjectWrap::TrimClip>("trimClip"),
          InstanceMethod<&ProjectWrap::MoveClip>("moveClip"),
          InstanceMethod<&ProjectWrap::SetTrackKind>("setTrackKind"),
          InstanceMethod<&ProjectWrap::SetClipWarpRef>("setClipWarpRef"),
          InstanceMethod<&ProjectWrap::SetWarpMap>("setWarpMap"),
          InstanceMethod<&ProjectWrap::RemoveWarpMap>("removeWarpMap"),
          InstanceMethod<&ProjectWrap::SetTrackMidiDestination>("setTrackMidiDestination"),
          InstanceMethod<&ProjectWrap::RemoveClip>("removeClip"),
          InstanceMethod<&ProjectWrap::SetClipGain>("setClipGain"),
          InstanceMethod<&ProjectWrap::SetClipFade>("setClipFade"),
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
          InstanceMethod<&ProjectWrap::GetSampleRate>("getSampleRate"),
          InstanceMethod<&ProjectWrap::SetOverlapPolicy>("setOverlapPolicy"),
          InstanceMethod<&ProjectWrap::GetOverlapPolicy>("getOverlapPolicy"),
          InstanceMethod<&ProjectWrap::SetMixerSceneJson>("setMixerSceneJson"),
          InstanceMethod<&ProjectWrap::SetMarker>("setMarker"),
          InstanceMethod<&ProjectWrap::SetTempoSegments>("setTempoSegments"),
          InstanceMethod<&ProjectWrap::SetTimeSignatures>("setTimeSignatures"),
          InstanceMethod<&ProjectWrap::TrackCount>("trackCount"),
          InstanceMethod<&ProjectWrap::SourceCount>("sourceCount"),
          InstanceMethod<&ProjectWrap::TempoSegmentCount>("tempoSegmentCount"),
          InstanceMethod<&ProjectWrap::TimeSignatureCount>("timeSignatureCount"),
          InstanceMethod<&ProjectWrap::Destroy>("destroy"),
          StaticMethod<&ProjectWrap::FromJson>("fromJson"),
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

Napi::Value ProjectWrap::SetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_sample_rate(project_, NumberArg(info, 0, 0.0)));
  return env.Undefined();
}

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
  desc.audio_channels = IntProperty(obj, "audioChannels", 1);
  desc.audio_sample_rate = IntProperty(obj, "audioSampleRate", 0);

  // Keep the interleaved samples alive (as a copy) for the duration of the call.
  std::vector<float> audio;
  Napi::Value audio_value = obj.Get("audio");
  if (sonare_node::IsFloat32Array(audio_value)) {
    Napi::Float32Array array = audio_value.As<Napi::Float32Array>();
    audio.assign(array.Data(), array.Data() + array.ElementLength());
    desc.audio_interleaved = audio.data();
    const int channels = desc.audio_channels > 0 ? desc.audio_channels : 1;
    desc.audio_frames = static_cast<int64_t>(audio.size()) / channels;
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
  // fadeIn / fadeOut are optional {lengthPpq, curve} objects; NULL = unchanged.
  SonareProjectClipFade fade_in{};
  SonareProjectClipFade fade_out{};
  const SonareProjectClipFade* fade_in_ptr = nullptr;
  const SonareProjectClipFade* fade_out_ptr = nullptr;
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    fade_in.length_ppq = obj.Get("lengthPpq").As<Napi::Number>().DoubleValue();
    fade_in.curve = static_cast<uint32_t>(IntProperty(obj, "curve", SONARE_FADE_CURVE_LINEAR));
    fade_in_ptr = &fade_in;
  }
  if (info.Length() > 2 && info[2].IsObject()) {
    Napi::Object obj = info[2].As<Napi::Object>();
    fade_out.length_ppq = obj.Get("lengthPpq").As<Napi::Number>().DoubleValue();
    fade_out.curve = static_cast<uint32_t>(IntProperty(obj, "curve", SONARE_FADE_CURVE_LINEAR));
    fade_out_ptr = &fade_out;
  }
  ThrowIfError(env, sonare_project_set_clip_fade(project_, clip_id, fade_in_ptr, fade_out_ptr));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_clip_loop(project_, Uint32Arg(info, 0, 0),
                                                 static_cast<int>(NumberArg(info, 1, 0.0)),
                                                 NumberArg(info, 2, 0.0)));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetClipSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(
      env, sonare_project_set_clip_source(project_, Uint32Arg(info, 0, 0), Uint32Arg(info, 1, 0)));
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
  if (!FillAutomationLaneDesc(env, info[1].As<Napi::Object>(), &points, &desc)) {
    return env.Undefined();  // exception already pending
  }
  size_t out_index = 0;
  ThrowIfError(env, sonare_project_add_automation_lane(project_, track_id, &desc, &out_index));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_index));
}

Napi::Value ProjectWrap::EditAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = Uint32Arg(info, 0, 0);
  const size_t lane_index = static_cast<size_t>(NumberArg(info, 1, 0.0));
  if (info.Length() < 3 || !info[2].IsObject()) {
    Napi::TypeError::New(env, "editAutomationLane expects a lane descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareAutomationPoint> points;
  SonareAutomationLaneDesc desc{};
  if (!FillAutomationLaneDesc(env, info[2].As<Napi::Object>(), &points, &desc)) {
    return env.Undefined();  // exception already pending
  }
  ThrowIfError(env, sonare_project_edit_automation_lane(project_, track_id, lane_index, &desc));
  return env.Undefined();
}

Napi::Value ProjectWrap::RemoveAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env,
               sonare_project_remove_automation_lane(project_, Uint32Arg(info, 0, 0),
                                                     static_cast<size_t>(NumberArg(info, 1, 0.0))));
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

Napi::Value ProjectWrap::SetMidiEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  std::vector<SonareMidiEventPod> events;
  if (info.Length() > 1 && info[1].IsArray()) {
    Napi::Array input = info[1].As<Napi::Array>();
    events.reserve(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      SonareMidiEventPod ev{};
      if (entry.IsArray()) {
        Napi::Array tuple = entry.As<Napi::Array>();
        ev.ppq = tuple.Get(0u).As<Napi::Number>().DoubleValue();
        ev.data0 = tuple.Get(1u).As<Napi::Number>().Uint32Value();
        ev.data1 = tuple.Get(2u).As<Napi::Number>().Uint32Value();
      } else if (entry.IsObject()) {
        Napi::Object obj = entry.As<Napi::Object>();
        ev.ppq = obj.Get("ppq").As<Napi::Number>().DoubleValue();
        ev.data0 = obj.Get("data0").As<Napi::Number>().Uint32Value();
        ev.data1 =
            obj.Get("data1").IsUndefined() ? 0u : obj.Get("data1").As<Napi::Number>().Uint32Value();
      } else {
        Napi::TypeError::New(env, "MIDI event must be a [ppq, data0, data1] tuple or object")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      events.push_back(ev);
    }
  }
  ThrowIfError(
      env, sonare_project_set_midi_events(project_, clip_id,
                                          events.empty() ? nullptr : events.data(), events.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::ImportSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 0 && sonare_node::IsUint8Array(info[0])) {
    Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "importSmf expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_import_smf(project_, bytes, len, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::ExportSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint8_t* bytes = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_export_smf(project_, &bytes, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Buffer<uint8_t> out = Napi::Buffer<uint8_t>::Copy(env, bytes != nullptr ? bytes : nullptr,
                                                          bytes != nullptr ? len : 0);
  if (bytes != nullptr) sonare_free_bytes(bytes);
  return out;
}

Napi::Value ProjectWrap::ImportClipFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 0 && sonare_node::IsUint8Array(info[0])) {
    Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "importClipFile expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_import_clip_file(project_, bytes, len, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
}

Napi::Value ProjectWrap::ExportClipFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  uint8_t* bytes = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_export_clip_file(project_, &bytes, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Buffer<uint8_t> out = Napi::Buffer<uint8_t>::Copy(env, bytes != nullptr ? bytes : nullptr,
                                                          bytes != nullptr ? len : 0);
  if (bytes != nullptr) sonare_free_bytes(bytes);
  return out;
}

Napi::Value ProjectWrap::SetProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_project_set_program(project_, Uint32Arg(info, 0, 0),
                                               static_cast<int>(NumberArg(info, 1, 0.0)),
                                               static_cast<int>(NumberArg(info, 2, 0.0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetProgramOnChannel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env,
               sonare_project_set_program_on_channel(project_, Uint32Arg(info, 0, 0),
                                                     static_cast<uint8_t>(Uint32Arg(info, 1, 0)),
                                                     static_cast<uint8_t>(Uint32Arg(info, 2, 0)),
                                                     static_cast<int>(NumberArg(info, 3, 0.0)),
                                                     static_cast<int>(NumberArg(info, 4, -1.0))));
  return env.Undefined();
}

Napi::Value ProjectWrap::BakeMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  ThrowIfError(env, sonare_project_bake_midi_fx(project_, clip_id, config.c_str()));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetMidiFx(const Napi::CallbackInfo& info) { return BakeMidiFx(info); }

Napi::Value ProjectWrap::ValidateMidiNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  SonareNotePairValidation out{};
  ThrowIfError(env, sonare_project_validate_midi_notes(project_, clip_id, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, out.ok != 0));
  result.Set("unmatchedNoteOns", Napi::Number::New(env, out.unmatched_note_ons));
  result.Set("unmatchedNoteOffs", Napi::Number::New(env, out.unmatched_note_offs));
  return result;
}

Napi::Value ProjectWrap::AutoTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "autoTempo expects a Float32Array of mono audio")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  const int sample_rate = static_cast<int>(NumberArg(info, 1, 0.0));
  float out_bpm = 0.0f;
  ThrowIfError(env, sonare_project_auto_tempo(project_, audio.Data(), audio.ElementLength(),
                                              sample_rate, &out_bpm));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_bpm);
}

Napi::Value ProjectWrap::SnapToGrid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  double out_ppq = 0.0;
  ThrowIfError(env, sonare_project_snap_to_grid(project_, NumberArg(info, 0, 0.0),
                                                NumberArg(info, 1, 1.0), &out_ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_ppq);
}

Napi::Value ProjectWrap::AnnotateKeys(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareProjectKeySegment> keys;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    keys.reserve(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "key segment must be an object").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectKeySegment seg{};
      seg.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
      seg.end_ppq = obj.Get("endPpq").As<Napi::Number>().DoubleValue();
      seg.tonic_pc = static_cast<uint32_t>(IntProperty(obj, "tonicPc", 255));
      seg.mode = static_cast<uint32_t>(IntProperty(obj, "mode", 0));
      keys.push_back(seg);
    }
  }
  ThrowIfError(env, sonare_project_annotate_keys(project_, keys.empty() ? nullptr : keys.data(),
                                                 keys.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::AnnotateChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareProjectChordSymbol> chords;
  // The extension byte arrays and roman-numeral strings must outlive the C call;
  // keep them in side buffers parallel to `chords` (pointers patched after fill).
  std::vector<std::vector<uint8_t>> extensions;
  std::vector<std::string> roman;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    chords.reserve(input.Length());
    extensions.resize(input.Length());
    roman.resize(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "chord symbol must be an object").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectChordSymbol chord{};
      chord.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
      chord.end_ppq = obj.Get("endPpq").As<Napi::Number>().DoubleValue();
      chord.root_pc = static_cast<uint32_t>(IntProperty(obj, "rootPc", 255));
      chord.quality = static_cast<uint32_t>(IntProperty(obj, "quality", 0));
      Napi::Value ext = obj.Get("extensions");
      if (ext.IsArray()) {
        Napi::Array arr = ext.As<Napi::Array>();
        extensions[i].reserve(arr.Length());
        for (uint32_t j = 0; j < arr.Length(); ++j) {
          extensions[i].push_back(
              static_cast<uint8_t>(arr.Get(j).As<Napi::Number>().Uint32Value()));
        }
      }
      chord.extensions = extensions[i].empty() ? nullptr : extensions[i].data();
      chord.extension_count = extensions[i].size();
      chord.slash_bass_pc = static_cast<uint32_t>(IntProperty(obj, "slashBassPc", 255));
      Napi::Value rn = obj.Get("romanNumeral");
      if (!rn.IsUndefined() && !rn.IsNull()) {
        roman[i] = rn.As<Napi::String>().Utf8Value();
        chord.roman_numeral = roman[i].c_str();
      }
      Napi::Value mod = obj.Get("modulationBoundary");
      chord.modulation_boundary = (!mod.IsUndefined() && mod.ToBoolean().Value()) ? 1 : 0;
      chords.push_back(chord);
    }
  }
  ThrowIfError(env, sonare_project_annotate_chords(
                        project_, chords.empty() ? nullptr : chords.data(), chords.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::SetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setAssistSidecar expects a sidecar descriptor object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  Napi::Value module_value = obj.Get("moduleId");
  if (!module_value.IsString()) {
    Napi::TypeError::New(env, "setAssistSidecar: moduleId must be a string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string module_id = module_value.As<Napi::String>().Utf8Value();
  const uint32_t schema_version = static_cast<uint32_t>(IntProperty(obj, "schemaVersion", 0));
  const uint32_t target_track_id = static_cast<uint32_t>(IntProperty(obj, "targetTrackId", 0));
  Napi::Value start_value = obj.Get("regionStartPpq");
  Napi::Value end_value = obj.Get("regionEndPpq");
  const double region_start_ppq =
      start_value.IsUndefined() ? 0.0 : start_value.As<Napi::Number>().DoubleValue();
  const double region_end_ppq =
      end_value.IsUndefined() ? 0.0 : end_value.As<Napi::Number>().DoubleValue();
  std::vector<uint8_t> payload;
  Napi::Value payload_value = obj.Get("payload");
  if (sonare_node::IsUint8Array(payload_value)) {
    Napi::Uint8Array arr = payload_value.As<Napi::Uint8Array>();
    payload.assign(arr.Data(), arr.Data() + arr.ByteLength());
  } else if (payload_value.IsBuffer()) {
    Napi::Buffer<uint8_t> buf = payload_value.As<Napi::Buffer<uint8_t>>();
    payload.assign(buf.Data(), buf.Data() + buf.Length());
  }
  ThrowIfError(env,
               sonare_project_set_assist_sidecar(
                   project_, module_id.c_str(), schema_version, target_track_id, region_start_ppq,
                   region_end_ppq, payload.empty() ? nullptr : payload.data(), payload.size()));
  return env.Undefined();
}

Napi::Value ProjectWrap::AssistSidecarCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Number::New(env, static_cast<double>(sonare_project_assist_sidecar_count(project_)));
}

namespace {

// Marshals one heap-owned SonareProjectAssistSidecar into a JS object and frees
// its heap fields. The struct is consumed (zeroed) by the C free function.
Napi::Object AssistSidecarToObject(Napi::Env env, SonareProjectAssistSidecar* sidecar) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("moduleId",
          Napi::String::New(env, sidecar->module_id != nullptr ? sidecar->module_id : ""));
  out.Set("schemaVersion", Napi::Number::New(env, sidecar->schema_version));
  out.Set("targetTrackId", Napi::Number::New(env, sidecar->target_track_id));
  out.Set("regionStartPpq", Napi::Number::New(env, sidecar->region_start_ppq));
  out.Set("regionEndPpq", Napi::Number::New(env, sidecar->region_end_ppq));
  Napi::Uint8Array payload = Napi::Uint8Array::New(env, sidecar->payload_len);
  if (sidecar->payload_len > 0 && sidecar->payload != nullptr) {
    std::memcpy(payload.Data(), sidecar->payload, sidecar->payload_len);
  }
  out.Set("payload", payload);
  sonare_project_free_assist_sidecar(sidecar);
  return out;
}

}  // namespace

Napi::Value ProjectWrap::GetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t index = static_cast<size_t>(NumberArg(info, 0, 0.0));
  SonareProjectAssistSidecar sidecar{};
  ThrowIfError(env, sonare_project_get_assist_sidecar(project_, index, &sidecar));
  if (env.IsExceptionPending()) return env.Undefined();
  return AssistSidecarToObject(env, &sidecar);
}

Napi::Value ProjectWrap::AssistSidecars(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t count = sonare_project_assist_sidecar_count(project_);
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    SonareProjectAssistSidecar sidecar{};
    ThrowIfError(env, sonare_project_get_assist_sidecar(project_, i, &sidecar));
    if (env.IsExceptionPending()) return env.Undefined();
    out.Set(static_cast<uint32_t>(i), AssistSidecarToObject(env, &sidecar));
  }
  return out;
}

namespace {

// Marshals a heap-owned SonareProjectCompileResult into the JS compile-result
// object shape and frees its heap fields (consuming the struct).
Napi::Object CompileResultToObject(Napi::Env env, SonareProjectCompileResult* result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("hasTimeline", Napi::Boolean::New(env, result->has_timeline != 0));
  out.Set("messages", Napi::String::New(env, result->messages != nullptr ? result->messages : ""));
  Napi::Array diagnostics = Napi::Array::New(env, result->diagnostic_count);
  for (size_t i = 0; i < result->diagnostic_count; ++i) {
    Napi::Object diag = Napi::Object::New(env);
    diag.Set("code", Napi::Number::New(env, result->diagnostics[i].code));
    diag.Set("severity", Napi::Number::New(env, result->diagnostics[i].severity));
    diag.Set("targetId", Napi::Number::New(env, result->diagnostics[i].target_id));
    diagnostics.Set(static_cast<uint32_t>(i), diag);
  }
  out.Set("diagnostics", diagnostics);
  sonare_project_free_compile_result(result);
  return out;
}

}  // namespace

Napi::Value ProjectWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectCompileResult result{};
  ThrowIfError(env, sonare_project_compile(project_, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  return CompileResultToObject(env, &result);
}

Napi::Value ProjectWrap::LastBounceCompileResult(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectCompileResult result{};
  ThrowIfError(env, sonare_project_last_bounce_compile_result(project_, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  return CompileResultToObject(env, &result);
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
      Napi::Value start_sample = obj.Get("startSample");
      seg.start_sample =
          start_sample.IsUndefined() ? 0.0 : start_sample.As<Napi::Number>().DoubleValue();
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

Napi::Value ProjectWrap::Bounce(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectBounceOptions options{};
  if (info.Length() > 0 && info[0].IsObject()) {
    FillBounceOptions(info[0].As<Napi::Object>(), &options);
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce(project_, &options, &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
}

Napi::Value ProjectWrap::BounceWithBuiltinInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  // Argument order is instrument-first to match the WASM and Python bindings:
  //   bounceWithBuiltinInstruments(instruments, options?)
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
  }
  std::vector<SonareBuiltinInstrumentBinding> bindings;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      if (!element.IsObject()) {
        Napi::TypeError::New(env,
                             "bounceWithBuiltinInstruments: instrument bindings must be objects")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      SonareBuiltinInstrumentBinding binding{};
      if (!ParseBuiltinInstrument(env, element.As<Napi::Object>(), &binding)) {
        return env.Undefined();  // exception already pending
      }
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_builtin_instruments(
                        project_, &options, bindings.empty() ? nullptr : bindings.data(),
                        bindings.size(), &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
}

void ProjectWrap::Destroy(const Napi::CallbackInfo& info) {
  (void)info;
  if (project_ != nullptr) {
    sonare_project_destroy(project_);
    project_ = nullptr;
  }
}
