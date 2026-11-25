/// @file realtime_engine.cpp
/// @brief Core of the embind realtime-engine facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include <string>

#include "c_api/engine_config_domains.h"
#include "core/audio.h"
#include "realtime_engine_wasm.h"
#include "util/refusal_predicates.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/project_timeline_internal.h"
#include "wasm/bindings/common/project_timeline_wasm.h"
#endif

void RealtimeEngineWasm::validatePrepare(double sample_rate) {
  if (!sonare::is_supported_sample_rate(sample_rate)) {
    throw WasmRangeError("sampleRate " +
                         sonare::util::refusal::integer_range_predicate(
                             sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate));
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
  if (result.refusal) std::rethrow_exception(result.refusal);
  if (!result.ok()) throw sonare::SonareException(result.code, result.message);
}
#endif

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, const val& max_block_size,
                                       const val& command_capacity, const val& telemetry_capacity) {
  prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity, val::undefined());
}

RealtimeEngineWasm::RealtimeEngineWasm(double sample_rate, const val& max_block_size,
                                       const val& command_capacity, const val& telemetry_capacity,
                                       const val& max_channels) {
  prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity, max_channels);
}

void RealtimeEngineWasm::prepare(double sample_rate, const val& max_block_size_val,
                                 const val& command_capacity_val, const val& telemetry_capacity_val,
                                 const val& max_channels_val) {
  using sonare::engine::RealtimeEngine;
  // An omitted maxBlockSize or maxChannels takes the engine default; a written one is read in its
  // domain. The two capacities read 0 as their default.
  const int max_block_size = optionalDomainValue<int>(max_block_size_val, "maxBlockSize",
                                                      sonare_c_detail::kEngineMaxBlockSizeDomain)
                                 .value_or(RealtimeEngine::kDefaultMaxBlock);
  const int command_capacity =
      command_capacity_val.isUndefined()
          ? static_cast<int>(RealtimeEngine::kDefaultCommandCapacity)
          : checkedIntFromVal(command_capacity_val, "commandCapacity");
  const int telemetry_capacity =
      telemetry_capacity_val.isUndefined()
          ? static_cast<int>(RealtimeEngine::kDefaultTelemetryCapacity)
          : checkedIntFromVal(telemetry_capacity_val, "telemetryCapacity");
  const int max_channels = optionalDomainValue<int>(max_channels_val, "maxChannels",
                                                    sonare_c_detail::kEngineMaxChannelsDomain)
                               .value_or(RealtimeEngine::kDefaultMaxChannels);
  validatePrepare(sample_rate);
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
      .function("prepare", &RealtimeEngineWasm::prepare);
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
