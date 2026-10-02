#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <memory>
#include <vector>

#include "mastering/utility/gain.h"
#include "mixing/channel_strip.h"
#include "no_alloc_test_helpers.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("Concurrent insert ramps retain every target's final value",
          "[mixing][rt][workflow_followup]") {
  struct Scenario {
    bool post;
    bool muted;
    int block;
  };
  // Pairwise coverage of placement, mute state and prepared block width.
  const std::array<Scenario, 4> scenarios{
      {{false, false, 128}, {false, true, 512}, {true, false, 512}, {true, true, 128}}};
  for (const auto scenario : scenarios) {
    INFO("post=" << scenario.post << " muted=" << scenario.muted << " block=" << scenario.block);
    sonare::mixing::ChannelStrip strip;
    std::array<sonare::mastering::utility::Gain*, 3> gains{};
    for (auto& observed : gains) {
      auto gain = std::make_unique<sonare::mastering::utility::Gain>();
      observed = gain.get();
      if (scenario.post)
        strip.add_post_insert(std::move(gain));
      else
        strip.add_pre_insert(std::move(gain));
    }
    strip.prepare(48000.0, scenario.block);
    strip.set_muted(scenario.muted);
    strip.settle();
    for (unsigned int index = 0; index < gains.size(); ++index) {
      REQUIRE(strip.schedule_insert_automation(index, 0, 0, 0.0f,
                                               sonare::mixing::AutomationCurveType::Linear));
      REQUIRE(strip.schedule_insert_automation(index, 0, scenario.block - 1,
                                               4.0f + static_cast<float>(index),
                                               sonare::mixing::AutomationCurveType::Hold));
    }
    std::vector<float> left(static_cast<size_t>(scenario.block), 0.1f);
    std::vector<float> right = left;
    float* channels[] = {left.data(), right.data()};
    size_t allocations = 0;
    {
      AllocationGuard guard;
      strip.process_at(channels, 2, scenario.block, 0);
      allocations = guard.count();
    }
    CHECK(allocations == 0);
    for (size_t index = 0; index < gains.size(); ++index) {
      CHECK_THAT(gains[index]->config().level_db,
                 WithinAbs(4.0f + static_cast<float>(index), 1e-6f));
    }
    strip.process_at(channels, 2, scenario.block, scenario.block);
    for (size_t index = 0; index < gains.size(); ++index) {
      CHECK_THAT(gains[index]->config().level_db,
                 WithinAbs(4.0f + static_cast<float>(index), 1e-6f));
    }
  }
}

TEST_CASE("Maximum insert automation targets retain their final held values",
          "[mixing][rt][workflow_followup]") {
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip;
  std::vector<sonare::mastering::utility::Gain*> gains;
  for (size_t index = 0; index < sonare::mixing::ChannelStrip::kMaxInsertAutomationLanes; ++index) {
    auto gain = std::make_unique<sonare::mastering::utility::Gain>();
    gains.push_back(gain.get());
    strip.add_pre_insert(std::move(gain));
  }
  strip.prepare(48000.0, kBlock);
  for (unsigned int index = 0; index < gains.size(); ++index) {
    REQUIRE(strip.schedule_insert_automation(index, 0, 0, 0.0f,
                                             sonare::mixing::AutomationCurveType::Linear));
    REQUIRE(strip.schedule_insert_automation(index, 0, kBlock - 1, 1.0f,
                                             sonare::mixing::AutomationCurveType::Hold));
  }
  std::array<float, kBlock> left{}, right{};
  float* channels[] = {left.data(), right.data()};
  size_t allocations = 0;
  {
    AllocationGuard guard;
    strip.process_at(channels, 2, kBlock, 0);
    allocations = guard.count();
  }
  CHECK(allocations == 0);
  for (size_t index = 0; index < gains.size(); ++index) {
    INFO("insert=" << index);
    CHECK_THAT(gains[index]->config().level_db, WithinAbs(1.0f, 1e-6f));
  }
  strip.process_at(channels, 2, kBlock, kBlock);
  for (auto* gain : gains) CHECK_THAT(gain->config().level_db, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Same-sample insert updates keep the last authored value under overflow",
          "[mixing][rt][workflow_followup]") {
  constexpr int kBlock = 128;
  for (const int second_offset : {32, 64}) {
    INFO("second target offset=" << second_offset);
    sonare::mixing::ChannelStrip strip;
    std::array<sonare::mastering::utility::Gain*, 2> gains{};
    for (auto& observed : gains) {
      auto gain = std::make_unique<sonare::mastering::utility::Gain>();
      observed = gain.get();
      strip.add_pre_insert(std::move(gain));
    }
    strip.prepare(48000.0, kBlock);
    for (unsigned int target = 0; target < gains.size(); ++target) {
      for (int index = 0; index < 150; ++index) {
        REQUIRE(strip.schedule_insert_automation(target, 0, target == 0 ? 64 : second_offset,
                                                 static_cast<float>(index) / 100.0f,
                                                 sonare::mixing::AutomationCurveType::Hold));
      }
    }
    std::array<float, kBlock> left{}, right{};
    float* channels[] = {left.data(), right.data()};
    size_t allocations = 0;
    {
      AllocationGuard guard;
      strip.process_at(channels, 2, kBlock, 0);
      allocations = guard.count();
    }
    CHECK(allocations == 0);
    for (auto* observed : gains) CHECK_THAT(observed->config().level_db, WithinAbs(1.49f, 1e-6f));
    strip.process_at(channels, 2, kBlock, kBlock);
    for (auto* observed : gains) CHECK_THAT(observed->config().level_db, WithinAbs(1.49f, 1e-6f));
  }
}
