#include "engine/monitor_runtime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("MonitorRuntime computes solo OR exclusive solo and solo-safe mute",
          "[engine][monitor_runtime]") {
  sonare::mixing::ChannelStrip vocal({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::mixing::ChannelStrip drums({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::mixing::ChannelStrip reverb({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});

  sonare::engine::MonitorRuntime runtime;
  runtime.prepare(48000.0, 32, 1.0f);
  REQUIRE(runtime.add_strip(&vocal));
  REQUIRE(runtime.add_strip(&drums));
  REQUIRE(runtime.add_strip(&reverb));

  runtime.set_solo_safe(2, true);
  runtime.set_solo(0, true);
  REQUIRE_FALSE(runtime.implied_mute(0));
  REQUIRE(runtime.implied_mute(1));
  REQUIRE_FALSE(runtime.implied_mute(2));

  runtime.set_solo(1, true);
  REQUIRE_FALSE(runtime.implied_mute(0));
  REQUIRE_FALSE(runtime.implied_mute(1));

  runtime.set_exclusive_solo(1, true);
  REQUIRE_FALSE(runtime.soloed(0));
  REQUIRE(runtime.soloed(1));
  REQUIRE(runtime.implied_mute(0));
  REQUIRE_FALSE(runtime.implied_mute(1));
}

TEST_CASE("MonitorRuntime smooths mute gain instead of hard muting", "[engine][monitor_runtime]") {
  constexpr int kBlock = 32;
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kBlock);

  sonare::engine::MonitorRuntime runtime;
  runtime.prepare(48000.0, kBlock, 5.0f);
  REQUIRE(runtime.add_strip(&strip));
  runtime.set_mute(0, true);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  runtime.process_strip(0, channels, 2, kBlock, 0);

  REQUIRE(left[0] > left[kBlock - 1]);
  REQUIRE(left[0] > 0.0f);
  REQUIRE(left[kBlock - 1] > 0.0f);
}

TEST_CASE("MonitorRuntime PFL and AFL taps are taken at different stages",
          "[engine][monitor_runtime]") {
  constexpr int kBlock = 8;
  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kBlock);

  sonare::engine::MonitorRuntime runtime;
  runtime.prepare(48000.0, kBlock, 0.0f);
  REQUIRE(runtime.add_strip(&strip));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  std::array<float, kBlock> mon_l{};
  std::array<float, kBlock> mon_r{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  float* monitor[] = {mon_l.data(), mon_r.data()};

  runtime.set_monitor_mode(0, sonare::engine::MonitorMode::kPfl);
  runtime.process_strip(0, channels, 2, kBlock, 0, monitor);
  REQUIRE(mon_l[0] == 1.0f);

  left.fill(1.0f);
  right.fill(1.0f);
  mon_l.fill(0.0f);
  mon_r.fill(0.0f);
  runtime.set_monitor_mode(0, sonare::engine::MonitorMode::kAfl);
  runtime.process_strip(0, channels, 2, kBlock, kBlock, monitor);
  REQUIRE(mon_l[kBlock - 1] > 0.49f);
  REQUIRE(mon_l[kBlock - 1] < 0.51f);
}
