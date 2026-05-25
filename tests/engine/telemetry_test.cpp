#include <array>
#include <catch2/catch_test_macros.hpp>

#include "engine/realtime_engine.h"

TEST_CASE("RealtimeEngine telemetry reports graph latency and audible timeline",
          "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  engine.set_graph_latency_samples_q8(2 << 8);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kProcessBlock);
  REQUIRE(telemetry.timeline_sample == 128);
  REQUIRE(telemetry.graph_latency_samples_q8 == (2 << 8));
  REQUIRE(telemetry.audible_timeline_sample == 126);
}

TEST_CASE("RealtimeEngine records unknown command as error telemetry", "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);

  sonare::rt::Command unknown{};
  unknown.type = sonare::rt::CommandType::kSetParam;
  unknown.sample_time = -1;
  REQUIRE(engine.push_command(unknown));

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kError);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kUnknownTarget);
}

TEST_CASE("RealtimeEngine records command queue overflow", "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128, 2);

  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportPlay;
  command.sample_time = -1;
  REQUIRE(engine.push_command(command));
  REQUIRE(engine.push_command(command));
  REQUIRE_FALSE(engine.push_command(command));

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));
  REQUIRE(telemetry.type == sonare::engine::TelemetryType::kError);
  REQUIRE(telemetry.error == sonare::engine::TelemetryErrorCode::kCommandQueueOverflow);
}

TEST_CASE("RealtimeEngine records pending command overflow without allocation",
          "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);

  for (size_t i = 0; i < sonare::engine::RealtimeEngine::kMaxPendingCommands + 1; ++i) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportPlay;
    command.sample_time = 1000 + static_cast<int64_t>(i);
    REQUIRE(engine.push_command(command));
  }

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);
  engine.process(io, 1, 128);

  bool found = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found =
        found || (telemetry.type == sonare::engine::TelemetryType::kError &&
                  telemetry.error == sonare::engine::TelemetryErrorCode::kPendingCommandOverflow);
  }
  REQUIRE(found);
}

TEST_CASE("RealtimeEngine records telemetry overflow after drain", "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128, 16, 2);

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);
  engine.process(io, 1, 128);
  engine.process(io, 1, 128);

  sonare::engine::Telemetry telemetry{};
  REQUIRE(engine.pop_telemetry(telemetry));

  engine.process(io, 1, 128);

  bool found = false;
  while (engine.pop_telemetry(telemetry)) {
    found = found || (telemetry.type == sonare::engine::TelemetryType::kError &&
                      telemetry.error == sonare::engine::TelemetryErrorCode::kTelemetryOverflow);
  }
  REQUIRE(found);
}

TEST_CASE("RealtimeEngine records non realtime-safe automation rejection", "[engine][telemetry]") {
  class NonRtProcessor final : public sonare::rt::ProcessorBase {
   public:
    void prepare(double, int) override {}
    void process(float* const*, int, int) override {}
    void reset() override {}
    bool set_parameter(unsigned int, float) override { return true; }
    bool parameter_is_realtime_safe(unsigned int) const noexcept override { return false; }
  };

  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  NonRtProcessor processor;

  sonare::automation::AutomationLane lane(4);
  lane.set_points({{0.0, 1.0f, sonare::automation::CurveType::kLinear}});
  engine.automation().set_lanes({lane});
  engine.automation().bind_target(4, &processor);

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);

  bool found = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found =
        found || (telemetry.type == sonare::engine::TelemetryType::kError &&
                  telemetry.error == sonare::engine::TelemetryErrorCode::kNonRealtimeSafeParameter);
  }
  REQUIRE(found);
}
