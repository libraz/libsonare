#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <string>
#include <vector>

#include "automation/automation_engine.h"
#include "automation/parameter.h"
#include "mixing/automation_lane.h"
#include "util/automation_curve.h"

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
  hold.set_points({{0.0, 0.25f, sonare::automation::CurveType::Hold},
                   {1.0, 0.75f, sonare::automation::CurveType::Linear}});
  REQUIRE_THAT(hold.value_at(0.5), WithinAbs(0.25f, 1.0e-6f));

  sonare::automation::AutomationLane linear(2);
  linear.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                     {1.0, 1.0f, sonare::automation::CurveType::Linear}});
  REQUIRE_THAT(linear.value_at(0.25), WithinAbs(0.25f, 1.0e-6f));

  sonare::automation::AutomationLane exponential(3);
  exponential.set_points({{0.0, 1.0f, sonare::automation::CurveType::Exponential},
                          {1.0, 4.0f, sonare::automation::CurveType::Linear}});
  REQUIRE_THAT(exponential.value_at(0.5), WithinAbs(2.0f, 1.0e-5f));

  sonare::automation::AutomationLane s_curve(4);
  s_curve.set_points({{0.0, 0.0f, sonare::automation::CurveType::SCurve},
                      {1.0, 1.0f, sonare::automation::CurveType::Linear}});
  REQUIRE_THAT(s_curve.value_at(0.5), WithinAbs(0.5f, 1.0e-6f));
  REQUIRE(s_curve.next_breakpoint_after(0.25) == 1.0);
}

TEST_CASE("Engine and mixer automation lanes share identical curve shapes", "[automation]") {
  // Regression: the mixer lane previously fell back to linear for any segment
  // whose endpoints were not both strictly positive, while the engine lane used
  // a signed-log domain. Both now route through interpolate_curve(), so a
  // negative-endpoint Exponential segment must produce the same shape on both.
  using sonare::AutomationCurve;
  using sonare::interpolate_curve;

  // Negative dB-domain endpoints, midpoint of an Exponential segment.
  const double engine_mid = interpolate_curve(AutomationCurve::Exponential, -1.0, -4.0, 0.5);
  REQUIRE(engine_mid < 0.0);  // stays negative, not linearised through 0
  REQUIRE_THAT(engine_mid, WithinAbs(-2.0, 1.0e-5));

  sonare::mixing::AutomationEvent a{};
  a.value = -1.0f;
  a.sample_pos = 0;
  a.curve = AutomationCurve::Exponential;
  sonare::mixing::AutomationEvent b{};
  b.value = -4.0f;
  b.sample_pos = 100;
  const float mixer_mid = sonare::mixing::interpolate_automation_value(a, b, 50.0);
  REQUIRE_THAT(static_cast<double>(mixer_mid), WithinAbs(engine_mid, 1.0e-5));
}

TEST_CASE("AutomationEngine applies lane values through ProcessorBase set_parameter",
          "[automation]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {1.0, 1.0f, sonare::automation::CurveType::Linear}});

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
  lane.set_points({{0.0, 0.1f, sonare::automation::CurveType::Linear}});

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

TEST_CASE("AutomationEngine reports target binding table overflow", "[automation]") {
  sonare::automation::AutomationEngine engine;
  std::vector<CaptureProcessor> processors(129);

  for (uint32_t id = 1; id <= 128; ++id) {
    REQUIRE(engine.bind_target(id, &processors[id - 1]));
  }
  REQUIRE_FALSE(engine.bind_target(129, &processors.back()));
  REQUIRE(engine.bind_target_overflow_count() == 1);
  REQUIRE_FALSE(engine.bind_target(0, &processors.front()));
  REQUIRE_FALSE(engine.bind_target(130, nullptr));
}

TEST_CASE("AutomationEngine reports apply before lane acquisition", "[automation]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.25f, sonare::automation::CurveType::Linear}});

  sonare::automation::AutomationEngine engine;
  engine.prepare(48000.0, &tempo);
  engine.set_lanes({lane});

  sonare::transport::TransportState state{};
  engine.apply(state, 0, 64);

  REQUIRE(engine.stale_lane_apply_count() == 1);
  engine.acquire_lanes();
  engine.apply(state, 0, 64);
  REQUIRE(engine.stale_lane_apply_count() == 1);
}

TEST_CASE("AutomationEngine collects breakpoint boundaries", "[automation]") {
  sonare::automation::AutomationLane first(1);
  first.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                    {1.0, 1.0f, sonare::automation::CurveType::Linear},
                    {2.0, 0.0f, sonare::automation::CurveType::Linear}});
  sonare::automation::AutomationLane second(2);
  second.set_points({{0.5, 0.0f, sonare::automation::CurveType::Linear},
                     {1.0, 0.5f, sonare::automation::CurveType::Linear}});

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

TEST_CASE("AutomationEngine resolves targets bound after an interior gap",
          "[automation][regression]") {
  // Regression: target_for() used to stop scanning at the first null/cleared
  // slot. After unbinding an interior target (leaving bound_count_ untouched),
  // a parameter bound to a slot AFTER the gap must still be resolved/applied.
  CaptureProcessor p10;
  CaptureProcessor p20;
  CaptureProcessor p30;

  sonare::automation::AutomationEngine engine;
  REQUIRE(engine.bind_target(10, &p10));
  REQUIRE(engine.bind_target(20, &p20));
  REQUIRE(engine.bind_target(30, &p30));

  // Punch an interior gap at the slot that held param 20.
  REQUIRE(engine.unbind_target(20));

  const uint32_t unknown_before = engine.unknown_target_count();

  // Param 30 lives in a slot after the gap; it must still be resolved/applied.
  REQUIRE(engine.set_parameter(30, 0.42f));
  REQUIRE(p30.set_count == 1);
  REQUIRE(p30.last_param == 30);
  REQUIRE_THAT(p30.last_value, WithinAbs(0.42f, 1.0e-6f));

  // Resolving the target must not have been counted as unknown.
  REQUIRE(engine.unknown_target_count() == unknown_before);

  // The cleared slot's param genuinely no longer resolves.
  REQUIRE_FALSE(engine.set_parameter(20, 0.5f));
  REQUIRE(p20.set_count == 0);
  REQUIRE(engine.unknown_target_count() == unknown_before + 1);
}

TEST_CASE("ParameterRegistry enumerates stable metadata", "[automation]") {
  sonare::automation::ParameterRegistry registry;
  REQUIRE(registry.add(
      {20, "gain", "dB", -60.0f, 12.0f, 0.0f, true, sonare::automation::CurveType::Linear}));
  REQUIRE(
      registry.add({10, "mode", "", 0.0f, 3.0f, 0.0f, false, sonare::automation::CurveType::Hold}));

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
