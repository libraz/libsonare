/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <string>

#include "realtime_engine_wasm.h"

void RealtimeEngineWasm::validatePrepare(double sample_rate, int max_block_size) {
  if (!std::isfinite(sample_rate) || sample_rate < sonare::kMinAudioSampleRate ||
      sample_rate > sonare::kMaxAudioSampleRate || max_block_size <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "prepare: sample_rate must be finite and within 8000..384000; "
                                  "max_block_size must be positive");
  }
}

size_t RealtimeEngineWasm::capacity(int requested) {
  return requested == 0 ? 1024 : static_cast<size_t>(requested);
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                                       int telemetry_capacity) {
  prepareWithChannels(sample_rate, val(max_block_size), val(command_capacity),
                      val(telemetry_capacity), val(64));
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                                       int telemetry_capacity, int max_channels) {
  prepareWithChannels(sample_rate, val(max_block_size), val(command_capacity),
                      val(telemetry_capacity), val(max_channels));
}

void RealtimeEngineWasm::prepare(double sample_rate, const val& max_block_size,
                                 const val& command_capacity, const val& telemetry_capacity) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, val(64));
}

void RealtimeEngineWasm::prepareWithChannels(double sample_rate, const val& max_block_size_val,
                                             const val& command_capacity_val,
                                             const val& telemetry_capacity_val,
                                             const val& max_channels_val) {
  const int max_block_size = checkedIntFromVal(max_block_size_val, "maxBlockSize");
  const int command_capacity = checkedIntFromVal(command_capacity_val, "commandCapacity");
  const int telemetry_capacity = checkedIntFromVal(telemetry_capacity_val, "telemetryCapacity");
  const int max_channels = checkedIntFromVal(max_channels_val, "maxChannels");
  validatePrepare(sample_rate, max_block_size);
  // Both the bound and the message it reports come from the engine's constant,
  // so raising the ceiling moves the guard and the text a host reads together.
  constexpr int kMaxChannels = static_cast<int>(sonare::engine::RealtimeEngine::kMaxAudioChannels);
  if (max_channels <= 0 || max_channels > kMaxChannels) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "prepare: max_channels must be within 1.." + std::to_string(kMaxChannels));
  }
  // The C ABI takes both as size_t, so only this surface can express a negative
  // one. Refuse it here rather than letting capacity() read it as the sentinel,
  // which would answer a caller error with the default queue.
  if (command_capacity < 0 || telemetry_capacity < 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "prepare: command_capacity and telemetry_capacity must be 0 (the internal minimum) or "
        "positive");
  }
  // Mirrors the C ABI: the engine clamps these, but a host asking for more than
  // it can get should hear about it rather than quietly receive a smaller
  // engine. telemetry_capacity is the sharp one — the engine reserves it per
  // metered lane, so its fan-out dwarfs the requested number.
  if (capacity(command_capacity) > sonare::engine::RealtimeEngine::kMaxCommandCapacity ||
      capacity(telemetry_capacity) > sonare::engine::RealtimeEngine::kMaxTelemetryCapacity) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "prepare: command_capacity or telemetry_capacity exceeds its documented maximum");
  }
  engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                  capacity(telemetry_capacity), max_channels);
}

void registerRealtimeEngineBindings() {
  class_<RealtimeEngineWasm> cls("RealtimeEngine");
  cls.constructor<double, int, int, int>()
      .constructor<double, int, int, int, int>()
      .function("prepare", &RealtimeEngineWasm::prepare)
      .function("prepareWithChannels", &RealtimeEngineWasm::prepareWithChannels);
  registerRealtimeEngineTransport(cls);
  registerRealtimeEngineParams(cls);
  registerRealtimeEngineMidi(cls);
  registerRealtimeEngineMixer(cls);
  registerRealtimeEngineClips(cls);
  registerRealtimeEngineCapture(cls);
  registerRealtimeEngineProcessing(cls);
  registerRealtimeEngineTelemetry(cls);
}

#endif  // __EMSCRIPTEN__
