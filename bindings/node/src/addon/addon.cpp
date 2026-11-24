#include <napi.h>
#include <sonare/sonare_c.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "c_api/midi_input_domains.h"
#include "sonare_wrap.h"
#include "sonare_wrap_clip_page_provider.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_midi_cc.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_playback.h"
#include "sonare_wrap_polyphony.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_project_timeline.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_transcribe.h"
#include "sonare_wrap_utils.h"
#include "vocal_edit.h"

namespace {

Napi::Value EngineAbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(info.Env(), sonare_engine_abi_version());
  SONARE_NODE_CATCH(env)
}

Napi::Value VoiceChangerAbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(info.Env(), sonare_voice_changer_abi_version());
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectAbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(info.Env(), sonare_project_abi_version());
  SONARE_NODE_CATCH(env)
}

Napi::Value NullableString(Napi::Env env, const char* value) {
  return value != nullptr ? Napi::String::New(env, value) : env.Null();
}

Napi::Value MidiGmInstrumentName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int program = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "program", sonare_c_detail::kMidiDataByteDomain,
                                      &program))
    return env.Undefined();
  return NullableString(env, sonare_midi_gm_instrument_name(program));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGmProgramForName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::string name;
  if (!sonare_node::RequiredStringValue(env, info[0], "name", &name)) return env.Undefined();
  return Napi::Number::New(env, sonare_midi_gm_program_for_name(name.c_str()));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGmFamilyName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int family = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "family",
                                      sonare_c_detail::kMidiFamilyIndexDomain, &family))
    return env.Undefined();
  return NullableString(env, sonare_midi_gm_family_name(family));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGmFamilyFirstProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int family = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "family",
                                      sonare_c_detail::kMidiFamilyIndexDomain, &family))
    return env.Undefined();
  return Napi::Number::New(env, sonare_midi_gm_family_first_program(family));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGm2InstrumentName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int bank_lsb = 0;
  int program = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "bankLsb", sonare_c_detail::kMidiDataByteDomain,
                                      &bank_lsb) ||
      !sonare_node::RequiredDomainArg(env, info, 1, "program", sonare_c_detail::kMidiDataByteDomain,
                                      &program)) {
    return env.Undefined();
  }
  return NullableString(env, sonare_midi_gm2_instrument_name(bank_lsb, program));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGmDrumName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int note = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "note", sonare_c_detail::kMidiDataByteDomain,
                                      &note))
    return env.Undefined();
  return NullableString(env, sonare_midi_gm_drum_name(note));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGmDrumNoteForName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::string name;
  if (!sonare_node::RequiredStringValue(env, info[0], "name", &name)) return env.Undefined();
  return Napi::Number::New(env, sonare_midi_gm_drum_note_for_name(name.c_str()));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGm2DrumSetName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int bank_lsb = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "bankLsb", sonare_c_detail::kMidiDataByteDomain,
                                      &bank_lsb))
    return env.Undefined();
  return NullableString(env, sonare_midi_gm2_drum_set_name(bank_lsb));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiGm2DrumName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int bank_lsb = 0;
  int note = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "bankLsb", sonare_c_detail::kMidiDataByteDomain,
                                      &bank_lsb) ||
      !sonare_node::RequiredDomainArg(env, info, 1, "note", sonare_c_detail::kMidiDataByteDomain,
                                      &note)) {
    return env.Undefined();
  }
  return NullableString(env, sonare_midi_gm2_drum_name(bank_lsb, note));
  SONARE_NODE_CATCH(env)
}

// The MIDI 1.0 <-> 2.0 value scalers: each argument is read at its C parameter's width, refusing a
// value the narrowing cast would wrap (300 reads as 44 in a byte), and the C call owns the MIDI
// range.
bool ReadScaleInput(Napi::Env env, const Napi::Value& value, const char* name, uint8_t* out) {
  return sonare_node::RequiredMidiByteValue(env, value, name, out);
}

bool ReadScaleInput(Napi::Env env, const Napi::Value& value, const char* name, uint16_t* out) {
  return sonare_node::RequiredUint16Value(env, value, name, out);
}

