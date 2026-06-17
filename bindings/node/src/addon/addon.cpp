#include <napi.h>
#include <sonare/sonare_c.h>

#include <string>
#include <vector>

#include "sonare_wrap.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

namespace {

Napi::Value EngineAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_engine_abi_version());
}

Napi::Value VoiceChangerAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_voice_changer_abi_version());
}

Napi::Value ProjectAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_project_abi_version());
}

Napi::Value NullableString(Napi::Env env, const char* value) {
  return value != nullptr ? Napi::String::New(env, value) : env.Null();
}

Napi::Value MidiGmInstrumentName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm_instrument_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiGmProgramForName(const Napi::CallbackInfo& info) {
  return Napi::Number::New(
      info.Env(), sonare_midi_gm_program_for_name(info[0].As<Napi::String>().Utf8Value().c_str()));
}

Napi::Value MidiGmFamilyName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm_family_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiGmFamilyFirstProgram(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(),
                           sonare_midi_gm_family_first_program(info[0].As<Napi::Number>()));
}

Napi::Value MidiGm2InstrumentName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm2_instrument_name(info[0].As<Napi::Number>(),
                                                                    info[1].As<Napi::Number>()));
}

Napi::Value MidiGmDrumName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm_drum_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiGmDrumNoteForName(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_midi_gm_drum_note_for_name(
                                           info[0].As<Napi::String>().Utf8Value().c_str()));
}

Napi::Value MidiGm2DrumSetName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm2_drum_set_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiGm2DrumName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_gm2_drum_name(info[0].As<Napi::Number>(),
                                                              info[1].As<Napi::Number>()));
}

Napi::Value MidiCcName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(), sonare_midi_cc_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiCcIndexForName(const Napi::CallbackInfo& info) {
  return Napi::Number::New(
      info.Env(), sonare_midi_cc_index_for_name(info[0].As<Napi::String>().Utf8Value().c_str()));
}

Napi::Value MidiPerNoteControllerName(const Napi::CallbackInfo& info) {
  return NullableString(info.Env(),
                        sonare_midi_per_note_controller_name(info[0].As<Napi::Number>()));
}

Napi::Value MidiBankProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareMidiEventPod events[3]{};
  size_t count = 0;
  const SonareError err = sonare_midi_bank_program(
      info[0].As<Napi::Number>(), static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()),
      static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()), info[3].As<Napi::Number>(),
      info[4].As<Napi::Number>(), info[5].As<Napi::Number>(), events, 3, &count);
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI bank/program arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    Napi::Object event = Napi::Object::New(env);
    event.Set("ppq", Napi::Number::New(env, events[i].ppq));
    event.Set("data0", Napi::Number::New(env, events[i].data0));
    event.Set("data1", Napi::Number::New(env, events[i].data1));
    out.Set(i, event);
  }
  return out;
}

SonareMidiEventPod MidiEventFromObject(Napi::Object event) {
  SonareMidiEventPod out{};
  out.ppq = event.Get("ppq").As<Napi::Number>();
  out.data0 = event.Get("data0").As<Napi::Number>().Uint32Value();
  out.data1 = event.Has("data1") ? event.Get("data1").As<Napi::Number>().Uint32Value() : 0;
  return out;
}

Napi::Object MidiEventToObject(Napi::Env env, const SonareMidiEventPod& event) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("ppq", Napi::Number::New(env, event.ppq));
  out.Set("data0", Napi::Number::New(env, event.data0));
  out.Set("data1", Napi::Number::New(env, event.data1));
  return out;
}

