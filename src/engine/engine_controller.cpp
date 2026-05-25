#include "engine/engine_controller.h"

namespace sonare::engine {

void EngineController::prepare(double sample_rate, int max_block_size) {
  engine_.prepare(sample_rate, max_block_size);
}

bool EngineController::send_command(const rt::Command& command) noexcept {
  return engine_.push_command(command);
}

bool EngineController::play(int64_t render_frame) noexcept {
  rt::Command command{};
  command.type = rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return send_command(command);
}

bool EngineController::stop(int64_t render_frame) noexcept {
  rt::Command command{};
  command.type = rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return send_command(command);
}

bool EngineController::seek_sample(int64_t timeline_sample, int64_t render_frame) noexcept {
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return send_command(command);
}

bool EngineController::drain_telemetry(Telemetry* out, size_t max_records,
                                       size_t* written) noexcept {
  size_t count = 0;
  Telemetry item{};
  while (count < max_records && engine_.pop_telemetry(item)) {
    if (out) {
      out[count] = item;
    }
    ++count;
  }
  if (written) {
    *written = count;
  }
  return count > 0;
}

}  // namespace sonare::engine
