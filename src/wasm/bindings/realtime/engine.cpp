/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include <string>

#include "realtime_engine_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/project_timeline_internal.h"
#include "wasm/bindings/common/project_timeline_wasm.h"
#endif

void RealtimeEngineWasm::validatePrepare(double sample_rate, int max_block_size) {
  if (!sonare::is_supported_sample_rate(sample_rate) || max_block_size <= 0) {
    throw WasmRangeError(
        "prepare: sample_rate must be a whole number of hertz within 8000..384000; "
        "max_block_size must be positive");
  }
}

size_t RealtimeEngineWasm::capacity(int requested) {
  return requested == 0 ? 1024 : static_cast<size_t>(requested);
}

#if defined(SONARE_WITH_ARRANGEMENT)
void RealtimeEngineWasm::applyProjectTimeline(const val& timeline_id_val) {
  namespace arr = sonare::arrangement;
  const uint32_t id = checkedUintFromVal(timeline_id_val, "timeline id");
  const SonareProjectTimeline* handle = ProjectTimelineWasm::lookup(id);
  if (handle == nullptr || !handle->timeline) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "project timeline has been released");
  }
  if (engine_.transport_state_control().playing) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        "a project timeline can only be applied while the transport is stopped");
  }
  arr::ApplyOptions options;
  options.bind_strips = true;
  arr::ApplyResult result = arr::apply_to_engine(*handle->timeline, engine_, options);
  // The lane copy is what setAutomationLane republishes, and the marker strings backed markers
  // the apply has replaced; both follow the engine's new state.
  if (result.outcome == arr::ApplyOutcome::kApplied) {
    automation_lanes_ = std::move(result.installed_automation);
    marker_strings_.clear();
    applied_timeline_ = handle->timeline;
  } else if (result.outcome == arr::ApplyOutcome::kCleared) {
    automation_lanes_.clear();
    marker_strings_.clear();
    applied_timeline_.reset();
  }
  if (!result.ok()) throw sonare::SonareException(result.code, result.message);
}
#endif

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, const val& max_block_size,
                                       const val& command_capacity, const val& telemetry_capacity) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, val(64));
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, const val& max_block_size,
                                       const val& command_capacity, const val& telemetry_capacity,
                                       const val& max_channels) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity,
                      max_channels);
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
    throw WasmRangeError("prepare: max_channels must be within 1.." + std::to_string(kMaxChannels));
  }
  // The C ABI takes both as size_t, so only this surface can express a negative
  // one. Refuse it here rather than letting capacity() read it as the sentinel,
  // which would answer a caller error with the default queue.
  if (command_capacity < 0 || telemetry_capacity < 0) {
    throw WasmRangeError(
        "prepare: command_capacity and telemetry_capacity must be 0 (the internal minimum) or "
        "positive");
  }
  // Mirrors the C ABI: the engine clamps these, but a host asking for more than
  // it can get should hear about it rather than quietly receive a smaller
  // engine. telemetry_capacity is the sharp one — the engine reserves it per
  // metered lane, so its fan-out dwarfs the requested number.
  if (capacity(command_capacity) > sonare::engine::RealtimeEngine::kMaxCommandCapacity ||
      capacity(telemetry_capacity) > sonare::engine::RealtimeEngine::kMaxTelemetryCapacity) {
    throw WasmRangeError(
        "prepare: command_capacity or telemetry_capacity exceeds its documented maximum");
  }
  engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                  capacity(telemetry_capacity), max_channels);
}

void registerRealtimeEngineBindings() {
  class_<RealtimeEngineWasm> cls("RealtimeEngine");
  // The block size and the capacities are registered as val rather than as int:
  // embind's integer glue wraps, so a request past 2^32 would reach a narrow
  // parameter as a small legal one and build an engine nobody asked for.
  cls.constructor<double, const val&, const val&, const val&>()
      .constructor<double, const val&, const val&, const val&, const val&>()
      .function("prepare", &RealtimeEngineWasm::prepare)
      .function("prepareWithChannels", &RealtimeEngineWasm::prepareWithChannels);
  registerRealtimeEngineTransport(cls);
  registerRealtimeEngineParams(cls);
  registerRealtimeEngineMidi(cls);
  registerRealtimeEngineMixer(cls);
#if defined(SONARE_WITH_ARRANGEMENT)
  cls.function("applyProjectTimeline", &RealtimeEngineWasm::applyProjectTimeline);
#endif
  registerRealtimeEngineClips(cls);
  registerRealtimeEngineCapture(cls);
  registerRealtimeEngineProcessing(cls);
  registerRealtimeEngineTelemetry(cls);
}

#endif  // __EMSCRIPTEN__