SonareMidiCcBinding CcBindingFromObject(Napi::Object object) {
  SonareMidiCcBinding out{};
  out.cc_number = object.Get("ccNumber").As<Napi::Number>().Uint32Value();
  out.channel = object.Has("channel") && !object.Get("channel").IsNull()
                    ? object.Get("channel").As<Napi::Number>().Uint32Value()
                    : 0xffu;
  out.kind = object.Has("kind") ? object.Get("kind").As<Napi::Number>().Uint32Value() : 0u;
  out.cc_lsb_number =
      object.Has("ccLsbNumber") ? object.Get("ccLsbNumber").As<Napi::Number>().Uint32Value() : 0u;
  out.selector_msb =
      object.Has("selectorMsb") ? object.Get("selectorMsb").As<Napi::Number>().Uint32Value() : 0u;
  out.selector_lsb =
      object.Has("selectorLsb") ? object.Get("selectorLsb").As<Napi::Number>().Uint32Value() : 0u;
  out.param_id = object.Get("paramId").As<Napi::Number>().Uint32Value();
  out.min_value =
      object.Has("minValue") ? object.Get("minValue").As<Napi::Number>().FloatValue() : 0.0f;
  out.max_value =
      object.Has("maxValue") ? object.Get("maxValue").As<Napi::Number>().FloatValue() : 1.0f;
  return out;
}

Napi::Object CcBindingToObject(Napi::Env env, const SonareMidiCcBinding& binding) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("ccNumber", Napi::Number::New(env, binding.cc_number));
  out.Set("channel", Napi::Number::New(env, binding.channel));
  out.Set("kind", Napi::Number::New(env, binding.kind));
  out.Set("ccLsbNumber", Napi::Number::New(env, binding.cc_lsb_number));
  out.Set("selectorMsb", Napi::Number::New(env, binding.selector_msb));
  out.Set("selectorLsb", Napi::Number::New(env, binding.selector_lsb));
  out.Set("paramId", Napi::Number::New(env, binding.param_id));
  out.Set("minValue", Napi::Number::New(env, binding.min_value));
  out.Set("maxValue", Napi::Number::New(env, binding.max_value));
  return out;
}

std::vector<SonareMidiCcBinding> CcBindingsFromArray(Napi::Array array) {
  std::vector<SonareMidiCcBinding> bindings(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) {
    bindings[i] = CcBindingFromObject(array.Get(i).As<Napi::Object>());
  }
  return bindings;
}

Napi::Value MidiCcLearn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array array = info[0].As<Napi::Array>();
  std::vector<SonareMidiEventPod> events(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) {
    events[i] = MidiEventFromObject(array.Get(i).As<Napi::Object>());
  }
  SonareMidiCcBinding learned{};
  const SonareError err = sonare_midi_cc_learn(
      events.empty() ? nullptr : events.data(), events.size(),
      info[1].As<Napi::Number>().Uint32Value(),
      info.Length() > 2 ? info[2].As<Napi::Number>().FloatValue() : 0.0f,
      info.Length() > 3 ? info[3].As<Napi::Number>().FloatValue() : 1.0f,
      info.Length() > 4 ? static_cast<uint8_t>(info[4].As<Napi::Number>().Uint32Value()) : 0u,
      &learned);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI CC learn arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CcBindingToObject(env, learned);
}

Napi::Value MidiCcToBreakpoint(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareMidiCcBinding> bindings = CcBindingsFromArray(info[0].As<Napi::Array>());
  SonareMidiEventPod event = MidiEventFromObject(info[1].As<Napi::Object>());
  SonareAutomationPoint point{};
  const SonareError err = sonare_midi_cc_to_breakpoint(bindings.empty() ? nullptr : bindings.data(),
                                                       bindings.size(), &event, &point);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI CC breakpoint arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("ppq", Napi::Number::New(env, point.ppq));
  out.Set("value", Napi::Number::New(env, point.value));
  out.Set("curveToNext", Napi::Number::New(env, point.curve_to_next));
  return out;
}

Napi::Value MidiParamToCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::vector<SonareMidiCcBinding> bindings = CcBindingsFromArray(info[0].As<Napi::Array>());
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      bindings.empty() ? nullptr : bindings.data(), bindings.size(),
      info[1].As<Napi::Number>().Uint32Value(), info[2].As<Napi::Number>().FloatValue(),
      static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()),
      info.Length() > 4 ? info[4].As<Napi::Number>().DoubleValue() : 0.0, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI param-to-CC arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return MidiEventToObject(env, event);
}

