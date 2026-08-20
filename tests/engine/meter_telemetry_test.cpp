#include "engine/meter_telemetry.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "metering/lufs.h"
#include "util/constants.h"

namespace {

using Catch::Approx;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 128;

/// LUFS metering with true peak off. Cases that assert peak, RMS, LUFS or ring
/// behaviour do not need the reconstruction, and the ones below that drive
/// process_lightweight() never reach the MeterProcessor at all; both state the
/// config anyway so enabling true peak stays an explicit act.
constexpr sonare::mixing::MeterConfig kLufsOnly{true, false, 4};

}  // namespace

TEST_CASE("MeterTelemetryTap publishes peak RMS LUFS and goniometer data",
          "[engine][meter_telemetry]") {
  constexpr int kFrames = kSampleRate * 3;
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(kSampleRate, kBlock, 42, 256, kLufsOnly);

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
  tap.prepare(48000.0, kBlock, 7, 1, kLufsOnly);

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
  tap.prepare(48000.0, kBlock, 0, 8, kLufsOnly);

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
  // Finiteness alone would pass on any value, so the fields this path leaves
  // unmeasured are pinned to the floor: the true-peak fields in particular are
  // the ones that silently read -120 for every signal when a tap is prepared
  // without true-peak measurement, and an isfinite check cannot tell the two
  // apart. Both planes are checked because the lightweight path writes neither.
  REQUIRE(std::isfinite(record.integrated_lufs));
  REQUIRE(record.integrated_lufs == Approx(sonare::constants::kFloorDb));
  REQUIRE(std::isfinite(record.momentary_lufs));
  REQUIRE(std::isfinite(record.short_term_lufs));
  REQUIRE(std::isfinite(record.max_true_peak_db));
  REQUIRE(record.max_true_peak_db == Approx(sonare::constants::kFloorDb));
  REQUIRE(std::isfinite(record.true_peak_db[0]));
  REQUIRE(record.true_peak_db[0] == Approx(sonare::constants::kFloorDb));
  REQUIRE(std::isfinite(record.true_peak_db[1]));
  REQUIRE(record.true_peak_db[1] == Approx(sonare::constants::kFloorDb));
  REQUIRE(std::isfinite(record.gain_reduction_db));
  REQUIRE_FALSE(tap.pop(record));
}

TEST_CASE("MeterTelemetryTap lightweight mono input floors the unused plane",
          "[engine][meter_telemetry]") {
  // A mono lane writes only plane 0; the right plane must report silence
  // (the dB floor), never an uninitialized 0 dBFS that pins the meter to clip.
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 0, 8, kLufsOnly);

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
  tap.prepare(48000.0, kBlock, 0, 8, kLufsOnly);

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

TEST_CASE("MeterTelemetryTap lightweight mono_compat_width matches the full meter",
          "[engine][meter_telemetry]") {
  // The lightweight tap derives mono_compat_width from the same mid/side energy
  // ratio as the full MeterProcessor (mixing::mono_compat_width_from_energy), not
  // the cheaper 1 - |correlation| proxy. For a decorrelated stereo pair the two
  // surfaces must report the same width.
  constexpr int kFrames = 2048;
  std::vector<float> left(kFrames);
  std::vector<float> right(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const float phase = sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                        static_cast<float>(kSampleRate);
    left[static_cast<size_t>(i)] = 0.3f * std::sin(phase);
    // Quadrature partner -> correlation ~0, a nontrivial mid/side width.
    right[static_cast<size_t>(i)] = 0.3f * std::cos(phase);
  }

  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(kSampleRate, kFrames, 7, 8, kLufsOnly);
  float* channels[] = {left.data(), right.data()};
  tap.process_lightweight(channels, 2, kFrames, 0, 7);
  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));

  sonare::mixing::MeterConfig config;
  config.measure_lufs = false;
  sonare::mixing::MeterProcessor full(config);
  full.prepare(kSampleRate, kFrames);
  full.process(channels, 2, kFrames);
  const auto snapshot = full.snapshot();

  REQUIRE(record.mono_compat_width == Approx(snapshot.mono_compat_width).margin(1e-4f));
  REQUIRE(record.mono_compat_width > 0.5f);
}

