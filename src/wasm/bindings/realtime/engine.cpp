/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"

void RealtimeEngineWasm::validatePrepare(double sample_rate, int max_block_size) {
  if (sample_rate <= 0.0 || max_block_size <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "prepare: sample_rate and max_block_size must be positive");
  }
}

size_t RealtimeEngineWasm::capacity(int requested) {
  return requested > 0 ? static_cast<size_t>(requested) : 1024;
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                                       int telemetry_capacity) {
  validatePrepare(sample_rate, max_block_size);
  engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                  capacity(telemetry_capacity));
}

void RealtimeEngineWasm::prepare(double sample_rate, int max_block_size, int command_capacity,
                                 int telemetry_capacity) {
  validatePrepare(sample_rate, max_block_size);
  engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                  capacity(telemetry_capacity));
}

void registerRealtimeEngineBindings() {
  class_<RealtimeEngineWasm> cls("RealtimeEngine");
  cls.constructor<double, int, int, int>().function("prepare", &RealtimeEngineWasm::prepare);
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
