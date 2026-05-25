#pragma once

/// @file engine_controller.h
/// @brief Non-RT control facade for the realtime engine skeleton.

#include <cstddef>

#include "engine/realtime_engine.h"

namespace sonare::engine {

class EngineController {
 public:
  void prepare(double sample_rate, int max_block_size);

  RealtimeEngine& engine() noexcept { return engine_; }
  const RealtimeEngine& engine() const noexcept { return engine_; }

  bool send_command(const rt::Command& command) noexcept;
  bool play(int64_t render_frame = -1) noexcept;
  bool stop(int64_t render_frame = -1) noexcept;
  bool seek_sample(int64_t timeline_sample, int64_t render_frame = -1) noexcept;
  bool drain_telemetry(Telemetry* out, size_t max_records, size_t* written) noexcept;

 private:
  RealtimeEngine engine_{};
};

}  // namespace sonare::engine
