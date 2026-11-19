/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include <string>

#include "realtime_engine_wasm.h"
#include "streaming/realtime_limits.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/project_timeline_internal.h"
#include "wasm/bindings/common/project_timeline_wasm.h"
#endif

void RealtimeEngineWasm::validatePrepare(double sample_rate, int max_block_size) {
  if (!sonare::is_supported_sample_rate(sample_rate) ||
      !sonare::is_valid_realtime_block_size(max_block_size)) {
    throw WasmRangeError(
        "sampleRate must be a whole number of hertz within 8000..384000; "
        "maxBlockSize must be within 1..1048576");
  }
}

size_t RealtimeEngineWasm::capacity(int requested, size_t default_value) {
  return requested == 0 ? default_value : static_cast<size_t>(requested);
}

#if defined(SONARE_WITH_ARRANGEMENT)
void RealtimeEngineWasm::applyProjectTimeline(const val& timeline_id_val) {
  namespace arr = sonare::arrangement;
  const uint32_t id = checkedUintFromVal(timeline_id_val, "timeline id");
  const SonareProjectTimeline* handle = ProjectTimelineWasm::lookup(id);
  if (handle == nullptr && ProjectTimelineWasm::issued(id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "project timeline has been released");
  }
  if (handle == nullptr || !handle->timeline) {
    throw WasmRangeError("timeline id names no project timeline");
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
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, val(0));
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, const val& max_block_size,
                                       const val& command_capacity, const val& telemetry_capacity,
                                       const val& max_channels) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity,
                      max_channels);
}

void RealtimeEngineWasm::prepare(double sample_rate, const val& max_block_size,
                                 const val& command_capacity, const val& telemetry_capacity) {
  prepareWithChannels(sample_rate, max_block_size, command_capacity, telemetry_capacity, val(0));
}

void RealtimeEngineWasm::prepareWithChannels(double sample_rate, const val& max_block_size_val,
                                             const val& command_capacity_val,
                                             const val& telemetry_capacity_val,
                                             const val& max_channels_val) {
  using sonare::engine::RealtimeEngine;
  // 0 is the library-default sentinel for each of the four sized arguments.
  int max_block_size = checkedIntFromVal(max_block_size_val, "maxBlockSize");
  if (max_block_size == 0) max_block_size = RealtimeEngine::kDefaultMaxBlock;
  const int command_capacity = checkedIntFromVal(command_capacity_val, "commandCapacity");
  const int telemetry_capacity = checkedIntFromVal(telemetry_capacity_val, "telemetryCapacity");
  int max_channels = checkedIntFromVal(max_channels_val, "maxChannels");
  if (max_channels == 0) max_channels = RealtimeEngine::kDefaultMaxChannels;
  validatePrepare(sample_rate, max_block_size);
  // Both the bound and the message it reports come from the engine's constant,
  // so raising the ceiling moves the guard and the text a host reads together.
  constexpr int kMaxChannels = static_cast<int>(sonare::engine::RealtimeEngine::kMaxAudioChannels);
  if (max_channels <= 0 || max_channels > kMaxChannels) {
    throw WasmRangeError("maxChannels must be within 1.." + std::to_string(kMaxChannels));
  }
  // The C ABI takes both as size_t, so only this surface can express a negative
  // one. Refuse it here rather than letting capacity() read it as the sentinel,
  // which would answer a caller error with the default queue.
  if (command_capacity < 0 || telemetry_capacity < 0) {
    throw WasmRangeError(
        "commandCapacity and telemetryCapacity must be 0 (the default) or positive");
  }
  // Mirrors the C ABI: the engine clamps these, but a host asking for more than
  // it can get should hear about it rather than quietly receive a smaller
  // engine. telemetry_capacity is the sharp one — the engine reserves it per
  // metered lane, so its fan-out dwarfs the requested number.
  const size_t command_resolved =
      capacity(command_capacity, RealtimeEngine::kDefaultCommandCapacity);
  const size_t telemetry_resolved =
      capacity(telemetry_capacity, RealtimeEngine::kDefaultTelemetryCapacity);
  if (command_resolved > RealtimeEngine::kMaxCommandCapacity ||
      telemetry_resolved > RealtimeEngine::kMaxTelemetryCapacity) {
    throw WasmRangeError("commandCapacity or telemetryCapacity exceeds its documented maximum");
  }
  engine_.prepare(sample_rate, max_block_size, command_resolved, telemetry_resolved, max_channels);
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
  registerRealtimeEngineMidiControl(cls);
  registerRealtimeEngineMidiIo(cls);
  registerRealtimeEngineMixer(cls);
#if defined(SONARE_WITH_ARRANGEMENT)
  cls.function("applyProjectTimeline", &RealtimeEngineWasm::applyProjectTimeline);
#endif
  registerRealtimeEngineClips(cls);
  registerClipPageProvider();
  registerRealtimeEngineCapture(cls);
  registerRealtimeEngineProcessing(cls);
  registerRealtimeEngineTelemetry(cls);
}

#endif  // __EMSCRIPTEN__
