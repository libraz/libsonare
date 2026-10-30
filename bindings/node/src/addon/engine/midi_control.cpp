// Node engine: MIDI CC binding, controller profile, articulation and MIDI FX methods.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/audio.h"
#include "engine/common.h"
#include "engine/realtime_engine.h"
#include "midi/live_push_args.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

// Controller-profile spellings: the C ABI's own tables.
constexpr const auto& kControllerInputs = sonare::c_api::synth_names::kControllerInputs;
constexpr const auto& kControllerAxes = sonare::c_api::synth_names::kControllerAxes;

// Articulation, MPE dimension and note-tracking spellings: the C ABI's own tables.
constexpr const auto& kArticulations = sonare::c_api::synth_names::kArticulations;
constexpr const auto& kMpeDimensions = sonare::c_api::synth_names::kMpeDimensions;
constexpr const auto& kNoteTrackings = sonare::c_api::synth_names::kNoteTrackings;

bool ReadControllerBinding(Napi::Env env, const Napi::Value& value, SonareControllerBinding* out) {
  if (!value.IsObject()) {
    Napi::TypeError::New(env, "controller binding must be an object").ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Object obj = value.As<Napi::Object>();
  // Both are required rather than defaulted: an omitted input would bind
  // control-change CC0, which the C ABI accepts as a binding that listens to
  // the wrong gesture.
  if (!sonare_node::SynthFieldPresent(obj, "input") ||
      !sonare_node::SynthFieldPresent(obj, "axis")) {
    Napi::TypeError::New(env, "controller binding requires an input and an axis")
        .ThrowAsJavaScriptException();
    return false;
  }
  int input = 0;
  int axis = 0;
  if (!sonare_node::SynthEnumProperty(env, obj, "input", kControllerInputs,
                                      SONARE_CONTROLLER_INPUT_COUNT, "controller input", &input) ||
      !sonare_node::SynthEnumProperty(env, obj, "axis", kControllerAxes,
                                      SONARE_CONTROLLER_AXIS_COUNT, "controller axis", &axis)) {
    return false;
  }
  out->input = static_cast<uint8_t>(input);
  out->axis = static_cast<uint8_t>(axis);
  out->index = MidiByteProperty(env, obj, "index", 0);
  out->lo = sonare_node::FiniteFloatProperty(obj, "lo", 0.0f);
  out->hi = sonare_node::FiniteFloatProperty(obj, "hi", 1.0f);
  out->curve = sonare_node::FiniteFloatProperty(obj, "curve", 1.0f);
  return !env.IsExceptionPending();
}

}  // namespace

