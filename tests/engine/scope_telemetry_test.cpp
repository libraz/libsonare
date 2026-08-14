/// @file scope_telemetry_test.cpp
/// @brief Scope telemetry FFT band calibration: absolute band levels must be
///        independent of the host block size (the band power is normalized by
///        the coherent gain of the window actually applied, not a fixed
///        2/n_fft factor that under-read short blocks by ~20*log10(m/n_fft) dB).

#include "engine/scope_telemetry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_MIXING)
#include "engine/realtime_engine.h"
#endif

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kNfft = 2048;
constexpr uint32_t kBands = 32;

// Drives a full-scale tone through a freshly prepared tap at the given block
// size and returns {peak band index, peak band level dB}.
std::pair<uint32_t, float> peak_band(int block, double freq) {
  sonare::engine::ScopeTelemetryTap tap;
  tap.prepare(kSampleRate, kNfft, 16, kNfft, kBands);

  std::vector<float> buf(static_cast<size_t>(block));
  for (int i = 0; i < block; ++i) {
    buf[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(2.0 * M_PI * freq * static_cast<double>(i) / kSampleRate));
  }
  float* channels[2] = {buf.data(), buf.data()};
  tap.begin_block(block, block);  // interval == block -> capture this block
  tap.process(channels, 2, block, 0, 7);
  tap.end_block();

  sonare::engine::ScopeTelemetryRecord rec{};
  REQUIRE(tap.pop(rec));
  uint32_t pk = 0;
  for (uint32_t b = 1; b < rec.band_count; ++b) {
    if (rec.bands[b] > rec.bands[pk]) pk = b;
  }
  return {pk, rec.bands[pk]};
}

}  // namespace

TEST_CASE("ScopeTelemetryTap band level is correctly calibrated for a full-scale tone",
          "[engine][scope_telemetry]") {
  // Block size == FFT size so the window covers the whole frame (no
  // zero-padding/leakage-width confound): the band level then reflects the true
  // calibration. A full-scale tone recovers ~0 dBFS at its peak bin; spread over
  // a ~32-bin band by the Hann main lobe the band-averaged level lands near
  // -13 dB. The old 2/n_fft normalization (instead of 2/sum(window)) read ~6 dB
  // low here (~-19 dB), so the [-16, -10] window confirms the fix and fails the
  // regression.
  constexpr double kFreq = 3375.0;            // centered in a band (bin ~144 of 2048).
  const auto full = peak_band(kNfft, kFreq);  // m == n_fft
  REQUIRE(full.second > -16.0f);
  REQUIRE(full.second < -10.0f);

  // Coherent-gain normalization recovers the peak amplitude regardless of block
  // size, so a short block no longer collapses to the old ~12 dB systematic
  // deficit (leakage only redistributes energy across bins within the band).
  const auto short_block = peak_band(512, kFreq);
  REQUIRE(short_block.first == full.first);
  REQUIRE(short_block.second > -18.0f);
}

TEST_CASE("ScopeTelemetryTap publishes one record per target per host block",
          "[engine][scope_telemetry]") {
  sonare::engine::ScopeTelemetryTap tap;
  tap.prepare(kSampleRate, 128, 8, 128, kBands);
  std::array<float, 64> first{};
  std::array<float, 64> second{};
  second.fill(0.5f);
  float* first_channels[] = {first.data()};
  float* second_channels[] = {second.data()};

  REQUIRE(tap.begin_block(128, 128));
  tap.process(first_channels, 1, 64, 0, 7);
  tap.process(second_channels, 1, 64, 64, 7);
  tap.process(second_channels, 1, 64, 64, 8);
  tap.end_block();

  sonare::engine::ScopeTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.target_id == 7);
  REQUIRE(tap.pop(record));
  REQUIRE(record.target_id == 8);
  REQUIRE_FALSE(tap.pop(record));
}

namespace {

sonare::engine::ScopeTelemetryRecord master_scope(bool split) {
  constexpr int kFrames = 128;
  sonare::engine::ScopeTelemetryTap tap;
  tap.prepare(kSampleRate, kFrames, 8, kFrames, kBands);

  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  for (int i = 0; i < kFrames; ++i) {
    left[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(2.0 * M_PI * 750.0 * i / kSampleRate));
    right[static_cast<size_t>(i)] =
        static_cast<float>(std::cos(2.0 * M_PI * 750.0 * i / kSampleRate));
  }
  float* channels[] = {left.data(), right.data()};

  REQUIRE(tap.begin_block(kFrames, kFrames));
  if (split) {
    tap.append_master_pre_metronome(channels, 2, kFrames / 2, 101);
    float* tail[] = {left.data() + kFrames / 2, right.data() + kFrames / 2};
    tap.append_master_pre_metronome(tail, 2, kFrames / 2, 101 + kFrames / 2);
  } else {
    tap.append_master_pre_metronome(channels, 2, kFrames, 101);
  }
  tap.end_block();

  sonare::engine::ScopeTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.target_id == 0);
  REQUIRE_FALSE(tap.pop(record));
  return record;
}

}  // namespace

