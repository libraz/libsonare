#include "engine/meter_telemetry.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "analysis/meter/lufs.h"

namespace {

using Catch::Approx;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 128;

}  // namespace

TEST_CASE("MeterTelemetryTap publishes peak RMS LUFS and goniometer data",
          "[engine][meter_telemetry]") {
  constexpr int kFrames = kSampleRate * 3;
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(kSampleRate, kBlock, 42, 256);

  std::vector<float> left(kFrames, 0.5f);
  std::vector<float> right(kFrames, 0.5f);
  std::vector<float> interleaved(static_cast<size_t>(kFrames) * 2);
  for (int i = 0; i < kFrames; ++i) {
    interleaved[static_cast<size_t>(i) * 2] = left[static_cast<size_t>(i)];
    interleaved[static_cast<size_t>(i) * 2 + 1] = right[static_cast<size_t>(i)];
  }

  for (int offset = 0; offset < kFrames; offset += kBlock) {
    float* channels[] = {left.data() + offset, right.data() + offset};
    tap.process(channels, 2, kBlock, offset);
  }

  sonare::engine::MeterTelemetryRecord latest{};
  sonare::engine::MeterTelemetryRecord record{};
  while (tap.pop(record)) {
    latest = record;
  }

  const auto offline =
      sonare::analysis::meter::lufs_interleaved(interleaved.data(), kFrames, 2, kSampleRate);
  REQUIRE(latest.target_id == 42);
  REQUIRE(latest.peak_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(latest.rms_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(latest.integrated_lufs == Approx(offline.integrated_lufs).margin(0.7f));
  REQUIRE(latest.correlation == Approx(1.0f).margin(0.001f));

  std::array<sonare::mixing::GoniometerPoint, 8> points{};
  REQUIRE(tap.read_goniometer(points.data(), points.size()) > 0);
}

TEST_CASE("MeterTelemetryTap keeps latest record when telemetry queue is full",
          "[engine][meter_telemetry]") {
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 7, 1);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  tap.process(channels, 2, kBlock, 0);
  tap.process(channels, 2, kBlock, kBlock);
  tap.process(channels, 2, kBlock, kBlock * 2);

  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.render_frame == kBlock * 2);
  REQUIRE(record.dropped_records > 0);
  REQUIRE_FALSE(tap.pop(record));
}