bool ReadScaleInput(Napi::Env env, const Napi::Value& value, const char* name, uint32_t* out) {
  return sonare_node::RequiredUint32Value(env, value, name, out);
}

template <typename In, typename Out>
Napi::Value MidiScale(const Napi::CallbackInfo& info, const char* name,
                      SonareError (*scale)(In, Out*)) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  In value{};
  if (!ReadScaleInput(env, info[0], name, &value)) return env.Undefined();
  Out out{};
  const SonareError err = scale(value, &out);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiVelocityToMidi2(const Napi::CallbackInfo& info) {
  return MidiScale(info, "velocity", sonare_midi_velocity_to_midi2);
}

Napi::Value MidiNoteOnVelocityToMidi2(const Napi::CallbackInfo& info) {
  return MidiScale(info, "velocity", sonare_midi_note_on_velocity_to_midi2);
}

Napi::Value Midi2VelocityToMidi1(const Napi::CallbackInfo& info) {
  return MidiScale(info, "velocity16", sonare_midi2_velocity_to_midi1);
}

Napi::Value Midi2NoteOnVelocityToMidi1(const Napi::CallbackInfo& info) {
  return MidiScale(info, "velocity16", sonare_midi2_note_on_velocity_to_midi1);
}

Napi::Value MidiControlToMidi2(const Napi::CallbackInfo& info) {
  return MidiScale(info, "value", sonare_midi_control_to_midi2);
}

Napi::Value MidiControl14ToMidi2(const Napi::CallbackInfo& info) {
  return MidiScale(info, "value14", sonare_midi_control14_to_midi2);
}

Napi::Value Midi2ControlToMidi1(const Napi::CallbackInfo& info) {
  return MidiScale(info, "value32", sonare_midi2_control_to_midi1);
}

Napi::Value Midi2ControlToMidi1Control14(const Napi::CallbackInfo& info) {
  return MidiScale(info, "value32", sonare_midi2_control_to_midi1_control14);
}

Napi::Value MidiPitchBendToMidi2(const Napi::CallbackInfo& info) {
  return MidiScale(info, "bend14", sonare_midi_pitch_bend_to_midi2);
}

Napi::Value Midi2PitchBendToMidi1(const Napi::CallbackInfo& info) {
  return MidiScale(info, "bend32", sonare_midi2_pitch_bend_to_midi1);
}

Napi::Value MidiCcName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int controller = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "controller",
                                      sonare_c_detail::kMidiDataByteDomain, &controller))
    return env.Undefined();
  return NullableString(env, sonare_midi_cc_name(controller));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiCcIndexForName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::string name;
  if (!sonare_node::RequiredStringValue(env, info[0], "name", &name)) return env.Undefined();
  return Napi::Number::New(env, sonare_midi_cc_index_for_name(name.c_str()));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiPerNoteControllerName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int index = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 0, "index",
                                      sonare_c_detail::kMidiPerNoteControllerIndexDomain, &index))
    return env.Undefined();
  return NullableString(env, sonare_midi_per_note_controller_name(index));
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiBankProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // group/channel are uint8_t C-ABI fields, read against the domains the C ABI enforces.
  double ppq = 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  int bank_msb = 0;
  int bank_lsb = 0;
  int program = 0;
  if (!sonare_node::RequiredDoubleArg(env, info, 0, "ppq", &ppq) ||
      !sonare_node::RequiredDomainArg(env, info, 1, "group", sonare_c_detail::kMidiUmpGroupDomain,
                                      &group) ||
      !sonare_node::RequiredDomainArg(env, info, 2, "channel", sonare_c_detail::kMidiChannelDomain,
                                      &channel) ||
      !sonare_node::RequiredIntArg(env, info, 3, "bankMsb", &bank_msb) ||
      !sonare_node::RequiredIntArg(env, info, 4, "bankLsb", &bank_lsb) ||
      !sonare_node::RequiredIntArg(env, info, 5, "program", &program)) {
    return env.Undefined();
  }
  SonareMidiEventPod events[3]{};
  size_t count = 0;
  const SonareError err =
      sonare_midi_bank_program(ppq, group, channel, bank_msb, bank_lsb, program, events, 3, &count);
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI bank/program arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    Napi::Object event = Napi::Object::New(env);
    event.Set("ppq", Napi::Number::New(env, events[i].ppq));
    event.Set("word0", Napi::Number::New(env, events[i].word0));
    event.Set("word1", Napi::Number::New(env, events[i].word1));
    out.Set(i, event);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

