#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

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
  const size_t candidate_index = static_cast<size_t>(NumberArg(info, 2, 0.0));
  const bool apply_time_signatures = info.Length() > 3 && info[3].ToBoolean().Value();
  ThrowIfError(
      env, sonare_project_auto_tempo_ex(project_, audio.Data(), audio.ElementLength(), sample_rate,
                                        candidate_index, apply_time_signatures ? 1 : 0, &out_bpm));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_bpm);
}

Napi::Value ProjectWrap::AnalyzeTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "analyzeTempo expects a Float32Array of mono audio")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  ThrowIfError(env, sonare_project_analyze_tempo(project_, audio.Data(), audio.ElementLength(),
                                                 static_cast<int>(NumberArg(info, 1, 0.0)),
                                                 candidates, std::size(candidates), &count));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array output =
      Napi::Array::New(env, static_cast<uint32_t>(std::min(count, std::size(candidates))));
  const char* labels[] = {"primary", "half", "double"};
  for (size_t i = 0; i < count && i < std::size(candidates); ++i) {
    const auto& candidate = candidates[i];
    Napi::Object value = Napi::Object::New(env);
    value.Set("bpm", Napi::Number::New(env, candidate.bpm));
    value.Set("confidence", Napi::Number::New(env, candidate.confidence));
    value.Set("label",
              labels[candidate.kind <= SONARE_TEMPO_CANDIDATE_DOUBLE ? candidate.kind : 0]);
    value.Set("timeSignatureCount", Napi::Number::New(env, candidate.time_signature_count));
    Napi::Object time_signature = Napi::Object::New(env);
    time_signature.Set("startPpq",
                       Napi::Number::New(env, candidate.first_time_signature.start_ppq));
    time_signature.Set("numerator",
                       Napi::Number::New(env, candidate.first_time_signature.numerator));
    time_signature.Set("denominator",
                       Napi::Number::New(env, candidate.first_time_signature.denominator));
    value.Set("timeSignature", time_signature);
    output.Set(static_cast<uint32_t>(i), value);
  }
  return output;
}

Napi::Value ProjectWrap::SnapToGrid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  double out_ppq = 0.0;
  ThrowIfError(env, sonare_project_snap_to_grid_ex(
                        project_, NumberArg(info, 0, 0.0), NumberArg(info, 1, 1.0),
                        static_cast<int>(NumberArg(info, 2, 1.0)), &out_ppq));
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