Napi::Value MidiRouteEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "events must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input_array = info[0].As<Napi::Array>();
  const uint32_t length = input_array.Length();
  std::vector<SonareMidiEventPod> input(length);
  for (uint32_t i = 0; i < length; ++i) {
    Napi::Value value = input_array.Get(i);
    if (!value.IsObject()) {
      Napi::TypeError::New(env, "events must contain objects").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object event = value.As<Napi::Object>();
    input[i] = MidiEventFromObject(event);
  }

  SonareMidiRouteConfig config{-1, -1, -1, 1};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object object = info[1].As<Napi::Object>();
    if (object.Has("filterGroup") && !object.Get("filterGroup").IsNull()) {
      config.filter_group = object.Get("filterGroup").As<Napi::Number>().Int32Value();
    }
    if (object.Has("filterChannel") && !object.Get("filterChannel").IsNull()) {
      config.filter_channel = object.Get("filterChannel").As<Napi::Number>().Int32Value();
    }
    if (object.Has("remapChannel") && !object.Get("remapChannel").IsNull()) {
      config.remap_channel = object.Get("remapChannel").As<Napi::Number>().Int32Value();
    }
    if (object.Has("thru")) {
      config.thru = object.Get("thru").ToBoolean() ? 1 : 0;
    }
  }

  std::vector<SonareMidiEventPod> output(input.size());
  size_t output_count = 0;
  int overflowed = 0;
  uint32_t overflow_count = 0;
  const SonareError err =
      sonare_midi_route_events(input.empty() ? nullptr : input.data(), input.size(), &config,
                               output.empty() ? nullptr : output.data(), output.size(),
                               &output_count, &overflowed, &overflow_count);
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI route arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  Napi::Array events = Napi::Array::New(env, output_count);
  for (size_t i = 0; i < output_count; ++i) {
    events.Set(i, MidiEventToObject(env, output[i]));
  }
  result.Set("events", events);
  result.Set("overflowed", overflowed != 0);
  result.Set("overflowCount", Napi::Number::New(env, overflow_count));
  return result;
}

// NativeSynth preset catalog: '\n'-joined program-lifetime string from the C
// ABI, split into a JS string[] like masteringInsertNames.
Napi::Value SynthPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const char* joined = sonare_synth_preset_names();
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') return out;
  std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.Set(index++, Napi::String::New(env, names.substr(start)));
      break;
    }
    out.Set(index++, Napi::String::New(env, names.substr(start, end - start)));
    start = end + 1;
  }
  return out;
}

Napi::Array SplitJoinedNames(Napi::Env env, const char* joined) {
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') return out;
  std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.Set(index++, Napi::String::New(env, names.substr(start)));
      break;
    }
    out.Set(index++, Napi::String::New(env, names.substr(start, end - start)));
    start = end + 1;
  }
  return out;
}

// Fetches a named catalog preset as a SynthPatch object (the preset name plus
// its wrapper-section values), so hosts can inspect and tweak before binding.
// A "va:" routing prefix is accepted; unknown names throw.
Napi::Value SynthPresetPatch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "synthPresetPatch expects a preset name string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string name = info[0].As<Napi::String>().Utf8Value();
  if (name.rfind("va:", 0) == 0) name = name.substr(3);
  SonareSynthPatch patch{};
  const SonareError err = sonare_synth_preset_patch(name.c_str(), &patch);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return sonare_node::SynthPatchToObject(env, patch);
}

Napi::Value SynthEnumTables(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object out = Napi::Object::New(env);
  out.Set("engineModes",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_ENGINE_MODE)));
  out.Set("waveforms",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_OSC_WAVEFORM)));
  out.Set("filterModels",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_MODEL)));
  out.Set("filterOutputs",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_OUTPUT)));
  out.Set("bodyTypes", SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_BODY_TYPE)));
  out.Set("modSources",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_SOURCE)));
  out.Set("modDestinations",
          SplitJoinedNames(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_DESTINATION)));
  return out;
}

