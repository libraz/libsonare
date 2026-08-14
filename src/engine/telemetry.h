#pragma once

/// @file telemetry.h
/// @brief Fixed-size realtime engine telemetry records.

#include <cstdint>
#include <type_traits>

namespace sonare::engine {

enum class TelemetryType : uint16_t {
  kProcessBlock = 0,
  kError = 1,
};

enum class TelemetryErrorCode : uint16_t {
  kNone = 0,
  kCommandQueueOverflow = 1,
  kPendingCommandOverflow = 2,
  kBoundaryOverflow = 3,
  kTelemetryOverflow = 4,
  kCaptureOverflow = 5,
  kMaxBlockExceeded = 6,
  kUnknownTarget = 7,
  kNonRealtimeSafeParameter = 8,
  kNotPrepared = 9,
  // A known CommandType that is part of the binding control vocabulary but is
  // not applied through the realtime command queue (it must be invoked via a
  // direct engine setter). Distinct from kUnknownTarget, which means the
  // command itself was queueable but referenced an unbound target.
  kNonQueueableCommand = 10,
  kAutomationBindTargetOverflow = 11,
  kStaleAutomationLanes = 12,
  kSmoothedParameterCapacity = 13,
  // Commands left queued because the per-block drain cap was exceeded. Unlike
  // kCommandQueueOverflow (commands dropped at push because the queue was
  // full), nothing is lost here: the backlog is deferred to future blocks and
  // the value carries the remaining queued count, not a dropped count.
  kCommandBacklogDeferred = 14,
  // A paged clip source returned a page miss on the audio thread. The value
  // carries the clip id; detailed page requests are drained separately.
  kClipPageUnderrun = 15,
  // An insert-parameter automation target could not claim a smoother slot
  // because the per-strip / master slot table was full. The automation is
  // dropped (existing slots keep advancing); the value carries the number of
  // drops accrued during the block.
  kInsertAutomationOverflow = 16,
  // MIDI clock generation reached its fixed per-block realtime budget. The
  // value is the number of overflow occurrences (normally one per block).
  kMidiClockOverflow = 17,
  // Metronome beat collection exceeded its fixed realtime event list. The
  // value is the number of overflow occurrences.
  kMetronomeOverflow = 18,
  // Ordinal 19 is reserved by the WASM worklet protocol. Keep new core
  // telemetry errors after that reserved slot so binding ordinals remain
  // stable across surfaces.
  kMaxChannelsExceeded = 20,
};

struct Telemetry {
  TelemetryType type = TelemetryType::kProcessBlock;
  TelemetryErrorCode error = TelemetryErrorCode::kNone;
  int64_t render_frame = 0;
  int64_t timeline_sample = 0;
  int64_t audible_timeline_sample = 0;
  int32_t graph_latency_samples_q8 = 0;
  uint32_t value = 0;
};

static_assert(std::is_trivially_copyable_v<Telemetry>,
              "Telemetry must stay trivially copyable for lock-free queues");

}  // namespace sonare::engine
