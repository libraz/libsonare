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
#include "sonare_wrap_midi_cc.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

struct MidiCcBindingListGuard {
  SonareMidiCcBindingList value{};

  ~MidiCcBindingListGuard() { sonare_free_midi_cc_binding_list(&value); }
};

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
  int input = 0;
  int axis = 0;
  if (!sonare_node::SynthEnumValue(env, obj.Get("input"), kControllerInputs,
                                   SONARE_CONTROLLER_INPUT_COUNT, "controller input", &input) ||
      !sonare_node::SynthEnumValue(env, obj.Get("axis"), kControllerAxes,
                                   SONARE_CONTROLLER_AXIS_COUNT, "controller axis", &axis)) {
    return false;
  }
  out->input = static_cast<uint8_t>(input);
  out->axis = static_cast<uint8_t>(axis);
  out->index = MidiByteProperty(env, obj, "index", 0);
  out->lo = sonare_node::FloatProperty(obj, "lo", 0.0f);
  out->hi = sonare_node::FloatProperty(obj, "hi", 1.0f);
  out->curve = sonare_node::FloatProperty(obj, "curve", 1.0f);
  return !env.IsExceptionPending();
}

}  // namespace

Napi::Value RealtimeEngineWrap::BindMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "binding must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareMidiCcBinding binding{};
  const auto object = info[0].As<Napi::Object>();
  if (!sonare_node::CcBindingFromObject(env, object, &binding, "")) {
    return env.Undefined();
  }
  sonare_node::ThrowIfErrorFlattening(env, sonare_engine_bind_midi_cc(engine_, &binding),
                                      "binding");
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

Napi::Value RealtimeEngineWrap::MidiCcBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  MidiCcBindingListGuard bindings;
  ThrowIfError(env, sonare_engine_midi_cc_bindings(engine_, &bindings.value));
  if (env.IsExceptionPending()) return env.Undefined();
  if ((bindings.value.count == 0) != (bindings.value.items == nullptr) ||
      bindings.value.count > std::numeric_limits<uint32_t>::max()) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                         "MIDI CC binding list is not representable as an array");
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, static_cast<uint32_t>(bindings.value.count));
  for (size_t i = 0; i < bindings.value.count; ++i) {
    const SonareMidiCcBinding& binding = bindings.value.items[i];
    if (binding.kind >= sonare_node::CcBindingKindNames().size()) {
      sonare_node::ThrowSonareErrorMessage(
          env, SONARE_ERROR_INVALID_FORMAT,
          "unknown MIDI CC binding kind ordinal: " + std::to_string(binding.kind));
      return env.Undefined();
    }
    if (binding.consume > 1) {
      sonare_node::ThrowSonareErrorMessage(
          env, SONARE_ERROR_INVALID_FORMAT,
          "unknown MIDI CC consume value: " + std::to_string(binding.consume));
      return env.Undefined();
    }
    Napi::Object item = sonare_node::CcBindingToObject(env, binding);
    out.Set(static_cast<uint32_t>(i), item);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerProfile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  if (info.Length() < 2 || !info[1].IsString()) {
    Napi::TypeError::New(env, "preset must be a string").ThrowAsJavaScriptException();
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
      !RequiredBoolArg(env, info, 1, "controllerVelocityMeaningful", &meaningful)) {
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
  // monoLegato plays every note and slurs none of them, which sounds exactly
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
  size_t count = 0;
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