Napi::Value RealtimeEngineWrap::BindMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t channel = 0;
  uint8_t controller = 0;
  uint32_t param_id = 0;
  float min_value = 0.0f;
  float max_value = 1.0f;
  if (!OptionalMidiByteArg(env, info, 0, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 1, "controller", 0, &controller) ||
      !RequiredUint32Arg(env, info, 2, "paramId", &param_id) ||
      !OptionalFloatArg(env, info, 3, "minValue", 0.0f, &min_value) ||
      !OptionalFloatArg(env, info, 4, "maxValue", 1.0f, &max_value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_bind_midi_cc(engine_, channel, controller, param_id, min_value,
                                               max_value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::BindMidiCcBinding(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected a MIDI CC binding object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object object = info[0].As<Napi::Object>();
  const Napi::Value cc_number = object.Get("ccNumber");
  const Napi::Value param_id = object.Get("paramId");
  if (!cc_number.IsNumber() || !param_id.IsNumber()) {
    Napi::TypeError::New(env, "MIDI CC binding requires numeric ccNumber and paramId")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareMidiCcBinding binding{};
  // Every byte-wide field goes through the MidiByte readers: the C-ABI struct
  // stores them as uint8_t, so a plain Uint32Value() would let 256 arrive as 0
  // and 271 as 15 — inside the range the C ABI's own check accepts.
  if (!RequiredMidiByteValue(env, cc_number, "ccNumber", &binding.cc_number)) {
    return env.Undefined();
  }
  // `channel` keeps its "any channel" sentinel for an omitted, null, or
  // undefined value.
  binding.channel = MidiByteProperty(env, object, "channel", 0xffu);
  binding.kind = MidiByteProperty(env, object, "kind", 0u);
  binding.cc_lsb_number = MidiByteProperty(env, object, "ccLsbNumber", 0u);
  binding.selector_msb = MidiByteProperty(env, object, "selectorMsb", 0u);
  binding.selector_lsb = MidiByteProperty(env, object, "selectorLsb", 0u);
  if (env.IsExceptionPending()) return env.Undefined();
  binding.param_id = node_narrow_uint32(env, param_id, "paramId");
  binding.min_value = FloatProperty(object, "minValue", 0.0f);
  binding.max_value = FloatProperty(object, "maxValue", 1.0f);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_bind_midi_cc_binding(engine_, &binding));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiCcBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_clear_midi_cc_bindings(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiCcBindingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_cc_binding_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerProfile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  if (info.Length() < 2 || !info[1].IsString()) {
    Napi::TypeError::New(env, "setControllerProfile expects a preset name string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string preset = sonare_node::node_narrow_string(env, info[1], "preset");
  ThrowIfError(env, sonare_engine_set_controller_profile(engine_, destination_id, preset.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::BindController(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  SonareControllerBinding binding{};
  if (!ReadControllerBinding(env, info.Length() > 1 ? info[1] : env.Undefined(), &binding)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_bind_controller(engine_, destination_id, &binding));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearControllerBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  ThrowIfError(env, sonare_engine_clear_controller_bindings(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ControllerBindingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_controller_binding_count(engine_, destination_id, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerVelocityMeaningful(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  bool meaningful = false;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !RequiredBoolArg(env, info, 1, "meaningful", &meaningful)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_controller_velocity_meaningful(engine_, destination_id,
                                                                     meaningful ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ControllerVelocityMeaningful(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  int meaningful = 0;
  ThrowIfError(env,
               sonare_engine_controller_velocity_meaningful(engine_, destination_id, &meaningful));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Boolean::New(env, meaningful != 0);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerNoteTracking(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  // Both are required rather than defaulted, for the reason the C ABI refuses
  // an ordinal past either enum instead of clamping it: a misspelling that
  // resolved to the default would configure a dimension the caller never named
  // with a rule they never asked for, and neither shows until two notes share a
  // member channel.
  int dimension = SONARE_MPE_DIMENSION_BEND;
  if (!sonare_node::SynthEnumValue(env, info[1], kMpeDimensions, SONARE_MPE_DIMENSION_COUNT,
                                   "dimension", &dimension)) {
    return env.Undefined();
  }
  int tracking = SONARE_NOTE_TRACKING_LAST;
  if (!sonare_node::SynthEnumValue(env, info[2], kNoteTrackings, SONARE_NOTE_TRACKING_COUNT,
                                   "tracking", &tracking)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_controller_note_tracking(engine_, destination_id, dimension,
                                                               tracking));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ControllerNoteTracking(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  int dimension = SONARE_MPE_DIMENSION_BEND;
  if (!sonare_node::SynthEnumValue(env, info[1], kMpeDimensions, SONARE_MPE_DIMENSION_COUNT,
                                   "dimension", &dimension)) {
    return env.Undefined();
  }
  int tracking = 0;
  ThrowIfError(
      env, sonare_engine_controller_note_tracking(engine_, destination_id, dimension, &tracking));
  if (env.IsExceptionPending()) return env.Undefined();
  return sonare_node::SynthEnumName(env, tracking, kNoteTrackings, SONARE_NOTE_TRACKING_COUNT);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetArticulation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t channel = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel)) {
    return env.Undefined();
  }
  // Required rather than defaulted, for the reason the C ABI refuses an ordinal
  // past the enum instead of clamping it: poly substituted for a misspelled
  // mono-legato plays every note and slurs none of them, which sounds exactly
  // like a request that took.
  int articulation = SONARE_ARTICULATION_POLY;
  if (!sonare_node::SynthEnumValue(env, info[2], kArticulations, SONARE_ARTICULATION_COUNT,
                                   "articulation", &articulation)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_articulation(engine_, destination_id, channel, articulation));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetPartRig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t part = 0;
  std::string inserts_json;
  int mode = SONARE_PART_RIG_BANK;
  // The mode and part are required: a defaulted mode would turn a misspelled
  // 'chain' into the bank rig, which is a request that looks as if it took.
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !RequiredMidiByteValue(env, info[1], "part", &part) ||
      !sonare_node::SynthEnumValue(env, info[2], sonare_node::kPartRigModes,
                                   sonare_node::kPartRigModeCount, "mode", &mode) ||
      !OptionalStringArg(env, info, 3, "inserts", "", &inserts_json)) {
    return env.Undefined();
  }
  const char* json = info[3].IsString() ? inserts_json.c_str() : nullptr;
  ThrowIfError(env, sonare_engine_set_part_rig(engine_, destination_id, part, mode, json));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Articulation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t channel = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel)) {
    return env.Undefined();
  }
  int articulation = 0;
  ThrowIfError(env, sonare_engine_articulation(engine_, destination_id, channel, &articulation));
  if (env.IsExceptionPending()) return env.Undefined();
  return sonare_node::SynthEnumName(env, articulation, kArticulations, SONARE_ARTICULATION_COUNT);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::LegatoFallbackCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_legato_fallback_count(engine_, destination_id, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  std::string config;
  if (!OptionalStringArg(env, info, 1, "config", "", &config)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_midi_fx(engine_, destination_id, config.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id)) return env.Undefined();
  ThrowIfError(env, sonare_engine_clear_midi_fx(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}