TEST_CASE("ScopeTelemetryTap keeps split and unsplit master snapshots identical",
          "[engine][scope_telemetry]") {
  const auto whole = master_scope(false);
  const auto split = master_scope(true);

  REQUIRE(split.render_frame == whole.render_frame);
  REQUIRE(split.band_count == whole.band_count);
  for (uint32_t i = 0; i < whole.band_count; ++i) {
    REQUIRE(split.bands[i] == whole.bands[i]);
  }
  REQUIRE(split.point_count == whole.point_count);
  for (uint32_t i = 0; i < whole.point_count; ++i) {
    REQUIRE(split.points[i].left == whole.points[i].left);
    REQUIRE(split.points[i].right == whole.points[i].right);
  }
}

TEST_CASE("ScopeTelemetryTap drops only an overflowing master accumulator",
          "[engine][scope_telemetry]") {
  constexpr int kFrames = 64;
  sonare::engine::ScopeTelemetryTap tap;
  tap.prepare(kSampleRate, kFrames, 8, kFrames, kBands);

  std::array<float, kFrames> samples{};
  float* channels[] = {samples.data(), nullptr};
  REQUIRE(tap.begin_block(kFrames, kFrames));
  tap.process(channels, 1, kFrames, 0, 7);
  tap.append_master_pre_metronome(channels, 2, kFrames, 0);
  tap.append_master_pre_metronome(channels, 2, 1, kFrames);
  tap.end_block();

  sonare::engine::ScopeTelemetryRecord record{};
  REQUIRE(tap.pop(record));
  REQUIRE(record.target_id == 7);
  REQUIRE_FALSE(tap.pop(record));
}

#if defined(SONARE_WITH_MIXING)
namespace {

struct EngineScopeResult {
  sonare::engine::ScopeTelemetryRecord scope{};
  std::array<float, 128> output{};
};

EngineScopeResult render_with_metronome(bool enabled) {
  constexpr int kBlock = 128;
  constexpr int kSplit = 32;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlock);
  REQUIRE(engine.configure_scope_telemetry(kBlock, kBands) == kBands);
  engine.set_metronome_config({enabled, 0.25f, 0.75f, kSplit, 0.0});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  sonare::rt::Command stop{};
  stop.type = sonare::rt::CommandType::kTransportStop;
  stop.sample_time = kSplit;
  REQUIRE(engine.push_command(stop));

  std::array<float, kBlock> output{};
  float* channels[] = {output.data(), output.data()};
  engine.process(channels, 2, kBlock);

  EngineScopeResult result;
  result.output = output;
  REQUIRE(engine.pop_scope_telemetry(result.scope));
  REQUIRE(result.scope.target_id == 0);
  return result;
}

}  // namespace

TEST_CASE("RealtimeEngine master scope excludes metronome across sub-blocks",
          "[engine][scope_telemetry][realtime]") {
  const auto without_metronome = render_with_metronome(false);
  const auto with_metronome = render_with_metronome(true);

  REQUIRE(with_metronome.scope.render_frame == without_metronome.scope.render_frame);
  REQUIRE(with_metronome.scope.band_count == without_metronome.scope.band_count);
  for (uint32_t i = 0; i < without_metronome.scope.band_count; ++i) {
    REQUIRE(with_metronome.scope.bands[i] == without_metronome.scope.bands[i]);
  }
  REQUIRE(with_metronome.scope.point_count == without_metronome.scope.point_count);
  for (uint32_t i = 0; i < without_metronome.scope.point_count; ++i) {
    REQUIRE(with_metronome.scope.points[i].left == without_metronome.scope.points[i].left);
    REQUIRE(with_metronome.scope.points[i].right == without_metronome.scope.points[i].right);
  }

  REQUIRE(*std::max_element(without_metronome.output.begin(), without_metronome.output.end()) ==
          0.0f);
  REQUIRE(*std::max_element(with_metronome.output.begin(), with_metronome.output.end()) > 0.0f);
}
#endif  // defined(SONARE_WITH_MIXING)
