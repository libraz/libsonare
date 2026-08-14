#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

// Fills `options` from a JS bounce-options object (zero-initialized on entry).
void FillBounceOptions(const Napi::Object& obj, SonareProjectBounceOptions* options) {
  options->total_frames = Int64Property(obj, "totalFrames", 0);
  options->block_size = IntProperty(obj, "blockSize", 0);
  options->num_channels = IntProperty(obj, "numChannels", 0);
  options->sample_rate = IntProperty(obj, "sampleRate", 0);
  options->instrument_latency_samples = IntProperty(obj, "instrumentLatencySamples", 0);
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
    const int mapped = sonare_synth_builtin_waveform_from_name(name.c_str());
    if (mapped < 0) {
      Napi::TypeError::New(env, "Unknown synth waveform name: '" + name +
                                    "' (expected sine, saw, sawtooth, square, or triangle)")
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

// Marshals a heap-owned SonareProjectCompileResult into the JS compile-result
// object shape and frees its heap fields (consuming the struct).
Napi::Object CompileResultToObject(Napi::Env env, SonareProjectCompileResult* result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("hasTimeline", Napi::Boolean::New(env, result->has_timeline != 0));
  const std::string messages = result->messages != nullptr ? result->messages : "";
  out.Set("messages", Napi::String::New(env, messages));
  std::vector<std::string> diagnostic_messages;
  std::stringstream message_stream(messages);
  std::string line;
  while (std::getline(message_stream, line)) {
    diagnostic_messages.push_back(line);
  }
  Napi::Array diagnostics = Napi::Array::New(env, result->diagnostic_count);
  for (size_t i = 0; i < result->diagnostic_count; ++i) {
    Napi::Object diag = Napi::Object::New(env);
    diag.Set("code", Napi::Number::New(env, result->diagnostics[i].code));
    diag.Set("severity", Napi::Number::New(env, result->diagnostics[i].severity));
    diag.Set("targetId", Napi::Number::New(env, result->diagnostics[i].target_id));
    diag.Set("message",
             Napi::String::New(env, i < diagnostic_messages.size() ? diagnostic_messages[i] : ""));
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

// Compiles + renders the project, routing MIDI tracks through the patch-driven
// NativeSynth (the full synthesizer; see SonareSynthPatch). Argument order is
// instrument-first to match the WASM and Python bindings:
//   bounceWithSynthInstruments(instruments, options?)
// Each binding is { destinationId?, ...patch } where the patch is a SynthPatch
// object or a preset-name string ("saw-lead" / "va:saw-lead"). An unknown
// preset name throws (the C ABI rejects it).
Napi::Value ProjectWrap::BounceWithSynthInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
  }
  std::vector<SonareSynthInstrumentBinding> bindings;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      SonareSynthInstrumentBinding binding{};
      if (element.IsObject() && !element.IsArray()) {
        Napi::Object obj = element.As<Napi::Object>();
        binding.destination_id = sonare_node::Uint32Property(obj, "destinationId", 0u);
        if (env.IsExceptionPending()) return env.Undefined();
        binding.use_gm_programs = BoolProperty(obj, "useGmPrograms", false) ? 1 : 0;
        if (env.IsExceptionPending()) return env.Undefined();
      }
      if (!sonare_node::ReadSynthPatch(env, element, &binding.patch)) {
        return env.Undefined();  // exception already pending
      }
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_synth_instruments(
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

void ProjectWrap::LoadSoundFont(const Napi::CallbackInfo& info) {
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
    Napi::TypeError::New(env, "loadSoundFont expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return;
  }
  ThrowIfError(env, sonare_project_load_soundfont(project_, bytes, len));
}

void ProjectWrap::ClearSoundFont(const Napi::CallbackInfo& info) {
  ThrowIfError(info.Env(), sonare_project_clear_soundfont(project_));
}

Napi::Value ProjectWrap::SoundFontPresetCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t out = 0;
  ThrowIfError(env, sonare_project_soundfont_preset_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value ProjectWrap::SoundFontManifest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t total = 0;
  ThrowIfError(env, sonare_project_soundfont_manifest(project_, nullptr, 0, &total));
  if (env.IsExceptionPending()) return env.Undefined();
  std::vector<SonareSf2ProgramStatus> entries(total);
  if (total > 0) {
    ThrowIfError(env, sonare_project_soundfont_manifest(project_, entries.data(), total, &total));
    if (env.IsExceptionPending()) return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("channel", Napi::Number::New(env, entries[i].channel));
    entry.Set("bank", Napi::Number::New(env, entries[i].bank));
    entry.Set("program", Napi::Number::New(env, entries[i].program));
    entry.Set(
        "backend",
        Napi::String::New(env, entries[i].backend == SONARE_SOURCE_BACKEND_SF2 ? "sf2" : "synth"));
    entry.Set("presetName", Napi::String::New(env, entries[i].preset_name));
    out.Set(static_cast<uint32_t>(i), entry);
  }
  return out;
}

Napi::Value ProjectWrap::BounceWithSf2Instruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  // Argument order is instrument-first to match bounceWithBuiltinInstruments:
  //   bounceWithSf2Instruments(instruments, options?)
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
  }
  std::vector<SonareSf2InstrumentBinding> bindings;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      if (!element.IsObject()) {
        Napi::TypeError::New(env, "bounceWithSf2Instruments: instrument bindings must be objects")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = element.As<Napi::Object>();
      SonareSf2InstrumentBinding binding{};
      binding.destination_id = obj.Get("destinationId").IsUndefined()
                                   ? 0u
                                   : obj.Get("destinationId").As<Napi::Number>().Uint32Value();
      binding.config.gain = FloatProperty(obj, "gain", 0.0f);
      binding.config.polyphony = IntProperty(obj, "polyphony", 0);
      const Napi::Value prefer_model = obj.Get("preferModelForModeledFamilies");
      if (!prefer_model.IsUndefined() && !prefer_model.IsNull()) {
        binding.config.struct_version = 2;
        binding.config.prefer_model_for_modeled_families = prefer_model.ToBoolean().Value() ? 1 : 0;
      }
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_sf2_instruments(
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
