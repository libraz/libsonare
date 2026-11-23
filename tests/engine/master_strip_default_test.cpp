/// @file master_strip_default_test.cpp
/// @brief The engine owns a master strip from construction: a master fader, pan or
///        width edit takes effect without a prior set_master_strip, live and offline,
///        and the untouched default strip leaves the output bit-identical.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/strip_spec.h"
#include "engine/track_mixer.h"
#include "mixing/api/scene.h"
#include "rt/command.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPi;
using sonare::engine::RealtimeEngine;

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
constexpr int kBlocks = 64;
constexpr int64_t kFrames = static_cast<int64_t>(kBlock) * kBlocks;
constexpr uint32_t kTrack = 1;

enum class Path { kLive, kOffline };
enum class Edit { kNone, kFaderMinus12, kPanHardRight, kWidthZero, kBypass };

struct Row {
  bool explicit_strip;
  Edit edit;
  // A track lane renders a stereo pair; a clip on no lane reaches the master at full width.
  bool on_lane = true;
};

// One tone per channel at its own frequency, so a fold or reorder shows.
std::vector<std::vector<float>> tones(int channels, float level) {
  std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                      std::vector<float>(static_cast<size_t>(kFrames) + kBlock));
  for (int ch = 0; ch < channels; ++ch) {
    const float hz = 220.0f * static_cast<float>(ch + 1);
    for (size_t i = 0; i < out[static_cast<size_t>(ch)].size(); ++i) {
      out[static_cast<size_t>(ch)][i] =
          level * std::sin(kTwoPi * hz * static_cast<float>(i) / static_cast<float>(kRate));
    }
  }
  return out;
}

void push_set(RealtimeEngine& engine, const std::string& key, float value) {
  const int64_t id = engine.resolve_master_automation_id(key);
  REQUIRE(id >= 0);
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kSetParam;
  command.target_id = static_cast<uint32_t>(id);
  command.sample_time = -1;
  command.arg.f = value;
  REQUIRE(engine.push_command(command));
}

std::vector<std::vector<float>> render(Path path, int channels, Row row) {
  const auto sources = tones(channels, 0.5f);
  std::vector<const float*> planes;
  for (const auto& plane : sources) planes.push_back(plane.data());
  RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  if (row.on_lane) REQUIRE(engine.set_track_lanes({sonare::engine::TrackLaneConfig{kTrack}}));
  sonare::engine::ClipSchedule clip{
      1,     {planes.data(), channels, static_cast<int64_t>(sources[0].size())},
      0.0,   0,
      0,     kFrames,
      false, 1.0f,
      0,     0};
  if (row.on_lane) clip.track_id = kTrack;
  engine.set_clips({clip});
  if (row.explicit_strip) REQUIRE(engine.set_master_strip(sonare::mixing::api::Strip{}));
  switch (row.edit) {
    case Edit::kNone:
      break;
    case Edit::kFaderMinus12:
      push_set(engine, "faderDb", -12.0f);
      break;
    case Edit::kPanHardRight:
      push_set(engine, "pan", 1.0f);
      break;
    case Edit::kWidthZero:
      push_set(engine, "width", 0.0f);
      break;
    case Edit::kBypass:
      engine.set_mixing_enabled(false);
      break;
  }
  std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                      std::vector<float>(static_cast<size_t>(kFrames), 0.0f));
  std::vector<float*> io;
  for (auto& plane : out) io.push_back(plane.data());
  if (path == Path::kOffline) {
    engine.render_offline(io.data(), channels, kFrames, kBlock);
    return out;
  }
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  for (int b = 0; b < kBlocks; ++b) {
    std::vector<float*> block;
    for (auto* plane : io) block.push_back(plane + static_cast<ptrdiff_t>(b) * kBlock);
    engine.process(block.data(), channels, kBlock);
  }
  return out;
}

// Peak over the last quarter, after every smoother has settled.
float settled_peak(const std::vector<float>& plane) {
  float peak = 0.0f;
  for (size_t i = plane.size() * 3 / 4; i < plane.size(); ++i) {
    peak = std::max(peak, std::abs(plane[i]));
  }
  return peak;
}

}  // namespace