Napi::Value SynthPatchRoundTrip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareSynthPatch patch{};
  if (!sonare_node::ReadSynthPatch(env, info.Length() > 0 ? info[0] : env.Undefined(), &patch)) {
    return env.Undefined();
  }
  return sonare_node::SynthPatchToObject(env, patch);
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  SonareWrap::Init(env, exports);
  RealtimeEngineWrap::Init(env, exports);
  ProjectWrap::Init(env, exports);
  exports.Set("engineAbiVersion", Napi::Function::New(env, EngineAbiVersion, "engineAbiVersion"));
  exports.Set("voiceChangerAbiVersion",
              Napi::Function::New(env, VoiceChangerAbiVersion, "voiceChangerAbiVersion"));
  exports.Set("projectAbiVersion",
              Napi::Function::New(env, ProjectAbiVersion, "projectAbiVersion"));
  exports.Set("synthPresetNames", Napi::Function::New(env, SynthPresetNames, "synthPresetNames"));
  exports.Set("synthPresetPatch", Napi::Function::New(env, SynthPresetPatch, "synthPresetPatch"));
  exports.Set("_synthEnumTables", Napi::Function::New(env, SynthEnumTables, "_synthEnumTables"));
  exports.Set("_synthPatchRoundTrip",
              Napi::Function::New(env, SynthPatchRoundTrip, "_synthPatchRoundTrip"));
  exports.Set("midiGmInstrumentName",
              Napi::Function::New(env, MidiGmInstrumentName, "midiGmInstrumentName"));
  exports.Set("midiGmProgramForName",
              Napi::Function::New(env, MidiGmProgramForName, "midiGmProgramForName"));
  exports.Set("midiGmFamilyName", Napi::Function::New(env, MidiGmFamilyName, "midiGmFamilyName"));
  exports.Set("midiGmFamilyFirstProgram",
              Napi::Function::New(env, MidiGmFamilyFirstProgram, "midiGmFamilyFirstProgram"));
  exports.Set("midiGm2InstrumentName",
              Napi::Function::New(env, MidiGm2InstrumentName, "midiGm2InstrumentName"));
  exports.Set("midiGmDrumName", Napi::Function::New(env, MidiGmDrumName, "midiGmDrumName"));
  exports.Set("midiGmDrumNoteForName",
              Napi::Function::New(env, MidiGmDrumNoteForName, "midiGmDrumNoteForName"));
  exports.Set("midiGm2DrumSetName",
              Napi::Function::New(env, MidiGm2DrumSetName, "midiGm2DrumSetName"));
  exports.Set("midiGm2DrumName", Napi::Function::New(env, MidiGm2DrumName, "midiGm2DrumName"));
  exports.Set("midiCcName", Napi::Function::New(env, MidiCcName, "midiCcName"));
  exports.Set("midiCcIndexForName",
              Napi::Function::New(env, MidiCcIndexForName, "midiCcIndexForName"));
  exports.Set("midiPerNoteControllerName",
              Napi::Function::New(env, MidiPerNoteControllerName, "midiPerNoteControllerName"));
  exports.Set("midiBankProgram", Napi::Function::New(env, MidiBankProgram, "midiBankProgram"));
  exports.Set("midiCcLearn", Napi::Function::New(env, MidiCcLearn, "midiCcLearn"));
  exports.Set("midiCcToBreakpoint",
              Napi::Function::New(env, MidiCcToBreakpoint, "midiCcToBreakpoint"));
  exports.Set("midiParamToCc", Napi::Function::New(env, MidiParamToCc, "midiParamToCc"));
  exports.Set("midiRouteEvents", Napi::Function::New(env, MidiRouteEvents, "midiRouteEvents"));
  return exports;
}

NODE_API_MODULE(sonare, Init)
