#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mastering/utility/gain.h"
#include "mixing/channel_strip.h"
#include "no_alloc_test_helpers.h"
#include "rt/processor_base.h"

using Catch::Matchers::WithinAbs;

namespace {

// Records the order in which distinct parameter ids are first set.
class OrderProbeProcessor final : public sonare::rt::ProcessorBase {
 public:
  static constexpr unsigned int kParams = 40;

  OrderProbeProcessor() { order.reserve(kParams); }
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter_impl(unsigned int param_id, float) override {
    if (param_id >= kParams) return false;
    if (!seen[param_id]) {
      seen[param_id] = true;
      order.push_back(param_id);
    }
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id < kParams;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (unsigned int id = 0; id < kParams; ++id) out.push_back({"p" + std::to_string(id), id});
    return out;
  }

  std::array<bool, kParams> seen{};
  std::vector<unsigned int> order;
};

}  // namespace

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

TEST_CASE("Same-sample insert parameters apply in the order their lanes were scheduled",
          "[mixing][rt][automation]") {
  // Enough same-offset events that an unstable sort would reorder the ties.
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip;
  auto probe = std::make_unique<OrderProbeProcessor>();
  OrderProbeProcessor* observed = probe.get();
  strip.add_pre_insert(std::move(probe));
  strip.prepare(48000.0, kBlock);
  strip.settle();
  std::vector<unsigned int> scheduled;
  for (unsigned int i = 0; i < OrderProbeProcessor::kParams; ++i) {
    const unsigned int param = (i * 7u) % OrderProbeProcessor::kParams;
    REQUIRE(strip.schedule_insert_automation(0, param, 32, 1.0f,
                                             sonare::mixing::AutomationCurveType::Hold));
    // A later event per lane, descending across lanes, so the block really needs sorting.
    REQUIRE(strip.schedule_insert_automation(
        0, param, 33 + static_cast<int64_t>(OrderProbeProcessor::kParams - 1 - i), 2.0f,
        sonare::mixing::AutomationCurveType::Hold));
    scheduled.push_back(param);
  }
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right = left;
  float* channels[] = {left.data(), right.data()};
  strip.process_at(channels, 2, kBlock, 0);
  CHECK(observed->order == scheduled);
}