TEST_CASE("The default master strip leaves the engine output bit-identical",
          "[engine][mixing][master_strip]") {
  for (const Path path : {Path::kLive, Path::kOffline}) {
    // Mono, stereo, 5.1 and 7.1.4: the wide layouts take the strip's unsegmented path.
    for (const int channels : {1, 2, 6, 12}) {
      CAPTURE(path == Path::kLive ? "live" : "offline", channels);
      const auto bypassed = render(path, channels, {false, Edit::kBypass, false});
      const auto by_default = render(path, channels, {false, Edit::kNone, false});
      for (const auto& plane : bypassed) REQUIRE(settled_peak(plane) > 0.4f);
      CHECK(by_default == bypassed);
    }
  }
}

TEST_CASE("A master fader or pan edit takes effect without a prior set_master_strip",
          "[engine][mixing][master_strip]") {
  const float kMinus12 = std::pow(10.0f, -12.0f / 20.0f);
  for (const Path path : {Path::kLive, Path::kOffline}) {
    CAPTURE(path == Path::kLive ? "live" : "offline");
    const auto untouched = render(path, 2, {false, Edit::kNone});
    const auto untouched_explicit = render(path, 2, {true, Edit::kNone});
    const auto fader = render(path, 2, {false, Edit::kFaderMinus12});
    const auto fader_explicit = render(path, 2, {true, Edit::kFaderMinus12});
    const auto pan = render(path, 2, {false, Edit::kPanHardRight});
    const auto pan_explicit = render(path, 2, {true, Edit::kPanHardRight});
    const auto width = render(path, 2, {false, Edit::kWidthZero});
    const auto width_explicit = render(path, 2, {true, Edit::kWidthZero});

    // An engine that never had set_master_strip renders as one that did.
    CHECK(untouched == untouched_explicit);
    CHECK(fader == fader_explicit);
    CHECK(pan == pan_explicit);
    CHECK(width == width_explicit);

    const float reference = settled_peak(untouched[0]);
    REQUIRE(reference > 0.4f);
    for (const auto& plane : fader) {
      CHECK(std::abs(settled_peak(plane) - reference * kMinus12) <= 1.0e-4f * reference);
    }
    CHECK(settled_peak(pan[0]) <= 1.0e-6f * reference);
    CHECK(settled_peak(pan[1]) >= reference);
    // Width 0 folds the pair to its mid: the two channels carry one signal.
    std::vector<float> side(width[0].size());
    for (size_t i = 0; i < side.size(); ++i) side[i] = width[0][i] - width[1][i];
    CHECK(settled_peak(side) <= 1.0e-6f * reference);
  }
}

TEST_CASE("Unbinding an external master strip returns to the engine's own strip",
          "[engine][mixing][master_strip]") {
  RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  sonare::mixing::ChannelStrip external;
  REQUIRE(engine.bind_mixing_strip(&external));
  REQUIRE(engine.bind_mixing_strip(nullptr));
  // The master fader reaches a bound strip again.
  push_set(engine, "faderDb", -6.0f);
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  const int64_t id = engine.resolve_master_automation_id("faderDb");
  float value = 0.0f;
  REQUIRE(engine.current_parameter_value(static_cast<uint32_t>(id), &value));
  CHECK(value == -6.0f);
}