// Required fields are validated explicitly so a missing/wrong-typed one raises a
// single clear TypeError and the caller stops; optional fields go through the
// shared *Property readers so an explicit `undefined` reads as an omitted field.
bool MidiEventFromObject(Napi::Env env, Napi::Object event, SonareMidiEventPod* out) {
  const Napi::Value ppq = event.Get("ppq");
  const Napi::Value word0 = event.Get("word0");
  if (!ppq.IsNumber() || !word0.IsNumber()) {
    Napi::TypeError::New(env, "MIDI event requires numeric ppq and word0")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = SonareMidiEventPod{};
  out->ppq = ppq.As<Napi::Number>().DoubleValue();
  out->word0 = sonare_node::node_narrow_word(env, word0, "word0");
  out->word1 = sonare_node::WordProperty(event, "word1", 0);
  return !env.IsExceptionPending();
}

bool CcBindingsFromArray(Napi::Env env, Napi::Array array, std::vector<SonareMidiCcBinding>* out) {
  const uint32_t array_count = sonare_node::node_list_length(array, "bindings");
  out->assign(array_count, SonareMidiCcBinding{});
  for (uint32_t i = 0; i < array_count; ++i) {
    const Napi::Value item = array.Get(i);
    if (!item.IsObject()) {
      Napi::TypeError::New(env, "MIDI CC bindings must be objects").ThrowAsJavaScriptException();
      return false;
    }
    if (!sonare_node::CcBindingFromObject(env, item.As<Napi::Object>(), &(*out)[i],
                                          "bindings[" + std::to_string(i) + "]"))
      return false;
  }
  return true;
}

Napi::Value MidiCcLearn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "events must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "paramId must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array array = info[0].As<Napi::Array>();
  const uint32_t array_count = sonare_node::node_list_length(array, "events");
  std::vector<SonareMidiEventPod> events(array_count);
  for (uint32_t i = 0; i < array_count; ++i) {
    const Napi::Value item = array.Get(i);
    if (!item.IsObject() || !MidiEventFromObject(env, item.As<Napi::Object>(), &events[i])) {
      if (!env.IsExceptionPending()) {
        Napi::TypeError::New(env, "events must contain objects").ThrowAsJavaScriptException();
      }
      return env.Undefined();
    }
  }
  uint8_t min_movement = 0;
  uint32_t param_id = 0;
  if (!sonare_node::OptionalDomainArg(env, info, 4, "minMovement", uint8_t{0},
                                      sonare_c_detail::kMidiDataByteDomain, &min_movement) ||
      !sonare_node::RequiredDomainArg(env, info, 1, "paramId",
                                      sonare_c_detail::kMidiTargetParamIdDomain, &param_id)) {
    return env.Undefined();
  }
  float min_value = 0.0f;
  float max_value = 1.0f;
  if (!sonare_node::OptionalFloatArg(env, info, 2, "minValue", 0.0f, &min_value) ||
      !sonare_node::OptionalFloatArg(env, info, 3, "maxValue", 1.0f, &max_value)) {
    return env.Undefined();
  }
  SonareMidiCcBinding learned{};
  const SonareError err =
      sonare_midi_cc_learn(events.empty() ? nullptr : events.data(), events.size(), param_id,
                           min_value, max_value, min_movement, &learned);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    Napi::RangeError::New(env, "invalid MIDI CC learn arguments").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return sonare_node::CcBindingToObject(env, learned);
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiCcToBreakpoint(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "bindings must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsObject()) {
    Napi::TypeError::New(env, "event must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareMidiCcBinding> bindings;
  if (!CcBindingsFromArray(env, info[0].As<Napi::Array>(), &bindings)) return env.Undefined();
  SonareMidiEventPod event{};
  if (!MidiEventFromObject(env, info[1].As<Napi::Object>(), &event)) return env.Undefined();
  SonareAutomationPoint point{};
  const SonareError err = sonare_midi_cc_to_breakpoint(bindings.empty() ? nullptr : bindings.data(),
                                                       bindings.size(), &event, &point);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("ppq", Napi::Number::New(env, point.ppq));
  out.Set("value", Napi::Number::New(env, point.value));
  out.Set("curveToNext", Napi::Number::New(env, point.curve_to_next));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiParamToCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "bindings must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "paramId must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "value must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "group must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<SonareMidiCcBinding> bindings;
  if (!CcBindingsFromArray(env, info[0].As<Napi::Array>(), &bindings)) return env.Undefined();
  uint8_t group = 0;
  uint32_t param_id = 0;
  if (!sonare_node::RequiredDomainArg(env, info, 3, "group", sonare_c_detail::kMidiUmpGroupDomain,
                                      &group) ||
      !sonare_node::RequiredDomainArg(env, info, 1, "paramId",
                                      sonare_c_detail::kMidiTargetParamIdDomain, &param_id)) {
    return env.Undefined();
  }
  double ppq = 0.0;
  if (!sonare_node::OptionalDoubleArg(env, info, 4, "ppq", 0.0, &ppq)) return env.Undefined();
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      bindings.empty() ? nullptr : bindings.data(), bindings.size(), param_id,
      sonare_node::node_narrow_float(env, info[2], "unitValue"), group, ppq, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return env.Null();
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return sonare_node::MidiEventToObject(env, event);
  SONARE_NODE_CATCH(env)
}

Napi::Value MidiRouteEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "events must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input_array = info[0].As<Napi::Array>();
  const uint32_t length = sonare_node::node_list_length(input_array, "events");
  std::vector<SonareMidiEventPod> input(length);
  std::vector<std::vector<uint8_t>> sysex_payloads(length);
  for (uint32_t i = 0; i < length; ++i) {
    Napi::Value value = input_array.Get(i);
    if (!value.IsObject()) {
      Napi::TypeError::New(env, "events must contain objects").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object event = value.As<Napi::Object>();
    if (!MidiEventFromObject(env, event, &input[i])) return env.Undefined();
    const Napi::Value sysex_value = event.Get("sysex");
    if (!sysex_value.IsUndefined() && !sysex_value.IsNull()) {
      Napi::Uint8Array sysex;
      const std::string subject = "events[" + std::to_string(i) + "].sysex";
      if (!sonare_node::RequiredUint8ArrayValue(env, sysex_value, subject, &sysex)) {
        return env.Undefined();
      }
      const size_t size = sysex.ElementLength();
      const uint8_t* bytes = sonare_node::ElementData(sysex);
      if (size > 0) sysex_payloads[i].assign(bytes, bytes + size);
      input[i].sysex = sonare_node::ElementData(sysex_payloads[i]);
      input[i].sysex_size = size;
    }
  }

  // An omitted, null, or explicitly `undefined` field must land on the init defaults (any
  // group/channel, no remap, pass-through).
  SonareMidiRouteConfig config;
  sonare_midi_route_config_init(&config);
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object object = info[1].As<Napi::Object>();
    config.filter_group =
        sonare_node::IntInRangeOrAbsentProperty(object, "filterGroup", 0, 15, config.filter_group);
    config.filter_channel = sonare_node::IntInRangeOrAbsentProperty(object, "filterChannel", 0, 15,
                                                                    config.filter_channel);
    config.remap_channel = sonare_node::IntInRangeOrAbsentProperty(object, "remapChannel", 0, 15,
                                                                   config.remap_channel);
    config.thru = sonare_node::BoolProperty(object, "thru", config.thru != 0) ? 1 : 0;
    if (env.IsExceptionPending()) return env.Undefined();
  }

  std::vector<SonareMidiEventPod> output(input.size());
  size_t output_count = 0;
  int overflowed = 0;
  size_t overflow_count = 0;
  const SonareError err =
      sonare_midi_route_events(input.empty() ? nullptr : input.data(), input.size(), &config,
                               output.empty() ? nullptr : output.data(), output.size(),
                               &output_count, &overflowed, &overflow_count);
  sonare_node::ThrowIfError(env, err);
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Object result = Napi::Object::New(env);
  Napi::Array events = Napi::Array::New(env, output_count);
  for (size_t i = 0; i < output_count; ++i) {
    Napi::Object event = sonare_node::MidiEventToObject(env, output[i]);
    if (output[i].sysex_size > 0) {
      if (output[i].sysex == nullptr || output[i].sysex_size > sonare_node::kNodeListBudget) {
        sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                             "MIDI route result has invalid SysEx storage");
        return env.Undefined();
      }
      Napi::Uint8Array sysex = Napi::Uint8Array::New(env, output[i].sysex_size);
      std::memcpy(sonare_node::ElementData(sysex), output[i].sysex, output[i].sysex_size);
      event.Set("sysex", sysex);
    } else if (output[i].sysex != nullptr) {
      sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                           "MIDI route result has empty SysEx storage");
      return env.Undefined();
    }
    events.Set(i, event);
  }
  result.Set("events", events);
  result.Set("overflowed", overflowed != 0);
  result.Set("overflowCount", Napi::Number::New(env, overflow_count));
  return result;
  SONARE_NODE_CATCH(env)
}

