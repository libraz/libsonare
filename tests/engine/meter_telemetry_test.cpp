#include "engine/meter_telemetry.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "metering/lufs.h"

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
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, 2, kSampleRate);
  REQUIRE(latest.target_id == 42);
  REQUIRE(latest.peak_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(latest.rms_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(latest.integrated_lufs == Approx(offline.integrated_lufs).margin(0.7f));
  REQUIRE(latest.correlation == Approx(1.0f).margin(0.001f));

  std::array<sonare::mixing::GoniometerPoint, 8> points{};
  REQUIRE(tap.read_goniometer(points.data(), points.size()) > 0);
}

TEST_CASE("MeterTelemetryTap drops newest record and counts drops when full",
          "[engine][meter_telemetry]") {
  // Race-safe contract: the producer (audio thread) never pops -- pop() is the
  // consumer role owned by the host. So when the SPSC queue is full the newest
  // record is dropped and accounted for, while already-queued (older) records
  // remain intact. The accumulated drop count is propagated to the host on the
  // next record that pushes successfully (after the host has drained a slot).
  // The freshest meter *value* is independently available via the meter seqlock
  // snapshot, so dropping newest telemetry records does not stall live meters.
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 7, 1);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  tap.process(channels, 2, kBlock, 0);           // record 0 -> queued (now full)
  tap.process(channels, 2, kBlock, kBlock);      // full -> newest dropped (1)
  tap.process(channels, 2, kBlock, kBlock * 2);  // full -> newest dropped (2)

  // The surviving record is the oldest; it predates any drop so its own
  // dropped_records snapshot is still zero.
  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.render_frame == 0);
  REQUIRE(record.dropped_records == 0);
  REQUIRE_FALSE(tap.pop(record));

  // After draining a slot the next push succeeds and carries the accumulated
  // drop count so the host learns exactly how many records were lost.
  tap.process(channels, 2, kBlock, kBlock * 3);
  REQUIRE(tap.pop(record));
  REQUIRE(record.render_frame == kBlock * 3);
  REQUIRE(record.dropped_records == 2);
  REQUIRE_FALSE(tap.pop(record));
}

TEST_CASE("MeterTelemetryTap publishes lightweight target records", "[engine][meter_telemetry]") {
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 0, 8);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.5f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};
  tap.process_lightweight(channels, 2, kBlock, 512, 0xFFFFu);

  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.target_id == 0xFFFFu);
  REQUIRE(record.render_frame == 512);
  REQUIRE(record.peak_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(record.rms_db[1] == Approx(-12.0412f).margin(0.01f));
  REQUIRE(record.correlation == Approx(-1.0f).margin(0.001f));
  REQUIRE(std::isnan(record.integrated_lufs));
  REQUIRE_FALSE(tap.pop(record));
}
