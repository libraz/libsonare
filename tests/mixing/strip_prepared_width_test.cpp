/// @file strip_prepared_width_test.cpp
/// @brief A channel strip is sized for the width it is prepared for, and every in-tree host
///        declares the width it renders.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "support/alloc_guard.h"
#include "util/exception.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
// +12 dB low shelf at 1 kHz: a DC source leaves it about four times louder once settled.
constexpr const char* kShelfParams =
    R"({"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1})";

std::unique_ptr<sonare::rt::ProcessorBase> shelf_insert() {
  return sonare::mastering::api::make_insert("eq.parametric", kShelfParams, nullptr);
}

struct Planes {
  explicit Planes(int count, float value = 0.5f)
      : storage(static_cast<size_t>(count), std::vector<float>(kBlock, value)) {
    for (auto& plane : storage) pointers.push_back(plane.data());
  }
  std::vector<std::vector<float>> storage;
  std::vector<float*> pointers;
};

/// Least bytes a strip holds once built with one EQ insert and prepared at @p channels
/// (0: left undeclared), over three builds.
long long strip_held_bytes(int channels) {
  long long least = -1;
  for (int round = 0; round < 3; ++round) {
    sonare::test::PeakLiveBytesGuard guard;
    auto strip = std::make_unique<sonare::mixing::ChannelStrip>();
    strip->add_pre_insert(shelf_insert());
    if (channels > 0) strip->set_prepared_channels(channels);
    strip->prepare(kSampleRate, 512);
    const long long held = sonare::test::g_live_bytes.load();
    least = least < 0 ? held : std::min(least, held);
  }
  return least;
}

/// Renders one DC block from track 10 through @p mixer twice and returns the last sample of
/// @p plane from the second, guarded render.
float render_lane(sonare::engine::TrackMixerRuntime& mixer, int master_channels, int plane,
                  size_t* allocations) {
  std::vector<float> left(kBlock, 0.25f);
  std::vector<float> right(kBlock, 0.25f);
  float* source[] = {left.data(), right.data()};
  Planes out(master_channels, 0.0f);
  REQUIRE(mixer.mix_source(10, source, out.pointers.data(), master_channels, kBlock));
  for (auto& p : out.storage) std::fill(p.begin(), p.end(), 0.0f);
  {
    sonare::test::AllocationGuard guard;
    const bool ok = mixer.mix_source(10, source, out.pointers.data(), master_channels, kBlock);
    *allocations = guard.count();
    REQUIRE(ok);
  }
  return out.storage[static_cast<size_t>(plane)].back();
}

}  // namespace

TEST_CASE("an undeclared strip is prepared for the realtime ceiling", "[mixing][prepared_width]") {
  sonare::mixing::ChannelStrip strip;
  strip.set_fader_db(-6.0f);
  strip.prepare(kSampleRate, kBlock);
  REQUIRE(strip.prepared_channels() == 64);
  Planes wide(64);
  strip.process(wide.pointers.data(), 64, kBlock);
  strip.process(wide.pointers.data(), 64, kBlock);
  for (const auto& plane : wide.storage) REQUIRE(std::abs(plane.back() - 0.125f) < 0.01f);
}

TEST_CASE("a strip refuses a block wider than its prepared width", "[mixing][prepared_width]") {
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(shelf_insert());
  strip.set_prepared_channels(2);
  strip.prepare(kSampleRate, kBlock);
  Planes planes(3);
  strip.process(planes.pointers.data(), 2, kBlock);
  try {
    strip.process(planes.pointers.data(), 3, kBlock);
    FAIL("a three-plane block reached a strip prepared for two");
  } catch (const sonare::SonareException& error) {
    REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    REQUIRE_THAT(std::string(error.what()), Catch::Matchers::ContainsSubstring("num_channels"));
  }
  // The refused block was left untouched.
  REQUIRE(planes.storage[2].back() == 0.5f);
}