TEST_CASE("MeterTelemetryTap merges sub-block peak and RMS across a host block",
          "[engine][meter_telemetry]") {
  // A host block that automation splits into several process() calls between
  // begin_block()/end_block() must publish ONE record describing the WHOLE
  // block: peak_db is the max over every sub-block, rms_db is the RMS over the
  // concatenation of all sub-blocks. Put the transient in the FIRST sub-block
  // and silence in the last one so a "keep only the last fragment" bug (which
  // would report the floor peak and zero RMS) is caught.
  constexpr int kSubBlock = 64;
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(kSampleRate, kSubBlock, 99, 8, kLufsOnly);

  std::array<float, kSubBlock> loud{};
  loud.fill(1.0f);  // 0 dBFS transient confined to the first sub-block
  std::array<float, kSubBlock> silent{};
  silent.fill(0.0f);
  float* loud_channels[] = {loud.data(), loud.data()};
  float* silent_channels[] = {silent.data(), silent.data()};

  tap.begin_block();
  tap.process(loud_channels, 2, kSubBlock, 0);            // sub-block 1: transient
  tap.process(silent_channels, 2, kSubBlock, kSubBlock);  // sub-block 2: silence
  tap.end_block();

  sonare::engine::MeterTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE_FALSE(tap.pop(record));

  // Without merging, end_block() would publish only the second (silent)
  // sub-block and this peak would sit at the dB floor.
  REQUIRE(record.peak_db[0] == Approx(0.0f).margin(0.01f));

  // Half the block is full-scale and half is silent; the combined RMS must
  // reflect both halves rather than just the last one.
  const double expected_rms_db = 10.0 * std::log10(0.5);
  REQUIRE(record.rms_db[0] == Approx(static_cast<float>(expected_rms_db)).margin(0.05f));
}

TEST_CASE("MeterTelemetryTap reports an inter-sample peak when configured for true peak",
          "[engine][meter_telemetry][truepeak]") {
  // A sine at a quarter of the sample rate, offset by an eighth of a period:
  // every sample lands on +-amplitude/sqrt(2) while the waveform they encode
  // reaches +-amplitude, so the true peak sits 3.01 dB above the sample peak by
  // construction. A tap that published the sample peak under the true-peak name
  // could not produce that gap, which is what makes this non-vacuous.
  constexpr float kAmplitude = 0.5f;
  constexpr int kBlocks = 16;
  const int frames = kBlock * kBlocks;

  std::vector<float> tone(static_cast<size_t>(frames), 0.0f);
  for (int i = 0; i < frames; ++i) {
    tone[static_cast<size_t>(i)] =
        kAmplitude *
        std::sin(static_cast<float>(sonare::constants::kTwoPi) * 0.25f * static_cast<float>(i) +
                 static_cast<float>(sonare::constants::kPi) * 0.25f);
  }

  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(kSampleRate, kBlock, 5, 64, sonare::mixing::MeterConfig{true, true, 4});

  sonare::engine::MeterTelemetryRecord latest{};
  sonare::engine::MeterTelemetryRecord record{};
  for (int block = 0; block < kBlocks; ++block) {
    float* channels[] = {tone.data() + block * kBlock, tone.data() + block * kBlock};
    tap.process(channels, 2, kBlock, block * kBlock);
    while (tap.pop(record)) latest = record;
  }

  const float sample_peak_db = 20.0f * std::log10(kAmplitude / std::sqrt(2.0f));
  REQUIRE(latest.peak_db[0] == Approx(sample_peak_db).margin(0.05f));
  REQUIRE(latest.true_peak_db[0] > latest.peak_db[0] + 1.5f);
  REQUIRE(latest.true_peak_db[1] > latest.peak_db[1] + 1.5f);
  REQUIRE(latest.max_true_peak_db >= latest.true_peak_db[0]);
  // The reconstruction must not invent headroom either: the analog envelope is
  // the amplitude, and the FIR's ripple keeps the reading just under it.
  REQUIRE(latest.max_true_peak_db < 20.0f * std::log10(kAmplitude) + 0.5f);
  REQUIRE(std::isfinite(latest.max_true_peak_db));

  // Silence must still floor rather than report the previous block's peak.
  std::vector<float> silence(static_cast<size_t>(frames), 0.0f);
  sonare::engine::MeterTelemetryTap silent_tap;
  silent_tap.prepare(kSampleRate, kBlock, 5, 64, sonare::mixing::MeterConfig{true, true, 4});
  for (int block = 0; block < kBlocks; ++block) {
    float* channels[] = {silence.data(), silence.data()};
    silent_tap.process(channels, 2, kBlock, block * kBlock);
    while (silent_tap.pop(record)) latest = record;
  }
  REQUIRE(latest.true_peak_db[0] == Approx(sonare::constants::kFloorDb));
  REQUIRE(latest.max_true_peak_db == Approx(sonare::constants::kFloorDb));
}
