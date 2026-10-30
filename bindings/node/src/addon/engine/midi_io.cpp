// Node engine: live MIDI push, MIDI input source and external MIDI methods.

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

// Refuses a live MIDI push whose arguments leave the MIDI domain before the C
// call, naming the JS method; true when a RangeError is now pending.
bool RefuseLivePushArg(Napi::Env env, const char* method, const char* error) {
  if (error == nullptr) return false;
  Napi::RangeError::New(env, std::string(method) + ": " + error).ThrowAsJavaScriptException();
  return true;
}

constexpr size_t kMaxUmpWords = 4;

/// Reads a 1..4 word UMP message from a Uint32Array or a plain number array,
/// refusing every other value by name and leaving one pending exception on false.
bool ReadUmpWords(Napi::Env env, const Napi::Value& value, const char* name,
                  std::array<uint32_t, kMaxUmpWords>* words, size_t* count) {
  if (env.IsExceptionPending()) return false;
  Napi::Object array;
  size_t length = 0;
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_uint32_array) {
    array = value.As<Napi::Object>();
    length = value.As<Napi::TypedArray>().ElementLength();
  } else if (value.IsArray()) {
    array = value.As<Napi::Object>();
    length = value.As<Napi::Array>().Length();
  } else {
    Napi::TypeError::New(env, std::string(name) + " must be a Uint32Array or a number array")
        .ThrowAsJavaScriptException();
    return false;
  }
  if (length < 1 || length > kMaxUmpWords) {
    Napi::RangeError::New(env, std::string(name) + " must hold 1 to 4 words")
        .ThrowAsJavaScriptException();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const std::string label = std::string(name) + "[" + std::to_string(i) + "]";
    if (!RequiredWordValue(env, array.Get(static_cast<uint32_t>(i)), label, &(*words)[i])) {
      return false;
    }
  }
  *count = length;
  return true;
}

}  // namespace