TEST_CASE("declaring a wider width on a prepared strip prepares it again",
          "[mixing][prepared_width]") {
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(shelf_insert());
  strip.set_prepared_channels(2);
  strip.prepare(kSampleRate, kBlock);
  strip.set_prepared_channels(8);
  REQUIRE(strip.prepared_channels() == 8);
  Planes planes(8);
  for (int round = 0; round < 8; ++round) {
    for (auto& p : planes.storage) std::fill(p.begin(), p.end(), 0.5f);
    strip.process(planes.pointers.data(), 8, kBlock);
  }
  // Every plane went through the shelf, not only the two the first prepare sized.
  for (const auto& plane : planes.storage) REQUIRE(plane.back() > 1.0f);
}

TEST_CASE("a strip declared stereo holds less than an undeclared one by its planes",
          "[mixing][prepared_width]") {
  strip_held_bytes(2);
  const long long stereo = strip_held_bytes(2);
  const long long undeclared = strip_held_bytes(0);
  INFO("stereo " << stereo << " bytes, undeclared " << undeclared);
  // Taps alone hold at least one block per plane, so 62 more planes cost no less than that.
  REQUIRE(undeclared - stereo > 62LL * 512 * static_cast<long long>(sizeof(float)));
}

TEST_CASE("lane strips are declared stereo and render without allocating",
          "[engine][track_mixer][prepared_width][rt]") {
  sonare::engine::TrackMixerRuntime mixer;
  mixer.prepare(kSampleRate, kBlock);
  REQUIRE(mixer.set_track_lanes({{10}}));
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(shelf_insert());
  REQUIRE(mixer.bind_track_strip(10, &strip));
  REQUIRE(strip.prepared_channels() == 2);
  size_t allocations = 99;
  const float left = render_lane(mixer, 2, 0, &allocations);
  REQUIRE(allocations == 0);
  REQUIRE(left > 0.5f);
}

TEST_CASE("a retained bus widened to 7.1 runs its insert on every plane without allocating",
          "[engine][track_mixer][prepared_width][rt][surround]") {
  constexpr int kCentrePlane = 2;
  const auto centre = [](bool shelf, size_t* allocations) {
    sonare::engine::TrackMixerRuntime mixer;
    mixer.prepare(kSampleRate, kBlock);
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::Stereo}}));
    sonare::engine::TrackLaneConfig lane{10};
    lane.output_bus_id = 1;
    REQUIRE(mixer.set_track_lanes({lane}));
    sonare::mixing::api::Bus bus;
    bus.id = "1";
    if (shelf) {
      bus.inserts.push_back(
          {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric", kShelfParams});
    }
    REQUIRE(mixer.set_bus_strip(1, bus));
    REQUIRE(mixer.set_buses({{1, 0.0f, sonare::ChannelLayout::SevenPointOne}}));
    return render_lane(mixer, 8, kCentrePlane, allocations);
  };
  size_t allocations = 99;
  const float shaped = centre(true, &allocations);
  REQUIRE(allocations == 0);
  const float plain = centre(false, &allocations);
  INFO("centre plane " << shaped << " through the shelf, " << plain << " without");
  REQUIRE(plain > 0.0f);
  REQUIRE(shaped > 2.0f * plain);
}

TEST_CASE("the engine master strip takes the engine's prepared width",
          "[engine][prepared_width][rt]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlock, sonare::engine::RealtimeEngine::kDefaultCommandCapacity,
                 sonare::engine::RealtimeEngine::kDefaultTelemetryCapacity, 4);
  REQUIRE(engine.mixing().strip() != nullptr);
  REQUIRE(engine.mixing().strip()->prepared_channels() == 4);

  sonare::mixing::api::Strip spec;
  spec.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric", kShelfParams});
  REQUIRE(engine.set_master_strip(spec));
  REQUIRE(engine.mixing().strip()->prepared_channels() == 4);

  sonare::mixing::ChannelStrip external;
  REQUIRE(engine.bind_mixing_strip(&external));
  REQUIRE(external.prepared_channels() == 4);
  REQUIRE(engine.bind_mixing_strip(nullptr));

  Planes planes(4);
  engine.mixing().process_at(planes.pointers.data(), 4, kBlock, 0);
  sonare::test::AllocationGuard guard;
  engine.mixing().process_at(planes.pointers.data(), 4, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}
