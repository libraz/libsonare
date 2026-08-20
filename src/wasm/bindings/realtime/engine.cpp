/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include <cmath>

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
  return requested > 0 ? static_cast<size_t>(requested) : 1024;
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                                       int telemetry_capacity) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, 64);
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                                       int telemetry_capacity, int max_channels) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity,
                      max_channels);
}

void RealtimeEngineWasm::prepare(double sample_rate, int max_block_size, int command_capacity,
                                 int telemetry_capacity) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, 64);
}

void RealtimeEngineWasm::prepareWithChannels(double sample_rate, int max_block_size,
                                             int command_capacity, int telemetry_capacity,
                                             int max_channels) {
  validatePrepare(sample_rate, max_block_size);
  if (max_channels <= 0 || max_channels > 64) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "prepare: max_channels must be within 1..64");
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
