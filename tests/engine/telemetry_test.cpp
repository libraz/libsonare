#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "engine/realtime_engine.h"

namespace {

class TelemetryCaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    last_param = param_id;
    last_value = value;
    return true;
  }

  unsigned int last_param = 0;
  float last_value = 0.0f;
};

}  // namespace

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

TEST_CASE("RealtimeEngine spreads control boundaries across a large block without overflow",
          "[engine][telemetry]") {
  // Regression: a fixed 64-sample control cadence emits frames/64 boundaries,
  // overflowing the 32-slot boundary list for blocks > ~2048 samples and
  // freezing automation/smoothing in the tail. The cadence now widens with the
  // block so all control boundaries fit and the whole block is re-evaluated.
  constexpr int kBlock = 4096;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  TelemetryCaptureProcessor processor;
  sonare::automation::AutomationLane lane(11);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {16.0, 1.0f, sonare::automation::CurveType::Linear}});
  engine.automation().set_lanes({lane});
  engine.automation().bind_target(11, &processor);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> buffer(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {buffer.data()};
  engine.process(io, 1, kBlock);

  bool overflowed = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    overflowed =
        overflowed || (telemetry.type == sonare::engine::TelemetryType::kError &&
                       telemetry.error == sonare::engine::TelemetryErrorCode::kBoundaryOverflow);
  }
  REQUIRE_FALSE(overflowed);
  // Automation kept evaluating: the parameter was driven to its bound target.
  REQUIRE(processor.last_param == 11);
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
  lane.set_points({{0.0, 1.0f, sonare::automation::CurveType::Linear}});
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

TEST_CASE("RealtimeEngine records automation target binding overflow", "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);

  std::vector<TelemetryCaptureProcessor> processors(129);
  for (uint32_t id = 1; id <= 128; ++id) {
    REQUIRE(engine.automation().bind_target(id, &processors[id - 1]));
  }
  REQUIRE_FALSE(engine.automation().bind_target(129, &processors.back()));

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);

  bool found = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found = found ||
            (telemetry.type == sonare::engine::TelemetryType::kError &&
             telemetry.error == sonare::engine::TelemetryErrorCode::kAutomationBindTargetOverflow &&
             telemetry.value == 1);
  }
  REQUIRE(found);
}

TEST_CASE("RealtimeEngine records stale automation lane diagnostics", "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.25f, sonare::automation::CurveType::Linear}});
  engine.automation().set_lanes({lane});

  sonare::transport::TransportState state{};
  engine.automation().apply(state, 0, 64);

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);

  bool found = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found =
        found || (telemetry.type == sonare::engine::TelemetryType::kError &&
                  telemetry.error == sonare::engine::TelemetryErrorCode::kStaleAutomationLanes &&
                  telemetry.value == 1);
  }
  REQUIRE(found);
}

TEST_CASE("RealtimeEngine reports smoothed parameter slot saturation without unknown target",
          "[engine][telemetry]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  std::vector<TelemetryCaptureProcessor> processors(65);
  for (uint32_t id = 1; id <= 65; ++id) {
    REQUIRE(engine.automation().bind_target(id, &processors[id - 1]));
  }

  for (uint32_t id = 1; id <= 65; ++id) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSetParamSmoothed;
    command.sample_time = -1;
    command.target_id = id;
    command.arg.f = 1.0f;
    REQUIRE(engine.push_command(command));
  }

  std::array<float, 128> buffer{};
  float* io[] = {buffer.data()};
  engine.process(io, 1, 128);
  engine.process(io, 1, 128);

  bool found_capacity = false;
  bool found_unknown_for_65 = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    found_capacity =
        found_capacity ||
        (telemetry.type == sonare::engine::TelemetryType::kError &&
         telemetry.error == sonare::engine::TelemetryErrorCode::kSmoothedParameterCapacity &&
         telemetry.value == 65);
    found_unknown_for_65 = found_unknown_for_65 ||
                           (telemetry.type == sonare::engine::TelemetryType::kError &&
                            telemetry.error == sonare::engine::TelemetryErrorCode::kUnknownTarget &&
                            telemetry.value == 65);
  }
  REQUIRE(found_capacity);
  REQUIRE_FALSE(found_unknown_for_65);
  REQUIRE(processors.back().last_param == 65);
  REQUIRE(processors.back().last_value == 1.0f);
}
