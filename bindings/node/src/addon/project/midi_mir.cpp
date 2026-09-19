#include <cstdint>
#include <cstring>
#include <memory>
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
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::vector<SonareMidiEventPod> events;
  if (info.Length() > 1 && info[1].IsArray()) {
    Napi::Array input = info[1].As<Napi::Array>();
    events.reserve(input.Length());
    for (uint32_t i = 0; i < input.Length(); ++i) {
      Napi::Value entry = input.Get(i);
      SonareMidiEventPod ev{};
      if (entry.IsArray()) {
        Napi::Array tuple = entry.As<Napi::Array>();
        if (!RequiredDoubleValue(env, tuple.Get(0u), "MIDI event ppq", &ev.ppq)) {
          return env.Undefined();
        }
        if (!RequiredWordValue(env, tuple.Get(1u), "MIDI event data0", &ev.data0)) {
          return env.Undefined();
        }
        if (!RequiredWordValue(env, tuple.Get(2u), "MIDI event data1", &ev.data1)) {
          return env.Undefined();
        }
      } else if (entry.IsObject()) {
        Napi::Object obj = entry.As<Napi::Object>();
        if (!RequiredDoubleProperty(env, obj, "ppq", &ev.ppq)) return env.Undefined();
        if (!RequiredWordValue(env, obj.Get("data0"), "data0", &ev.data0)) return env.Undefined();
        ev.data1 = WordProperty(obj, "data1", 0u);
        if (env.IsExceptionPending()) return env.Undefined();
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ImportSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ExportSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t* bytes = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_export_smf(project_, &bytes, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Buffer<uint8_t> out = Napi::Buffer<uint8_t>::Copy(env, bytes != nullptr ? bytes : nullptr,
                                                          bytes != nullptr ? len : 0);
  if (bytes != nullptr) sonare_free_bytes(bytes);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ImportClipFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ExportClipFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t* bytes = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_export_clip_file(project_, &bytes, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Buffer<uint8_t> out = Napi::Buffer<uint8_t>::Copy(env, bytes != nullptr ? bytes : nullptr,
                                                          bytes != nullptr ? len : 0);
  if (bytes != nullptr) sonare_free_bytes(bytes);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  int program = 0;
  int bank = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !Int32Arg(env, info, 1, "program", 0, &program) ||
      !Int32Arg(env, info, 2, "bank", 0, &bank)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_program(project_, clip_id, program, bank));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetProgramOnChannel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // group/channel are uint8_t C-ABI arguments; the MidiByte reader rejects the
  // values a narrowing cast would wrap into a range the C ABI accepts.
  uint32_t clip_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel)) {
    return env.Undefined();
  }
  int program = 0;
  int bank = -1;
  if (!Int32Arg(env, info, 3, "program", 0, &program) ||
      !Int32Arg(env, info, 4, "bank", -1, &bank)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_project_set_program_on_channel(project_, clip_id, group, channel, program, bank));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::BakeMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  ThrowIfError(env, sonare_project_bake_midi_fx(project_, clip_id, config.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::BakeMidiFxWithSourceIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  // Size the provenance buffer from the non-destructive preview so the bake
  // runs once with an exactly-fitting buffer.
  size_t expected = 0;
  ThrowIfError(env,
               sonare_project_preview_midi_fx_count(project_, clip_id, config.c_str(), &expected));
  if (env.IsExceptionPending()) return env.Undefined();
  std::vector<int32_t> source_index(expected, -1);
  size_t written = 0;
  ThrowIfError(
      env, sonare_project_bake_midi_fx_ex(project_, clip_id, config.c_str(), source_index.data(),
                                          source_index.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  const size_t count = std::min(written, source_index.size());
  Napi::Int32Array out = Napi::Int32Array::New(env, count);
  for (size_t i = 0; i < count; ++i) out[i] = source_index[i];
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::PreviewMidiFxCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  size_t count = 0;
  ThrowIfError(env,
               sonare_project_preview_midi_fx_count(project_, clip_id, config.c_str(), &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY return BakeMidiFx(info);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ValidateMidiNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  SonareNotePairValidation out{};
  ThrowIfError(env, sonare_project_validate_midi_notes(project_, clip_id, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, out.ok != 0));
  result.Set("unmatchedNoteOns", Napi::Number::New(env, out.unmatched_note_ons));
  result.Set("unmatchedNoteOffs", Napi::Number::New(env, out.unmatched_note_offs));
  return result;
  SONARE_NODE_CATCH(env)
}

namespace {

// Seeds the native defaults, then applies whichever the caller supplied. Seeding
// rather than zeroing matters: a zeroed ramp_threshold folds the whole take into
// one tempo segment and a zeroed interval is rejected.
SonareProjectTempoOptions TempoOptionsFrom(Napi::Value value) {
  SonareProjectTempoOptions options = sonare_project_tempo_options_default();
  if (!value.IsObject()) return options;
  Napi::Object object = value.As<Napi::Object>();
  options.adaptive_tempo =
      sonare_node::BoolProperty(object, "adaptiveTempo", options.adaptive_tempo != 0) ? 1 : 0;
  options.tempo_update_interval_beats = sonare_node::IntProperty(
      object, "tempoUpdateIntervalBeats", options.tempo_update_interval_beats);
  options.ramp_threshold = static_cast<float>(
      sonare_node::DoubleProperty(object, "rampThreshold", options.ramp_threshold));
  options.include_octave_candidates =
      sonare_node::BoolProperty(object, "includeOctaveCandidates",
                                options.include_octave_candidates != 0)
          ? 1
          : 0;
  return options;
}

}  // namespace

Napi::Value ProjectWrap::AutoTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "autoTempo expects a Float32Array of mono audio")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 1, "sampleRate", 0, &sample_rate)) return env.Undefined();
  size_t candidate_index = 0;
  if (!NonNegativeSizeTArg(env, info, 2, "candidateIndex", &candidate_index)) {
    return env.Undefined();
  }
  float out_bpm = 0.0f;
  const bool apply_time_signatures = info.Length() > 3 && info[3].ToBoolean().Value();
  const SonareProjectTempoOptions options =
      TempoOptionsFrom(info.Length() > 4 ? info[4] : env.Undefined());
  ThrowIfError(env, sonare_project_auto_tempo_with_options(
                        project_, audio.Data(), audio.ElementLength(), sample_rate, &options,
                        candidate_index, apply_time_signatures ? 1 : 0, &out_bpm));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_bpm);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnalyzeTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "analyzeTempo expects a Float32Array of mono audio")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 1, "sampleRate", 0, &sample_rate)) return env.Undefined();
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  const SonareProjectTempoOptions options =
      TempoOptionsFrom(info.Length() > 2 ? info[2] : env.Undefined());
  ThrowIfError(env, sonare_project_analyze_tempo_with_options(
                        project_, audio.Data(), audio.ElementLength(), sample_rate, &options,
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SnapToGrid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  double ppq = 0.0;
  double strength = 1.0;
  int division = 1;
  if (!OptionalDoubleArg(env, info, 0, "ppq", 0.0, &ppq) ||
      !OptionalDoubleArg(env, info, 1, "strength", 1.0, &strength) ||
      !Int32Arg(env, info, 2, "division", 1, &division)) {
    return env.Undefined();
  }
  double out_ppq = 0.0;
  ThrowIfError(env, sonare_project_snap_to_grid_ex(project_, ppq, strength, division, &out_ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_ppq);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnnotateKeys(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnnotateChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
              static_cast<uint8_t>(node_narrow_uint32(env, arr.Get(j), "extensions")));
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AssistSidecarCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(env, static_cast<double>(sonare_project_assist_sidecar_count(project_)));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectAssistSidecar sidecar{};
  ThrowIfError(env, sonare_project_get_assist_sidecar(project_, index, &sidecar));
  if (env.IsExceptionPending()) return env.Undefined();
  return AssistSidecarToObject(env, &sidecar);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AssistSidecars(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const size_t count = sonare_project_assist_sidecar_count(project_);
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    SonareProjectAssistSidecar sidecar{};
    ThrowIfError(env, sonare_project_get_assist_sidecar(project_, i, &sidecar));
    if (env.IsExceptionPending()) return env.Undefined();
    out.Set(static_cast<uint32_t>(i), AssistSidecarToObject(env, &sidecar));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

// Owns the heap anchor array for the rest of the call, so the marshalling below
// cannot leak it by throwing.
struct WarpAnchorArrayDeleter {
  void operator()(SonareProjectWarpAnchor* anchors) const { sonare_free_warp_anchors(anchors); }
};

}  // namespace

namespace sonare_node {

Napi::Value AlignTakeToReference(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(
        env, "alignTakeToReference expects (reference, take, sampleRate) with two Float32Arrays")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array reference = info[0].As<Napi::Float32Array>();
  Napi::Float32Array take = info[1].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 2, "sampleRate", 0, &sample_rate)) return env.Undefined();
  // Zeroed rather than seeded: the C entry reads 0 on either field as "library
  // value", so a key the caller left out must stay 0 instead of carrying a
  // default this surface spelled.
  SonareTakeAlignConfig config{};
  if (info.Length() > 3 && info[3].IsObject()) {
    Napi::Object options = info[3].As<Napi::Object>();
    config.hop_length = IntProperty(options, "hopLength", kZeroIsSentinel);
    config.bins_per_octave = IntProperty(options, "binsPerOctave", kZeroIsSentinel);
  }

  SonareProjectWarpAnchor* raw_anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  const SonareError code = sonare_align_take_to_reference(
      reference.Data(), reference.ElementLength(), take.Data(), take.ElementLength(), sample_rate,
      &config, &raw_anchors, &count, &alignment);
  std::unique_ptr<SonareProjectWarpAnchor, WarpAnchorArrayDeleter> anchors(raw_anchors);
  ThrowIfError(env, code);
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Array out_anchors = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    Napi::Object anchor = Napi::Object::New(env);
    anchor.Set("warpSample", Napi::Number::New(env, anchors.get()[i].warp_sample));
    anchor.Set("sourceSample", Napi::Number::New(env, anchors.get()[i].source_sample));
    out_anchors.Set(static_cast<uint32_t>(i), anchor);
  }
  Napi::Object conditioning = Napi::Object::New(env);
  conditioning.Set("meanResidualFrames", Napi::Number::New(env, alignment.mean_residual_frames));
  conditioning.Set("referenceFrames", Napi::Number::New(env, alignment.reference_frames));
  conditioning.Set("takeFrames", Napi::Number::New(env, alignment.take_frames));
  Napi::Object result = Napi::Object::New(env);
  result.Set("anchors", out_anchors);
  result.Set("alignment", conditioning);
  return result;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
