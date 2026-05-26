#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <string>

#include "automation/automation_engine.h"
#include "automation/parameter.h"

using Catch::Matchers::WithinAbs;

namespace {

class CaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}

  bool set_parameter(unsigned int param_id, float value) override {
    last_param = param_id;
    last_value = value;
    ++set_count;
    return true;
  }

  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id != 99;
  }

  unsigned int last_param = 0;
  float last_value = 0.0f;
  int set_count = 0;
};

}  // namespace

TEST_CASE("AutomationLane evaluates hold linear exponential and s-curve breakpoints",
          "[automation]") {
  sonare::automation::AutomationLane hold(1);
  hold.set_points({{0.0, 0.25f, sonare::automation::CurveType::kHold},
                   {1.0, 0.75f, sonare::automation::CurveType::kLinear}});
  REQUIRE_THAT(hold.value_at(0.5), WithinAbs(0.25f, 1.0e-6f));

  sonare::automation::AutomationLane linear(2);
  linear.set_points({{0.0, 0.0f, sonare::automation::CurveType::kLinear},
                     {1.0, 1.0f, sonare::automation::CurveType::kLinear}});
  REQUIRE_THAT(linear.value_at(0.25), WithinAbs(0.25f, 1.0e-6f));

  sonare::automation::AutomationLane exponential(3);
  exponential.set_points({{0.0, 1.0f, sonare::automation::CurveType::kExponential},
                          {1.0, 4.0f, sonare::automation::CurveType::kLinear}});
  REQUIRE_THAT(exponential.value_at(0.5), WithinAbs(2.0f, 1.0e-5f));

  sonare::automation::AutomationLane s_curve(4);
  s_curve.set_points({{0.0, 0.0f, sonare::automation::CurveType::kSCurve},
                      {1.0, 1.0f, sonare::automation::CurveType::kLinear}});
  REQUIRE_THAT(s_curve.value_at(0.5), WithinAbs(0.5f, 1.0e-6f));
  REQUIRE(s_curve.next_breakpoint_after(0.25) == 1.0);
}

TEST_CASE("AutomationEngine applies lane values through ProcessorBase set_parameter",
          "[automation]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::kLinear},
                   {1.0, 1.0f, sonare::automation::CurveType::kLinear}});

  CaptureProcessor processor;
  sonare::automation::AutomationEngine engine;
  engine.prepare(48000.0, &tempo);
  engine.set_lanes({lane});
  engine.acquire_lanes();
  engine.bind_target(7, &processor);

  sonare::transport::TransportState state{};
  state.sample_position = 0;
  engine.apply(state, 12000, 64);

  REQUIRE(processor.last_param == 7);
  REQUIRE(processor.set_count == 1);
  REQUIRE_THAT(processor.last_value, WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("AutomationEngine skips non realtime-safe parameters", "[automation]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(99);
  lane.set_points({{0.0, 0.1f, sonare::automation::CurveType::kLinear}});

  CaptureProcessor processor;
  sonare::automation::AutomationEngine engine;
  engine.prepare(48000.0, &tempo);
  engine.set_lanes({lane});
  engine.acquire_lanes();
  engine.bind_target(99, &processor);

  sonare::transport::TransportState state{};
  engine.apply(state, 0, 64);

  REQUIRE(processor.set_count == 0);
}

TEST_CASE("AutomationEngine collects breakpoint boundaries", "[automation]") {
  sonare::automation::AutomationLane first(1);
  first.set_points({{0.0, 0.0f, sonare::automation::CurveType::kLinear},
                    {1.0, 1.0f, sonare::automation::CurveType::kLinear},
                    {2.0, 0.0f, sonare::automation::CurveType::kLinear}});
  sonare::automation::AutomationLane second(2);
  second.set_points({{0.5, 0.0f, sonare::automation::CurveType::kLinear},
                     {1.0, 0.5f, sonare::automation::CurveType::kLinear}});

  sonare::automation::AutomationEngine engine;
  engine.set_lanes({first, second});
  engine.acquire_lanes();

  sonare::automation::AutomationBoundaryList boundaries;
  engine.collect_boundaries(0.25, 2.0, &boundaries);

  REQUIRE(boundaries.size == 3);
  REQUIRE(boundaries.ppq[0] == 0.5);
  REQUIRE(boundaries.ppq[1] == 1.0);
  REQUIRE(boundaries.ppq[2] == 2.0);
  REQUIRE_FALSE(boundaries.overflowed);
}

TEST_CASE("ParameterRegistry enumerates stable metadata", "[automation]") {
  sonare::automation::ParameterRegistry registry;
  REQUIRE(registry.add(
      {20, "gain", "dB", -60.0f, 12.0f, 0.0f, true, sonare::automation::CurveType::kLinear}));
  REQUIRE(registry.add(
      {10, "mode", "", 0.0f, 3.0f, 0.0f, false, sonare::automation::CurveType::kHold}));

  REQUIRE(registry.parameter_count() == 2);
  sonare::automation::ParameterInfo info{};
  REQUIRE(registry.parameter_info_by_index(0, &info));
  REQUIRE(info.id == 10);
  REQUIRE_FALSE(info.rt_safe);
  REQUIRE(registry.parameter_info(20, &info));
  REQUIRE(info.name == std::string("gain"));
  REQUIRE(registry.parameter_is_realtime_safe(20));
  REQUIRE_FALSE(registry.parameter_is_realtime_safe(10));
}
