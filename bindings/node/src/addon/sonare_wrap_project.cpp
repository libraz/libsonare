#include "sonare_wrap_project.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap_utils.h"

namespace {

// Must match SONARE_PROJECT_ABI_VERSION (src/sonare_c_project.h) and the other
// bindings' EXPECTED_PROJECT_ABI_VERSION. A mismatch means the loaded native
// binary lays out the flat project PODs differently than this addon expects (or
// arrangement support was compiled out, in which case the runtime version is 0).
constexpr uint32_t kExpectedProjectAbiVersion = 1;

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

}  // namespace

Napi::FunctionReference ProjectWrap::constructor;

Napi::Object ProjectWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func =
      DefineClass(env, "Project",
                  {
                      InstanceMethod<&ProjectWrap::ToJson>("toJson"),
                      InstanceMethod<&ProjectWrap::SetSampleRate>("setSampleRate"),
                      InstanceMethod<&ProjectWrap::AddTrack>("addTrack"),
                      InstanceMethod<&ProjectWrap::AddClip>("addClip"),
                      InstanceMethod<&ProjectWrap::AddMidiClip>("addMidiClip"),
                      InstanceMethod<&ProjectWrap::SplitClip>("splitClip"),
                      InstanceMethod<&ProjectWrap::TrimClip>("trimClip"),
                      InstanceMethod<&ProjectWrap::MoveClip>("moveClip"),
                      InstanceMethod<&ProjectWrap::Undo>("undo"),
                      InstanceMethod<&ProjectWrap::Redo>("redo"),
                      InstanceMethod<&ProjectWrap::SetMidiEvents>("setMidiEvents"),
                      InstanceMethod<&ProjectWrap::ImportSmf>("importSmf"),
                      InstanceMethod<&ProjectWrap::ExportSmf>("exportSmf"),
                      InstanceMethod<&ProjectWrap::SetProgram>("setProgram"),
                      InstanceMethod<&ProjectWrap::SetProgramOnChannel>("setProgramOnChannel"),
                      InstanceMethod<&ProjectWrap::SetMidiFx>("setMidiFx"),
                      InstanceMethod<&ProjectWrap::AutoTempo>("autoTempo"),
                      InstanceMethod<&ProjectWrap::SnapToGrid>("snapToGrid"),
                      InstanceMethod<&ProjectWrap::Compile>("compile"),
                      InstanceMethod<&ProjectWrap::Bounce>("bounce"),
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

Napi::Value ProjectWrap::SetMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t clip_id = Uint32Arg(info, 0, 0);
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  ThrowIfError(env, sonare_project_set_midi_fx(project_, clip_id, config.c_str()));
  return env.Undefined();
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

Napi::Value ProjectWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectCompileResult result{};
  ThrowIfError(env, sonare_project_compile(project_, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("hasTimeline", Napi::Boolean::New(env, result.has_timeline != 0));
  out.Set("messages", Napi::String::New(env, result.messages != nullptr ? result.messages : ""));
  Napi::Array diagnostics = Napi::Array::New(env, result.diagnostic_count);
  for (size_t i = 0; i < result.diagnostic_count; ++i) {
    Napi::Object diag = Napi::Object::New(env);
    diag.Set("code", Napi::Number::New(env, result.diagnostics[i].code));
    diag.Set("severity", Napi::Number::New(env, result.diagnostics[i].severity));
    diag.Set("targetId", Napi::Number::New(env, result.diagnostics[i].target_id));
    diagnostics.Set(static_cast<uint32_t>(i), diag);
  }
  out.Set("diagnostics", diagnostics);
  sonare_project_free_compile_result(&result);
  return out;
}

Napi::Value ProjectWrap::Bounce(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectBounceOptions options{};
  if (info.Length() > 0 && info[0].IsObject()) {
    Napi::Object obj = info[0].As<Napi::Object>();
    options.total_frames = Int64Property(obj, "totalFrames", 0);
    options.block_size = IntProperty(obj, "blockSize", 0);
    options.num_channels = IntProperty(obj, "numChannels", 0);
    options.sample_rate = IntProperty(obj, "sampleRate", 0);
    options.instrument_latency_samples = IntProperty(obj, "instrumentLatencySamples", 0);
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

void ProjectWrap::Destroy(const Napi::CallbackInfo& info) {
  (void)info;
  if (project_ != nullptr) {
    sonare_project_destroy(project_);
    project_ = nullptr;
  }
}