Napi::Value RealtimeEngineWrap::PushMidiNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 4, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiNoteOn",
                        sonare::midi::live_push_note_error(group, channel, note, velocity))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_note_on(engine_, destination_id, group, channel, note,
                                                    velocity, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_midi_input_source(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_clear_midi_input_source(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiInputPendingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_input_pending_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 3, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 4, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputNoteOn",
                        sonare::midi::live_push_note_error(group, channel, note, velocity))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_note_on(engine_, group, channel, note, velocity,
                                                          port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 3, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 4, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputNoteOff",
                        sonare::midi::live_push_note_error(group, channel, note, velocity))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_note_off(engine_, group, channel, note, velocity,
                                                           port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t controller = 0;
  uint8_t value = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "controller", 0, &controller) ||
      !OptionalMidiByteArg(env, info, 3, "value", 0, &value) ||
      !OptionalInt64Arg(env, info, 4, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputCc",
                        sonare::midi::live_push_cc_error(group, channel, controller, value))) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_push_midi_input_cc(engine_, group, channel, controller, value, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputPitchBend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint16_t bend14 = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalUint16Arg(env, info, 2, "bend14", 0, &bend14) ||
      !OptionalInt64Arg(env, info, 3, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputPitchBend",
                        sonare::midi::live_push_pitch_bend_error(group, channel, bend14))) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_push_midi_input_pitch_bend(engine_, group, channel, bend14, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputChannelPressure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t pressure = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "pressure", 0, &pressure) ||
      !OptionalInt64Arg(env, info, 3, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputChannelPressure",
                        sonare::midi::live_push_channel_pressure_error(group, channel, pressure))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_channel_pressure(engine_, group, channel,
                                                                   pressure, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputPolyPressure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t pressure = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 3, "pressure", 0, &pressure) ||
      !OptionalInt64Arg(env, info, 4, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(
          env, "pushMidiInputPolyPressure",
          sonare::midi::live_push_poly_pressure_error(group, channel, note, pressure))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_poly_pressure(engine_, group, channel, note,
                                                                pressure, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 4, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiNoteOff",
                        sonare::midi::live_push_note_error(group, channel, note, velocity))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_note_off(engine_, destination_id, group, channel, note,
                                                     velocity, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t controller = 0;
  uint8_t value = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "controller", 0, &controller) ||
      !OptionalMidiByteArg(env, info, 4, "value", 0, &value) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiCc",
                        sonare::midi::live_push_cc_error(group, channel, controller, value))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_cc(engine_, destination_id, group, channel, controller,
                                               value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiPitchBend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint16_t bend14 = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalUint16Arg(env, info, 3, "bend14", 0, &bend14) ||
      !OptionalInt64Arg(env, info, 4, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiPitchBend",
                        sonare::midi::live_push_pitch_bend_error(group, channel, bend14))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_pitch_bend(engine_, destination_id, group, channel,
                                                       bend14, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiChannelPressure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t pressure = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "pressure", 0, &pressure) ||
      !OptionalInt64Arg(env, info, 4, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiChannelPressure",
                        sonare::midi::live_push_channel_pressure_error(group, channel, pressure))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_channel_pressure(engine_, destination_id, group,
                                                             channel, pressure, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiPolyPressure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t pressure = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 4, "pressure", 0, &pressure) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(
          env, "pushMidiPolyPressure",
          sonare::midi::live_push_poly_pressure_error(group, channel, note, pressure))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_poly_pressure(engine_, destination_id, group, channel,
                                                          note, pressure, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiPanic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_push_midi_panic(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiSysex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) return env.Undefined();
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 1 && info[1].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[1].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 1 && sonare_node::IsUint8Array(info[1])) {
    Napi::Uint8Array arr = info[1].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "pushMidiSysex expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_push_midi_sysex(engine_, destination_id, bytes, len, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiUmp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  std::array<uint32_t, kMaxUmpWords> words{};
  size_t count = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !ReadUmpWords(env, info[1], "words", &words, &count) ||
      !OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiUmp",
                        sonare::engine::RealtimeEngine::is_pushable_midi_ump(words.data(), count)
                            ? nullptr
                            : sonare::midi::kLivePushUmpShapeError)) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_push_midi_ump(engine_, destination_id, words.data(), count, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputUmp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  std::array<uint32_t, kMaxUmpWords> words{};
  size_t count = 0;
  int64_t port_time = 0;
  if (!ReadUmpWords(env, info[0], "words", &words, &count) ||
      !OptionalInt64Arg(env, info, 1, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputUmp",
                        sonare::engine::RealtimeEngine::is_pushable_midi_ump(words.data(), count)
                            ? nullptr
                            : sonare::midi::kLivePushUmpShapeError)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_ump(engine_, words.data(), count, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t program = 0;
  bool bank_valid = false;
  uint8_t bank_msb = 0;
  uint8_t bank_lsb = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "program", 0, &program) ||
      !OptionalBoolArg(env, info, 4, "bankValid", false, &bank_valid) ||
      !OptionalMidiByteArg(env, info, 5, "bankMsb", 0, &bank_msb) ||
      !OptionalMidiByteArg(env, info, 6, "bankLsb", 0, &bank_lsb) ||
      !OptionalInt64Arg(env, info, 7, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiProgram",
                        sonare::midi::live_push_program_error(group, channel, program, bank_valid,
                                                              bank_msb, bank_lsb))) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_push_midi_program(engine_, destination_id, group, channel, program,
                                               bank_valid ? 1 : 0, bank_msb, bank_lsb, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputProgram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t program = 0;
  bool bank_valid = false;
  uint8_t bank_msb = 0;
  uint8_t bank_lsb = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "program", 0, &program) ||
      !OptionalBoolArg(env, info, 3, "bankValid", false, &bank_valid) ||
      !OptionalMidiByteArg(env, info, 4, "bankMsb", 0, &bank_msb) ||
      !OptionalMidiByteArg(env, info, 5, "bankLsb", 0, &bank_lsb) ||
      !OptionalInt64Arg(env, info, 6, "offsetInBlockSamples", 0, &port_time)) {
    return env.Undefined();
  }
  if (RefuseLivePushArg(env, "pushMidiInputProgram",
                        sonare::midi::live_push_program_error(group, channel, program, bank_valid,
                                                              bank_msb, bank_lsb))) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_program(engine_, group, channel, program,
                                                          bank_valid ? 1 : 0, bank_msb, bank_lsb,
                                                          port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiDestinationExternal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  bool external = false;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &destination_id) ||
      !RequiredBoolArg(env, info, 1, "external", &external)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_set_midi_destination_external(engine_, destination_id, external ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetExternalMidiClockEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  bool enabled = false;
  if (!RequiredBoolArg(env, info, 0, "enabled", &enabled)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_external_midi_clock_enabled(engine_, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ExternalMidiDroppedCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_external_midi_dropped_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

// Drains queued external-MIDI events, already lowered to MIDI 1.0 byte
// messages. Each returned item is { destinationId, renderFrame, bytes:
// number[] }; transport/clock bytes carry destinationId === 0xFFFFFFFF. A
// single queued channel-voice event may lower to more than one item. @p
// maxRecords caps the number of output events produced (the unit shared by
// every surface) and goes through the same domain check as the telemetry
// drains. The C-ABI capacity passed per call is clamped to the remaining budget
// so the destructive drain never consumes more events than it can return —
// events that do not fit stay queued for the next call (lossless). A budget
// below kMaxLoweredMessages could never consume a record, so it is rejected
// rather than draining nothing while the queue keeps growing.
Napi::Value RealtimeEngineWrap::DrainExternalMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  // The most MIDI-1 messages one queue record can lower to (a MIDI 2.0
  // registered / assignable controller's four RPN / NRPN messages), and hence
  // the smallest capacity the C ABI accepts.
  constexpr size_t kMaxLoweredMessages = 4;
  constexpr uint32_t kMaxBytes =
      static_cast<uint32_t>(std::extent<decltype(SonareExternalMidiEvent::bytes)>::value);
  size_t max_records = 1024;
  if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!NonNegativeSizeTArg(env, info, 0, "maxRecords", &max_records)) return env.Undefined();
  }
  if (max_records > 0 && max_records < kMaxLoweredMessages) {
    Napi::RangeError::New(env, "maxRecords must be 0 or at least " +
                                   std::to_string(kMaxLoweredMessages) +
                                   " to guarantee forward progress")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env);
  if (max_records == 0) return out;
  std::array<SonareExternalMidiEvent, 256> records{};
  size_t out_index = 0;
  while (out_index + kMaxLoweredMessages <= max_records) {
    const size_t want = std::min(records.size(), max_records - out_index);
    size_t written = 0;
    const SonareError err =
        sonare_engine_drain_external_midi(engine_, records.data(), want, &written);
    ThrowIfError(env, err);
    if (env.IsExceptionPending()) return env.Undefined();
    if (written == 0) break;
    // Defensive: the C ABI promises written <= want, but clamp anyway so a
    // misreporting drain cannot read past the buffer or overrun the budget.
    written = std::min(written, want);
    for (size_t i = 0; i < written; ++i) {
      const SonareExternalMidiEvent& rec = records[i];
      Napi::Object item = Napi::Object::New(env);
      item.Set("destinationId", Napi::Number::New(env, static_cast<double>(rec.destination_id)));
      item.Set("renderFrame", Napi::Number::New(env, static_cast<double>(rec.render_frame)));
      // byte_count is documented as 1..3, but it indexes a fixed-size array:
      // bound it by the array extent before reading through it.
      const uint32_t byte_count = std::min(rec.byte_count, kMaxBytes);
      Napi::Array bytes = Napi::Array::New(env, byte_count);
      for (uint32_t b = 0; b < byte_count; ++b) {
        bytes.Set(b, Napi::Number::New(env, rec.bytes[b]));
      }
      item.Set("bytes", bytes);
      out.Set(static_cast<uint32_t>(out_index++), item);
    }
  }
  return out;
  SONARE_NODE_CATCH(env)
}