TEST_CASE("A later set_master_strip replaces the master fader wholesale",
          "[engine][mixing][master_strip]") {
  RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  const int64_t id = engine.resolve_master_automation_id("faderDb");
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  float* io[] = {left.data(), right.data()};
  float value = 0.0f;

  push_set(engine, "faderDb", -12.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(engine.current_parameter_value(static_cast<uint32_t>(id), &value));
  REQUIRE(value == -12.0f);

  // set_master_strip states the whole strip, as set_track_strip does: its fader wins.
  sonare::mixing::api::Strip spec;
  spec.fader_db = -3.0f;
  REQUIRE(engine.set_master_strip(spec));
  REQUIRE(engine.current_parameter_value(static_cast<uint32_t>(id), &value));
  CHECK(value == -3.0f);
  REQUIRE(engine.set_master_strip(sonare::mixing::api::Strip{}));
  REQUIRE(engine.current_parameter_value(static_cast<uint32_t>(id), &value));
  CHECK(value == 0.0f);
}

TEST_CASE("A master strip at rest skips only blocks processing would leave unchanged",
          "[engine][mixing][master_strip]") {
  for (const int channels : {1, 2, 6, 12}) {
    CAPTURE(channels);
    auto processed =
        sonare::engine::make_channel_strip_from_spec(sonare::mixing::api::Strip{}, kRate);
    auto skipped =
        sonare::engine::make_channel_strip_from_spec(sonare::mixing::api::Strip{}, kRate);
    processed->prepare(kRate, kBlock);
    skipped->prepare(kRate, kBlock);
    // A fresh strip's first block admits its EQ stage, so that block is processed.
    CHECK_FALSE(skipped->passes_through(channels));
    auto a = tones(channels, 0.5f);
    auto b = a;
    std::vector<float*> pa;
    std::vector<float*> pb;
    for (auto& plane : a) pa.push_back(plane.data());
    for (auto& plane : b) pb.push_back(plane.data());
    for (int block = 0; block < kBlocks; ++block) {
      const int64_t start = static_cast<int64_t>(block) * kBlock;
      std::vector<float*> ba;
      std::vector<float*> bb;
      for (auto* plane : pa) ba.push_back(plane + start);
      for (auto* plane : pb) bb.push_back(plane + start);
      processed->process_at(ba.data(), channels, kBlock, start);
      skipped->process_at_skipping_rest(bb.data(), channels, kBlock, start);
    }
    CHECK(a == b);
    CHECK(a == tones(channels, 0.5f));
    CHECK(skipped->passes_through(channels));
  }

  auto strip = sonare::engine::make_channel_strip_from_spec(sonare::mixing::api::Strip{}, kRate);
  strip->prepare(kRate, kBlock);
  auto wide = tones(6, 0.5f);
  std::vector<float*> planes;
  for (auto& plane : wide) planes.push_back(plane.data());
  strip->process_at(planes.data(), 6, kBlock, 0);
  REQUIRE(strip->passes_through(2));
  strip->set_fader_db(-12.0f);
  CHECK_FALSE(strip->passes_through(2));
  REQUIRE(strip->set_pan(0.5f));
  strip->set_fader_db(0.0f);
  CHECK_FALSE(strip->passes_through(2));
  CHECK(strip->passes_through(6));
}

TEST_CASE("The first set_master_strip starts on its values; a later one glides to them",
          "[engine][mixing][master_strip]") {
  // A constant input, so the last sample of a block reads the fader there.
  const std::vector<float> ones(static_cast<size_t>(kBlock) * 64, 1.0f);
  const float* source[] = {ones.data(), ones.data()};
  RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  const auto length = static_cast<int64_t>(ones.size());
  engine.set_clips(
      {sonare::engine::ClipSchedule{1, {source, 2, length}, 0.0, 0, 0, length, false, 1.0f, 0, 0}});
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  float* io[] = {left.data(), right.data()};
  const float minus12 = std::pow(10.0f, -12.0f / 20.0f);
  // The engine passes its input through, so each block starts from silence.
  const auto block = [&] {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.process(io, 2, kBlock);
  };

  // Built fresh, as a track's first set_track_strip is: the first block is already at -12 dB.
  sonare::mixing::api::Strip spec;
  spec.fader_db = -12.0f;
  REQUIRE(engine.set_master_strip(spec));
  block();
  CHECK(std::abs(left.back() - minus12) < 1e-4f);

  // Edited in place: the fader ramps, so the next block's first sample still sits near -12 dB.
  spec.fader_db = 0.0f;
  REQUIRE(engine.set_master_strip(spec));
  block();
  CHECK(left.front() < 0.5f);
  CHECK(left.back() < 1.0f);
  for (int b = 0; b < 32; ++b) block();
  CHECK(std::abs(left.back() - 1.0f) < 1e-4f);
}
