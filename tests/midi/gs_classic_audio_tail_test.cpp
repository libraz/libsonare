/// @file gs_classic_audio_tail_test.cpp
/// @brief Audio-insert tail reporting for the GS classic realization.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/classic_unit.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "support/alloc_guard.h"

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using sonare::midi::synth::gs_efx_type_defaults;

constexpr std::array<uint16_t, 2> kTailTypes = {0x0150, 0x0155};
constexpr int kBlock = 256;
constexpr std::size_t kImpulseAt = 64;
constexpr std::size_t kRenderSamples = 96000;  // Two seconds at 48 kHz.
constexpr std::size_t kLateTailOffset = 4800;  // 100 ms after the impulse.

void set_type_defaults(gc::GsClassicUnit& unit, uint16_t type) {
  const auto* defaults = gs_efx_type_defaults(type);
  REQUIRE(defaults != nullptr);
  for (std::size_t slot = 0; slot < defaults->params.size(); ++slot) {
    REQUIRE(unit.set_parameter(static_cast<unsigned int>(slot),
                               static_cast<float>(defaults->params[slot])));
  }
}

std::vector<float> render_impulse(gc::GsClassicUnit& unit) {
  std::vector<float> left(kRenderSamples, 0.0f);
  std::vector<float> right(kRenderSamples, 0.0f);
  left[kImpulseAt] = 1.0f;
  right[kImpulseAt] = 1.0f;
  for (std::size_t offset = 0; offset < left.size(); offset += kBlock) {
    const int count = static_cast<int>(std::min<std::size_t>(kBlock, left.size() - offset));
    float* channels[2] = {left.data() + offset, right.data() + offset};
    unit.process(channels, 2, count);
  }
  return left;
}

}  // namespace

TEST_CASE("classic delay audio inserts expose their post-input state",
          "[midi][gs-classic][audio]") {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  for (const uint16_t type_number : kTailTypes) {
    const gc::GsClassicType* type = registry.find(type_number);
    REQUIRE(type != nullptr);
    gc::GsClassicUnit unit(registry.models(), *type);
    set_type_defaults(unit, type_number);
    unit.prepare(48000.0, kBlock);

    const std::vector<float> output = render_impulse(unit);
    double late_tail_energy = 0.0;
    for (std::size_t i = kImpulseAt + kLateTailOffset; i < output.size(); ++i) {
      late_tail_energy += static_cast<double>(output[i]) * output[i];
    }
    REQUIRE(late_tail_energy > 1.0e-8);
    REQUIRE(unit.tail_samples() > 0);
  }
}

TEST_CASE("classic audio insert tail allowance follows the host sample rate",
          "[midi][gs-classic][audio]") {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  for (const double sample_rate : {32000.0, 48000.0, 96000.0}) {
    const gc::GsClassicType* type = registry.find(kTailTypes.front());
    REQUIRE(type != nullptr);
    gc::GsClassicUnit unit(registry.models(), *type);
    REQUIRE(unit.tail_samples() == 480000);
    unit.prepare(sample_rate, kBlock);
    REQUIRE(unit.tail_samples() == static_cast<int>(sample_rate * 10.0));
    const int expected_tail = static_cast<int>(sample_rate * 10.0);
    int before_reset = 0;
    int after_reset = 0;
    std::size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      before_reset = unit.tail_samples();
      unit.reset();
      after_reset = unit.tail_samples();
      allocations = guard.count();
    }
    REQUIRE(before_reset == expected_tail);
    REQUIRE(after_reset == expected_tail);
    REQUIRE(allocations == 0);
  }
}

TEST_CASE("classic Thru keeps the zero tail contract", "[midi][gs-classic][audio]") {
  gc::GsClassicType thru{};
  thru.out_l = 0;
  thru.out_r = 1;
  thru.max_delay_samples = 1;
  gc::GsClassicModelSet models{};
  models.types = &thru;
  models.n_types = 1;
  gc::GsClassicUnit unit(models, thru);
  REQUIRE(unit.tail_samples() == 0);
  unit.prepare(96000.0, kBlock);
  REQUIRE(unit.tail_samples() == 0);
}