// NativeSynth preset catalog: '\n'-joined program-lifetime string from the C
// ABI, split into a JS string[] like masteringInsertNames.
Napi::Value SynthPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return sonare_node::JoinedNamesToArray(info.Env(), sonare_synth_preset_names(),
                                         "synth preset names");
  SONARE_NODE_CATCH(env)
}

// GS rhythm-set name for a rhythm part's program, or null where no set sits.
Napi::Value SynthGsDrumKitName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int program = 0;
  if (!sonare_node::RequiredIntArg(env, info, 0, "program", &program)) return env.Undefined();
  return NullableString(env, sonare_synth_gs_drum_kit_name(program));
  SONARE_NODE_CATCH(env)
}

// Whether a GS rhythm set is voiced apart from Standard: 1 / 0 / -1.
Napi::Value SynthGsDrumKitIsVoicedApart(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int program = 0;
  if (!sonare_node::RequiredIntArg(env, info, 0, "program", &program)) return env.Undefined();
  return Napi::Number::New(env, sonare_synth_gs_drum_kit_is_voiced_apart(program));
  SONARE_NODE_CATCH(env)
}

// Whether a melodic GS variation bank is voiced apart from its capital tone.
Napi::Value SynthGsVariationIsVoicedApart(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int bank = 0;
  int program = 0;
  if (!sonare_node::RequiredIntArg(env, info, 0, "bank", &bank) ||
      !sonare_node::RequiredIntArg(env, info, 1, "program", &program)) {
    return env.Undefined();
  }
  return Napi::Number::New(env, sonare_synth_gs_variation_is_voiced_apart(bank, program));
  SONARE_NODE_CATCH(env)
}

