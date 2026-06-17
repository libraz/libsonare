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

  // Unmeasured fields must be finite (JSON-safe), not NaN, and sit at the dB
  // floor so a host can serialize the record without producing invalid JSON.
  REQUIRE(std::isfinite(record.integrated_lufs));
  REQUIRE(record.integrated_lufs == Approx(sonare::constants::kFloorDb));
  REQUIRE(std::isfinite(record.momentary_lufs));
  REQUIRE(std::isfinite(record.short_term_lufs));
  REQUIRE(std::isfinite(record.max_true_peak_db));
  REQUIRE(std::isfinite(record.true_peak_db[0]));
  REQUIRE(std::isfinite(record.gain_reduction_db));
  REQUIRE_FALSE(tap.pop(record));
}

TEST_CASE("MeterTelemetryTap lightweight mono input floors the unused plane",
          "[engine][meter_telemetry]") {
  // A mono lane writes only plane 0; the right plane must report silence
  // (the dB floor), never an uninitialized 0 dBFS that pins the meter to clip.
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 0, 8);

  std::array<float, kBlock> mono{};
  mono.fill(0.5f);
  float* channels[] = {mono.data()};
  tap.process_lightweight(channels, 1, kBlock, 0, 0xFFFFu);

  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.channel_count == 1);
  REQUIRE(record.peak_db[0] == Approx(-6.0206f).margin(0.01f));
  REQUIRE(record.peak_db[1] == Approx(sonare::constants::kFloorDb));
  REQUIRE(record.rms_db[1] == Approx(sonare::constants::kFloorDb));
}

TEST_CASE("MeterTelemetryTap lightweight seq advances monotonically", "[engine][meter_telemetry]") {
  // The lightweight path carries its own counter; consecutive records must have
  // strictly increasing seq so host-side change/drop detection works.
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 0, 8);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.1f);
  right.fill(0.1f);
  float* channels[] = {left.data(), right.data()};

  tap.process_lightweight(channels, 2, kBlock, 0, 0xFFFFu);
  tap.process_lightweight(channels, 2, kBlock, kBlock, 0xFFFFu);
  tap.process_lightweight(channels, 2, kBlock, kBlock * 2, 0xFFFFu);

  sonare::engine::MeterTelemetryRecord a{};
  sonare::engine::MeterTelemetryRecord b{};
  sonare::engine::MeterTelemetryRecord c{};
  REQUIRE(tap.pop(a));
  REQUIRE(tap.pop(b));
  REQUIRE(tap.pop(c));
  REQUIRE(b.seq > a.seq);
  REQUIRE(c.seq > b.seq);
}
