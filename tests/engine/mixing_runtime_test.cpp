#include "engine/mixing_runtime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "automation/automation_engine.h"

TEST_CASE("MixingRuntime drives ChannelStrip process_at and RT-safe parameters",
          "[engine][mixing_runtime]") {
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::engine::MixingRuntime runtime;
  REQUIRE(runtime.bind(&strip));
  runtime.prepare(48000.0, 8);

  REQUIRE(runtime.set_parameter(sonare::engine::MixingRuntime::kFaderDb, -6.0f));
  REQUIRE(runtime.set_parameter(sonare::engine::MixingRuntime::kPan, 0.0f));
  REQUIRE(runtime.set_parameter(sonare::engine::MixingRuntime::kWidth, 1.0f));
  REQUIRE(runtime.parameter_is_realtime_safe(sonare::engine::MixingRuntime::kFaderDb));
  REQUIRE_FALSE(runtime.parameter_is_realtime_safe(999));

  std::array<float, 8> left{};
  std::array<float, 8> right{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  runtime.process_at(channels, 2, 8, 1200);

  REQUIRE(left[7] > 0.49f);
  REQUIRE(left[7] < 0.51f);
  REQUIRE(right[7] > 0.49f);
  REQUIRE(right[7] < 0.51f);
}

TEST_CASE("AutomationEngine can drive MixingRuntime fader parameter",
          "[engine][mixing_runtime][automation]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::engine::MixingRuntime runtime;
  REQUIRE(runtime.bind(&strip));
  runtime.prepare(48000.0, 128);

  sonare::automation::AutomationLane lane(sonare::engine::MixingRuntime::kFaderDb);
  lane.set_points({{0.0, -12.0f, sonare::automation::CurveType::kLinear}});

  sonare::automation::AutomationEngine automation;
  automation.prepare(48000.0, &tempo);
  automation.set_lanes({lane});
  automation.acquire_lanes();
  automation.bind_target(sonare::engine::MixingRuntime::kFaderDb, &runtime);

  sonare::transport::TransportState state{};
  automation.apply(state, 0, 64);

  REQUIRE(strip.fader_db() == -12.0f);
}
