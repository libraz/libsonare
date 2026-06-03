#include "engine/mixing_runtime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "automation/automation_engine.h"
#include "rt/processor_base.h"

namespace {

class TailGainProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TailGainProcessor(float gain, int tail_samples)
      : gain_(gain), tail_samples_(tail_samples) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    ++process_count;
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) channels[ch][i] *= gain_;
    }
  }
  void reset() override { ++reset_count; }
  int tail_samples() const noexcept override { return tail_samples_; }

  int process_count = 0;
  int reset_count = 0;

 private:
  float gain_ = 1.0f;
  int tail_samples_ = 0;
};

}  // namespace

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
  lane.set_points({{0.0, -12.0f, sonare::automation::CurveType::Linear}});

  sonare::automation::AutomationEngine automation;
  automation.prepare(48000.0, &tempo);
  automation.set_lanes({lane});
  automation.acquire_lanes();
  automation.bind_target(sonare::engine::MixingRuntime::kFaderDb, &runtime);

  sonare::transport::TransportState state{};
  automation.apply(state, 0, 64);

  REQUIRE(strip.fader_db() == -12.0f);
}

TEST_CASE("ChannelStrip insert bypass preserves dry signal and reports tail",
          "[engine][mixing_runtime]") {
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto insert = std::make_unique<TailGainProcessor>(0.25f, 128);
  TailGainProcessor* raw_insert = insert.get();
  strip.add_pre_insert(std::move(insert));
  strip.prepare(48000.0, 8);
  REQUIRE(strip.tail_samples() == 128);

  std::array<float, 4> left{};
  std::array<float, 4> right{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  strip.process(channels, 2, 4);
  REQUIRE(left[0] == 0.25f);
  REQUIRE(right[3] == 0.25f);
  REQUIRE(raw_insert->process_count == 1);

  REQUIRE(raw_insert->set_bypassed(true, true));
  REQUIRE(raw_insert->reset_count == 1);
  left.fill(1.0f);
  right.fill(1.0f);
  strip.process(channels, 2, 4);
  REQUIRE(left[0] == 1.0f);
  REQUIRE(right[3] == 1.0f);
  REQUIRE(raw_insert->process_count == 1);
}