// Controller-profile preset catalog, split from the C ABI's '\n'-joined
// program-lifetime string like SynthPresetNames.
Napi::Value ControllerProfileNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return sonare_node::JoinedNamesToArray(info.Env(), sonare_controller_profile_names(),
                                         "controller profile names");
  SONARE_NODE_CATCH(env)
}

// Fetches a named catalog preset as a SynthPatch object (the preset name plus
// its wrapper-section values), so hosts can inspect and tweak before binding.
// A "va:" routing prefix is accepted; unknown names throw.
Napi::Value SynthPresetPatch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "preset must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string name = sonare_node::node_narrow_string(env, info[0], "preset");
  if (name.rfind("va:", 0) == 0) name = name.substr(3);
  SonareSynthPatch patch{};
  const SonareError err = sonare_synth_preset_patch(name.c_str(), &patch);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return sonare_node::SynthPatchToObject(env, patch);
  SONARE_NODE_CATCH(env)
}

Napi::Value SynthEnumTables(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Object out = Napi::Object::New(env);
  out.Set("engineModes",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_ENGINE_MODE), "synth enum names"));
  out.Set("waveforms",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_OSC_WAVEFORM), "synth enum names"));
  out.Set("builtinWaveforms", sonare_node::JoinedNamesToArray(
                                  env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM),
                                  "synth enum names"));
  out.Set("filterModels",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_MODEL), "synth enum names"));
  out.Set("filterOutputs",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_OUTPUT), "synth enum names"));
  out.Set("bodyTypes",
          sonare_node::JoinedNamesToArray(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_BODY_TYPE),
                                          "synth enum names"));
  out.Set("modSources",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_SOURCE), "synth enum names"));
  out.Set("modDestinations",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_DESTINATION), "synth enum names"));
  out.Set("controllerInputs", sonare_node::JoinedNamesToArray(
                                  env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_INPUT),
                                  "synth enum names"));
  out.Set("controllerAxes",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_AXIS), "synth enum names"));
  out.Set("articulations",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_ARTICULATION), "synth enum names"));
  out.Set("mpeDimensions",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_MPE_DIMENSION), "synth enum names"));
  out.Set("noteTrackings",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_NOTE_TRACKING), "synth enum names"));
  out.Set("sampleLoopModes", sonare_node::JoinedNamesToArray(
                                 env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_SAMPLE_LOOP_MODE),
                                 "synth enum names"));
  out.Set("sampleKeyTracks", sonare_node::JoinedNamesToArray(
                                 env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_SAMPLE_KEY_TRACK),
                                 "synth enum names"));
  out.Set("retriggers",
          sonare_node::JoinedNamesToArray(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_RETRIGGER),
                                          "synth enum names"));
  out.Set("lfoShapes",
          sonare_node::JoinedNamesToArray(env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_LFO_SHAPE),
                                          "synth enum names"));
  out.Set("partRigModes",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_PART_RIG_MODE), "synth enum names"));
  out.Set("sampleDescLoopModes",
          sonare_node::JoinedNamesToArray(
              env, sonare_synth_enum_names(SONARE_SYNTH_ENUM_SAMPLE_DESC_LOOP_MODE),
              "synth enum names"));
  if (env.IsExceptionPending()) return env.Undefined();
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SynthPatchRoundTrip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  sonare_node::SynthPatchStorage storage;
  if (!sonare_node::ReadSynthPatch(env, info.Length() > 0 ? info[0] : env.Undefined(), &storage)) {
    return env.Undefined();
  }
  return sonare_node::SynthPatchToObject(env, storage.patch);
  SONARE_NODE_CATCH(env)
}

