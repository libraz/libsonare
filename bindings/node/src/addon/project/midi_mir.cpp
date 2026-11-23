#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "c_api/midi_input_domains.h"
#include "c_api/project_input_domains.h"
#include "feature/cqt.h"
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
    std::memcpy(sonare_node::ElementData(payload), sidecar->payload, sidecar->payload_len);
  }
  out.Set("payload", payload);
  sonare_free_project_assist_sidecar(sidecar);
  return out;
}

}  // namespace

Napi::Value ProjectWrap::SetMidiEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::vector<SonareMidiEventPod> events;
  if (info.Length() > 1 && info[1].IsArray()) {
    Napi::Array input = info[1].As<Napi::Array>();
    const uint32_t input_count = sonare_node::node_list_length(input, "events");
    events.reserve(input_count);
    for (uint32_t i = 0; i < input_count; ++i) {
      Napi::Value entry = input.Get(i);
      SonareMidiEventPod ev{};
      if (entry.IsArray()) {
        Napi::Array tuple = entry.As<Napi::Array>();
        if (!RequiredDoubleValue(env, tuple.Get(0u), "MIDI event ppq", &ev.ppq)) {
          return env.Undefined();
        }
        if (!RequiredWordValue(env, tuple.Get(1u), "MIDI event word0", &ev.word0)) {
          return env.Undefined();
        }
        if (!RequiredWordValue(env, tuple.Get(2u), "MIDI event word1", &ev.word1)) {
          return env.Undefined();
        }
      } else if (entry.IsObject()) {
        Napi::Object obj = entry.As<Napi::Object>();
        if (!RequiredDoubleProperty(env, obj, "ppq", &ev.ppq)) return env.Undefined();
        if (!RequiredWordValue(env, obj.Get("word0"), "word0", &ev.word0)) return env.Undefined();
        ev.word1 = WordProperty(obj, "word1", ev.word1);
        if (env.IsExceptionPending()) return env.Undefined();
      } else {
        Napi::TypeError::New(env, "MIDI event must be a [ppq, word0, word1] tuple or object")
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (!RequiredBytesArg(env, info, 0, "data", &bytes, &len)) return env.Undefined();
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_import_smf(project_, bytes, len, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ExportSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (!RequiredBytesArg(env, info, 0, "data", &bytes, &len)) return env.Undefined();
  uint32_t out_id = 0;
  ThrowIfError(env, sonare_project_import_clip_file(project_, bytes, len, &out_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ExportClipFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
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

// setProgram(clipId, program, bank?, group?, channel?): an absent bank is -1 (no Bank Select), an
// absent group and channel are 0.
Napi::Value ProjectWrap::SetProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  int program = 0;
  int bank = -1;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id) ||
      !Int32Arg(env, info, 1, "program", 0, &program) ||
      !Int32Arg(env, info, 2, "bank", -1, &bank)) {
    return env.Undefined();
  }
  // group/channel are uint8_t C-ABI arguments, read against the domains the C ABI enforces.
  uint8_t group = 0;
  uint8_t channel = 0;
  if (!sonare_node::OptionalDomainArg(env, info, 3, "group", uint8_t{0},
                                      sonare_c_detail::kMidiUmpGroupDomain, &group) ||
      !sonare_node::OptionalDomainArg(env, info, 4, "channel", uint8_t{0},
                                      sonare_c_detail::kMidiChannelDomain, &channel)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_program(project_, clip_id, group, channel, program, bank));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// A trailing `withSourceIndex` of true returns the per-event provenance as an Int32Array.
Napi::Value ProjectWrap::BakeMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::string config;
  if (!OptionalStringArg(env, info, 1, "config", "", &config)) return env.Undefined();
  bool with_source_index = false;
  if (!OptionalBoolArg(env, info, 2, "withSourceIndex", false, &with_source_index)) {
    return env.Undefined();
  }
  if (!with_source_index) {
    ThrowIfError(
        env, sonare_project_bake_midi_fx(project_, clip_id, config.c_str(), nullptr, 0, nullptr));
    return env.Undefined();
  }
  // Size the provenance buffer from the non-destructive preview so the bake
  // runs once with an exactly-fitting buffer.
  size_t expected = 0;
  ThrowIfError(env,
               sonare_project_preview_midi_fx_count(project_, clip_id, config.c_str(), &expected));
  if (env.IsExceptionPending()) return env.Undefined();
  std::vector<int32_t> source_index(expected, -1);
  size_t written = 0;
  ThrowIfError(env, sonare_project_bake_midi_fx(project_, clip_id, config.c_str(),
                                                sonare_node::ElementData(source_index),
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t clip_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "clipId", 0, &clip_id)) return env.Undefined();
  std::string config;
  if (!OptionalStringArg(env, info, 1, "config", "", &config)) return env.Undefined();
  size_t count = 0;
  ThrowIfError(env,
               sonare_project_preview_midi_fx_count(project_, clip_id, config.c_str(), &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ValidateMidiNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
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

// Seeds the native defaults, then applies whichever the caller supplied.
SonareProjectTempoOptions TempoOptionsFrom(Napi::Value value) {
  SonareProjectTempoOptions options;
  sonare_project_tempo_options_init(&options);
  if (!value.IsObject()) return options;
  Napi::Object object = value.As<Napi::Object>();
  options.adaptive_tempo =
      sonare_node::BoolProperty(object, "adaptiveTempo", options.adaptive_tempo != 0) ? 1 : 0;
  options.tempo_update_interval_beats = sonare_node::DomainProperty<int>(
      object, "tempoUpdateIntervalBeats", options.tempo_update_interval_beats,
      sonare::kMusicCountDomain);
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 1, "sampleRate", 0, &sample_rate)) return env.Undefined();
  size_t candidate_index = 0;
  if (!NonNegativeSizeTArg(env, info, 2, "candidateIndex", &candidate_index)) {
    return env.Undefined();
  }
  bool apply_time_signatures = false;
  if (!OptionalBoolArg(env, info, 3, "applyTimeSignatures", false, &apply_time_signatures)) {
    return env.Undefined();
  }
  float out_bpm = 0.0f;
  const SonareProjectTempoOptions options =
      TempoOptionsFrom(info.Length() > 4 ? info[4] : env.Undefined());
  ThrowIfError(
      env, sonare_project_auto_tempo(project_, sonare_node::ElementData(audio),
                                     audio.ElementLength(), sample_rate, &options, candidate_index,
                                     apply_time_signatures ? 1 : 0, &out_bpm));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_bpm);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnalyzeTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !sonare_node::IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array audio = info[0].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 1, "sampleRate", 0, &sample_rate)) return env.Undefined();
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  const SonareProjectTempoOptions options =
      TempoOptionsFrom(info.Length() > 2 ? info[2] : env.Undefined());
  ThrowIfError(env, sonare_project_analyze_tempo(project_, sonare_node::ElementData(audio),
                                                 audio.ElementLength(), sample_rate, &options,
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double ppq = 0.0;
  double strength = 1.0;
  int division = 1;
  if (!OptionalDoubleArg(env, info, 0, "ppq", 0.0, &ppq) ||
      !OptionalDoubleArg(env, info, 1, "strength", 1.0, &strength) ||
      !sonare_node::OptionalDomainArg<int>(env, info, 2, "division", 1,
                                           sonare_c_detail::kSnapDivisionDomain, &division)) {
    return env.Undefined();
  }
  double out_ppq = 0.0;
  SonareProjectSnapToGridOptions snap_options;
  sonare_project_snap_to_grid_options_init(&snap_options);
  snap_options.division = division;
  ThrowIfError(env, sonare_project_snap_to_grid(project_, ppq, strength, &snap_options, &out_ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out_ppq);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnnotateKeys(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "keys", &input)) return env.Undefined();
  std::vector<SonareProjectKeySegment> keys;
  const uint32_t input_count = sonare_node::node_list_length(input, "keys");
  keys.reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Value entry = input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "key segment must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareProjectKeySegment seg{};
    if (!RequiredDoubleProperty(env, obj, "startPpq", &seg.start_ppq) ||
        !RequiredDoubleProperty(env, obj, "endPpq", &seg.end_ppq)) {
      return env.Undefined();
    }
    seg.tonic_pc =
        static_cast<uint32_t>(sonare_node::IntInRangeOrAbsentProperty(obj, "tonicPc", 0, 11, 255));
    seg.mode = Uint32Property(obj, "mode", seg.mode);
    keys.push_back(seg);
  }
  ThrowIfError(env, sonare_project_annotate_keys(project_, keys.empty() ? nullptr : keys.data(),
                                                 keys.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AnnotateChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  std::vector<SonareProjectChordSymbol> chords;
  // The extension byte arrays and roman-numeral strings must outlive the C call;
  // keep them in side buffers parallel to `chords` (pointers patched after fill).
  std::vector<std::vector<uint8_t>> extensions;
  std::vector<std::string> roman;
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "chords", &input)) return env.Undefined();
  const uint32_t input_count = sonare_node::node_list_length(input, "chords");
  chords.reserve(input_count);
  extensions.resize(input_count);
  roman.resize(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Value entry = input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "chord symbol must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareProjectChordSymbol chord{};
    if (!RequiredDoubleProperty(env, obj, "startPpq", &chord.start_ppq) ||
        !RequiredDoubleProperty(env, obj, "endPpq", &chord.end_ppq)) {
      return env.Undefined();
    }
    chord.root_pc =
        static_cast<uint32_t>(sonare_node::IntInRangeOrAbsentProperty(obj, "rootPc", 0, 11, 255));
    chord.quality = Uint32Property(obj, "quality", chord.quality);
    Napi::Value ext = obj.Get("extensions");
    if (!ext.IsUndefined() && !ext.IsNull()) {
      Napi::Array arr;
      if (!RequiredArrayValue(env, ext, "extensions", &arr)) return env.Undefined();
      const uint32_t arr_count = sonare_node::node_list_length(arr, "extensions");
      extensions[i].reserve(arr_count);
      for (uint32_t j = 0; j < arr_count; ++j) {
        uint8_t extension = 0;
        if (!RequiredMidiByteValue(env, arr.Get(j), "extensions[" + std::to_string(j) + "]",
                                   &extension)) {
          return env.Undefined();
        }
        extensions[i].push_back(extension);
      }
    }
    chord.extensions = extensions[i].empty() ? nullptr : extensions[i].data();
    chord.extension_count = extensions[i].size();
    chord.slash_bass_pc = static_cast<uint32_t>(
        sonare_node::IntInRangeOrAbsentProperty(obj, "slashBassPc", 0, 11, 255));
    Napi::Value rn = obj.Get("romanNumeral");
    if (!rn.IsUndefined() && !rn.IsNull()) {
      roman[i] = StringProperty(obj, "romanNumeral", "");
      chord.roman_numeral = roman[i].c_str();
    }
    chord.modulation_boundary =
        BoolProperty(obj, "modulationBoundary", chord.modulation_boundary != 0) ? 1 : 0;
    if (env.IsExceptionPending()) return env.Undefined();
    chords.push_back(chord);
  }
  ThrowIfError(env, sonare_project_annotate_chords(
                        project_, chords.empty() ? nullptr : chords.data(), chords.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "sidecar must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  Napi::Value module_value = obj.Get("moduleId");
  if (!module_value.IsString()) {
    Napi::TypeError::New(env, "moduleId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string module_id = sonare_node::node_narrow_string(env, module_value, "moduleId");
  const uint32_t schema_version = Uint32Property(obj, "schemaVersion", 0);
  const uint32_t target_track_id = Uint32Property(obj, "targetTrackId", 0);
  const double region_start_ppq = DoubleProperty(obj, "regionStartPpq", 0.0);
  const double region_end_ppq = DoubleProperty(obj, "regionEndPpq", 0.0);
  // Every field is read and checked before the C call, so a refusal leaves the project untouched.
  std::vector<uint8_t> payload;
  Napi::Value payload_value = obj.Get("payload");
  if (sonare_node::IsUint8Array(payload_value)) {
    Napi::Uint8Array arr = payload_value.As<Napi::Uint8Array>();
    payload.assign(sonare_node::ElementData(arr), sonare_node::ElementData(arr) + arr.ByteLength());
  } else if (payload_value.IsBuffer()) {
    Napi::Buffer<uint8_t> buf = payload_value.As<Napi::Buffer<uint8_t>>();
    payload.assign(sonare_node::ElementData(buf),
                   sonare_node::ElementData(buf) + buf.ElementLength());
  } else if (!payload_value.IsUndefined() && !payload_value.IsNull()) {
    Napi::TypeError::New(env, "payload must be a Uint8Array").ThrowAsJavaScriptException();
    return env.Undefined();
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  return Napi::Number::New(env, static_cast<double>(sonare_project_assist_sidecar_count(project_)));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetAssistSidecar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
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
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
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
  void operator()(SonareProjectWarpAnchor* anchors) const {
    sonare_free_project_warp_anchors(anchors);
  }
};

// Capacity for the extension scale degrees; the library emits at most three.
constexpr size_t kChordExtensionCapacity = 32;

}  // namespace

namespace sonare_node {

Napi::Value AlignTakeToReference(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "reference must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "take must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 4) {
    Napi::TypeError::New(env, "takeSampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array reference = info[0].As<Napi::Float32Array>();
  Napi::Float32Array take = info[1].As<Napi::Float32Array>();
  int sample_rate = 0;
  if (!Int32Arg(env, info, 2, "sampleRate", 0, &sample_rate)) return env.Undefined();
  int take_sample_rate = 0;
  if (!Int32Arg(env, info, 3, "takeSampleRate", 0, &take_sample_rate)) return env.Undefined();
  SonareTakeAlignConfig config;
  sonare_take_align_config_init(&config);
  if (info.Length() > 4 && info[4].IsObject()) {
    Napi::Object options = info[4].As<Napi::Object>();
    config.hop_length = sonare_node::DomainProperty<int>(options, "hopLength", config.hop_length,
                                                         sonare::kCqtHopLengthDomain);
    config.bins_per_octave = sonare_node::DomainProperty<int>(
        options, "binsPerOctave", config.bins_per_octave, sonare::kCqtCountDomain);
  }

  SonareProjectWarpAnchor* raw_anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  const SonareError code = sonare_align_take_to_reference(
      sonare_node::ElementData(reference), reference.ElementLength(), sample_rate,
      sonare_node::ElementData(take), take.ElementLength(), take_sample_rate, &config, &raw_anchors,
      &count, &alignment);
  std::unique_ptr<SonareProjectWarpAnchor, WarpAnchorArrayDeleter> anchors(raw_anchors);
  if (code != SONARE_OK) {
    ThrowSonareError(env, code);
    return env.Undefined();
  }

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

Napi::Value ChordSymbolFromAnalysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsObject() || info[0].IsArray()) {
    Napi::TypeError::New(env, "chord must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object chord = info[0].As<Napi::Object>();
  const SonarePitchClass root = PitchClassFromValue(chord.Get("root"), "chord.root");
  const SonarePitchClass bass = PitchClassFromValue(chord.Get("bass"), "chord.bass");
  const SonareChordQuality quality = ChordQualityFromValue(chord.Get("quality"), "chord.quality");

  uint32_t root_pc = 0;
  uint32_t out_quality = 0;
  uint32_t slash_bass_pc = 0;
  uint8_t extensions[kChordExtensionCapacity] = {};
  size_t extension_count = 0;
  ThrowIfError(env, sonare_chord_symbol_from_analysis(root, quality, bass, &root_pc, &out_quality,
                                                      extensions, kChordExtensionCapacity,
                                                      &extension_count, &slash_bass_pc));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Array out_extensions = Napi::Array::New(env, extension_count);
  for (size_t i = 0; i < extension_count; ++i) {
    out_extensions.Set(static_cast<uint32_t>(i), Napi::Number::New(env, extensions[i]));
  }
  Napi::Object result = Napi::Object::New(env);
  result.Set("rootPc", Napi::Number::New(env, root_pc));
  result.Set("quality", Napi::Number::New(env, out_quality));
  result.Set("extensions", out_extensions);
  result.Set("slashBassPc", Napi::Number::New(env, slash_bass_pc));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value KeyModeFromAnalysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "mode must be a mode name").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string name = node_narrow_string(env, info[0], "mode");
  for (int mode = SONARE_MODE_MAJOR; mode <= SONARE_MODE_LOCRIAN; ++mode) {
    if (name == ModeNameLocal(static_cast<SonareMode>(mode))) {
      uint32_t out_mode = 0;
      ThrowIfError(env, sonare_key_mode_from_analysis(static_cast<SonareMode>(mode), &out_mode));
      if (env.IsExceptionPending()) return env.Undefined();
      return Napi::Number::New(env, out_mode);
    }
  }
  throw Napi::RangeError::New(env, "mode is not a key mode name: '" + name + "'");
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
