#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

#include "engine/meter_telemetry.h"

namespace {

constexpr int kBlock = 64;
constexpr sonare::mixing::MeterConfig kLufsOnly{true, false, 4};

sonare::engine::MeterTelemetryRecord lightweight_record(std::array<float, kBlock> left,
                                                        std::array<float, kBlock> right) {
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 0, 8, kLufsOnly);
  float* channels[] = {left.data(), right.data()};
  tap.process_lightweight(channels, 2, kBlock, 0, 0xFFFFu);
  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  return record;
}

}  // namespace

TEST_CASE("Lightweight meter counts a non-finite sample as silence like the full meter",
          "[engine][meter_telemetry]") {
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  const sonare::engine::MeterTelemetryRecord clean = lightweight_record(left, right);

  std::array<float, kBlock> poisoned_left = left;
  std::array<float, kBlock> poisoned_right = right;
  poisoned_left[3] = std::numeric_limits<float>::infinity();
  poisoned_right[9] = std::numeric_limits<float>::quiet_NaN();
  // The two silenced cells are what the full meter would read in their place.
  std::array<float, kBlock> silenced_left = left;
  std::array<float, kBlock> silenced_right = right;
  silenced_left[3] = 0.0f;
  silenced_right[9] = 0.0f;
  const sonare::engine::MeterTelemetryRecord poisoned =
      lightweight_record(poisoned_left, poisoned_right);
  const sonare::engine::MeterTelemetryRecord silenced =
      lightweight_record(silenced_left, silenced_right);

  for (int ch = 0; ch < 2; ++ch) {
    const size_t c = static_cast<size_t>(ch);
    CHECK(std::isfinite(poisoned.peak_db[c]));
    CHECK(std::isfinite(poisoned.rms_db[c]));
    CHECK(poisoned.peak_db[c] == clean.peak_db[c]);
    CHECK(poisoned.rms_db[c] == silenced.rms_db[c]);
  }
  CHECK(std::isfinite(poisoned.correlation));
  CHECK(std::isfinite(poisoned.mono_compat_width));
  CHECK(poisoned.correlation == silenced.correlation);
  CHECK(poisoned.mono_compat_width == silenced.mono_compat_width);
}