// Engine-section field descriptors for one engine mode (name or ordinal), as the
// C ABI's JSON text; the TS wrapper parses it.
Napi::Value SynthEngineParamInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int mode = 0;
  if (!sonare_node::SynthEnumValue(env, info.Length() > 0 ? info[0] : env.Undefined(),
                                   sonare_node::kSynthEngineModes, SONARE_SYNTH_ENGINE_MODE_COUNT,
                                   "synth engine mode", &mode)) {
    return env.Undefined();
  }
  char* json = nullptr;
  const SonareError err = sonare_synth_engine_param_info(mode, &json);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "synth engine param info: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

// Descriptors for every numeric SynthPatch wrapper field, as JSON text.
Napi::Value SynthPatchParamInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  char* json = nullptr;
  const SonareError err = sonare_synth_patch_param_info(&json);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "synth patch param info: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  sonare_node::InitVocalEdit(env, exports);
  SonareWrap::Init(env, exports);
  RealtimeEngineWrap::Init(env, exports);
  ProjectWrap::Init(env, exports);
  SampleBankWrap::Init(env, exports);
  ClipPageProviderWrap::Init(env, exports);
  ProjectTimelineWrap::Init(env, exports);
  sonare_node::PolyphonicAnalysisWrap::Init(env, exports);
  HrtfSetWrap::Init(env, exports);
  PlaybackRendererWrap::Init(env, exports);
  PlaybackLoudnessMeterWrap::Init(env, exports);
  exports.Set("renderPlayback",
              Napi::Function::New(env, &sonare_node::RenderPlayback, "renderPlayback"));
  exports.Set("engineAbiVersion", Napi::Function::New(env, EngineAbiVersion, "engineAbiVersion"));
  exports.Set("voiceChangerAbiVersion",
              Napi::Function::New(env, VoiceChangerAbiVersion, "voiceChangerAbiVersion"));
  exports.Set("projectAbiVersion",
              Napi::Function::New(env, ProjectAbiVersion, "projectAbiVersion"));
  exports.Set("synthPresetNames", Napi::Function::New(env, SynthPresetNames, "synthPresetNames"));
  exports.Set("synthPresetPatch", Napi::Function::New(env, SynthPresetPatch, "synthPresetPatch"));
  exports.Set("synthEngineParamInfo",
              Napi::Function::New(env, SynthEngineParamInfo, "synthEngineParamInfo"));
  exports.Set("synthPatchParamInfo",
              Napi::Function::New(env, SynthPatchParamInfo, "synthPatchParamInfo"));
  exports.Set("synthGsDrumKitName",
              Napi::Function::New(env, SynthGsDrumKitName, "synthGsDrumKitName"));
  exports.Set("synthGsDrumKitIsVoicedApart",
              Napi::Function::New(env, SynthGsDrumKitIsVoicedApart, "synthGsDrumKitIsVoicedApart"));
  exports.Set(
      "synthGsVariationIsVoicedApart",
      Napi::Function::New(env, SynthGsVariationIsVoicedApart, "synthGsVariationIsVoicedApart"));
  exports.Set("controllerProfileNames",
              Napi::Function::New(env, ControllerProfileNames, "controllerProfileNames"));
  exports.Set("_synthEnumTables", Napi::Function::New(env, SynthEnumTables, "_synthEnumTables"));
  exports.Set("_synthPatchRoundTrip",
              Napi::Function::New(env, SynthPatchRoundTrip, "_synthPatchRoundTrip"));
  exports.Set("midiVelocityToMidi2",
              Napi::Function::New(env, MidiVelocityToMidi2, "midiVelocityToMidi2"));
  exports.Set("midiNoteOnVelocityToMidi2",
              Napi::Function::New(env, MidiNoteOnVelocityToMidi2, "midiNoteOnVelocityToMidi2"));
  exports.Set("midi2VelocityToMidi1",
              Napi::Function::New(env, Midi2VelocityToMidi1, "midi2VelocityToMidi1"));
  exports.Set("midi2NoteOnVelocityToMidi1",
              Napi::Function::New(env, Midi2NoteOnVelocityToMidi1, "midi2NoteOnVelocityToMidi1"));
  exports.Set("midiControlToMidi2",
              Napi::Function::New(env, MidiControlToMidi2, "midiControlToMidi2"));
  exports.Set("midiControl14ToMidi2",
              Napi::Function::New(env, MidiControl14ToMidi2, "midiControl14ToMidi2"));
  exports.Set("midi2ControlToMidi1",
              Napi::Function::New(env, Midi2ControlToMidi1, "midi2ControlToMidi1"));
  exports.Set("midi2ControlToMidi1Control14", Napi::Function::New(env, Midi2ControlToMidi1Control14,
                                                                  "midi2ControlToMidi1Control14"));
  exports.Set("midiPitchBendToMidi2",
              Napi::Function::New(env, MidiPitchBendToMidi2, "midiPitchBendToMidi2"));
  exports.Set("midi2PitchBendToMidi1",
              Napi::Function::New(env, Midi2PitchBendToMidi1, "midi2PitchBendToMidi1"));
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
  exports.Set("transcribe", Napi::Function::New(env, &sonare_node::Transcribe, "transcribe"));
  exports.Set("alignTakeToReference",
              Napi::Function::New(env, &sonare_node::AlignTakeToReference, "alignTakeToReference"));
  exports.Set(
      "chordSymbolFromAnalysis",
      Napi::Function::New(env, &sonare_node::ChordSymbolFromAnalysis, "chordSymbolFromAnalysis"));
  exports.Set("keyModeFromAnalysis",
              Napi::Function::New(env, &sonare_node::KeyModeFromAnalysis, "keyModeFromAnalysis"));
  return exports;
}

NODE_API_MODULE(sonare, Init)
