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

// Only kSetParam, kSetParamSmoothed, kTransportPlay, kTransportStop,
// kTransportSeekSample, kTransportSeekPpq, and kSeekMarker are processed
// inline by RealtimeEngine::push_command/apply_command. The remaining values
// form the higher-level binding control vocabulary applied through direct
// engine setter methods (set_tempo, set_loop, set_capture_*,
// set_metronome_config, set_clips, set_markers, swap_graph, etc.), not via
// the realtime command queue.
enum class CommandType : uint16_t {
  kSetParam,
  kSetParamSmoothed,
  kTransportPlay,
  kTransportStop,
  kTransportSeekSample,
  kTransportSeekPpq,
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
  kSeekMarker,
};

union CommandArg {
  float f;
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
