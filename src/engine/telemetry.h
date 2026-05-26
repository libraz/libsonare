#pragma once

/// @file telemetry.h
/// @brief Fixed-size realtime engine telemetry records.

#include <cstdint>
#include <type_traits>

namespace sonare::engine {

enum class TelemetryType : uint16_t {
  kProcessBlock,
  kError,
};

enum class TelemetryErrorCode : uint16_t {
  kNone,
  kCommandQueueOverflow,
  kPendingCommandOverflow,
  kBoundaryOverflow,
  kTelemetryOverflow,
  kCaptureOverflow,
  kMaxBlockExceeded,
  kUnknownTarget,
  kNonRealtimeSafeParameter,
  kNotPrepared,
  // A known CommandType that is part of the binding control vocabulary but is
  // not applied through the realtime command queue (it must be invoked via a
  // direct engine setter). Distinct from kUnknownTarget, which means the
  // command itself was queueable but referenced an unbound target.
  kNonQueueableCommand,
  kAutomationBindTargetOverflow,
  kStaleAutomationLanes,
  kSmoothedParameterCapacity,
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
