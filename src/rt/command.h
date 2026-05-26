#pragma once

/// @file command.h
/// @brief POD command records for realtime engine control queues.

#include <cstdint>
#include <type_traits>

namespace sonare::rt {

/// Increment when Command, telemetry, or SharedArrayBuffer record layouts
/// change. Bindings compare this value at startup before connecting to the
/// realtime engine.
inline constexpr uint32_t kEngineAbiVersion = 2;

// The CommandType space is split into two disjoint groups:
//
//  (1) RT-QUEUE VOCABULARY -- safe to enqueue via push_command() and applied by
//      apply_command() on the audio thread. These carry only POD scalars and
//      perform in-place, allocation-free updates:
//        kSetParam, kSetParamSmoothed, kTransportPlay, kTransportStop,
//        kTransportSeekSample, kTransportSeekPpq, kSeekMarker.
//
//  (2) DIRECT-SETTER OPERATIONS -- known command names that must NOT flow
//      through the realtime queue because they own data swapped via the
//      RtPublisher pattern on control-thread setters (set_tempo, set_loop,
//      swap_graph, set_clips, set_capture_*, set_metronome_config,
//      set_markers, ...):
//        kSetTempoMap, kSetLoop, kSwapGraph, kSwapAutomation, kSetSoloMute,
//        kAddClip, kRemoveClip, kArmRecord, kPunch, kSetMetronome, kSetMarker.
//
// If a group-(2) value is pushed through the queue, apply_command() rejects it
// with TelemetryErrorCode::kNonQueueableCommand (NOT the misleading
// kUnknownTarget, which is reserved for queueable commands referencing an
// unbound target). The two groups are kept in a single enum so the binding ABI
// (kEngineAbiVersion) and the SharedArrayBuffer record layout stay stable.
enum class CommandType : uint16_t {
  // -- Group (1): RT-queue vocabulary --
  kSetParam,
  kSetParamSmoothed,
  kTransportPlay,
  kTransportStop,
  kTransportSeekSample,
  kTransportSeekPpq,
  // -- Group (2): direct-setter operations (rejected if queued) --
  kSetTempoMap,
  kSetLoop,
  kSwapGraph,
  kSwapAutomation,
  kSetSoloMute,
  kAddClip,
  kRemoveClip,
  kArmRecord,
  kPunch,
  kSetMetronome,
  kSetMarker,
  // -- Group (1) continued --
  kSeekMarker,
};

union CommandArg {
  float f;
  double d;  // full-precision scalar (e.g. seek PPQ); shares the 64-bit slot
  int64_t i;
  void* ptr;
};

struct Command {
  CommandType type = CommandType::kSetParam;
  uint32_t target_id = 0;
  /// Monotonic render-frame time. -1 means block-head/immediate.
  int64_t sample_time = -1;
  CommandArg arg{};
};

static_assert(std::is_trivially_copyable_v<CommandArg>,
              "CommandArg must stay trivially copyable for lock-free queues");
static_assert(std::is_trivially_copyable_v<Command>,
              "Command must stay trivially copyable for lock-free queues");

}  // namespace sonare::rt
