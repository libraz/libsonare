/// @file automation_staging_test.cpp
/// @brief Dense automation reaches the DSP at every accepted offset.

#include <catch2/catch_approx.hpp>

#include "mastering/api/insert_factory.h"
#include "routing_test_helpers.h"
#include "util/json.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

namespace {

constexpr int kSr = 48000;
constexpr int kMaxBlock = 1024;
constexpr int kLength = 1024;
constexpr int kHold = 2;
constexpr int kLinear = 0;

struct Point {
  int64_t sample;
  float db;
};

// Hold points every 4 samples alternating 0 / -12 dB, plus a same-time pair
// whose last value must win.
std::vector<Point> dense_points(int count) {
  std::vector<Point> points;
  for (int i = 0; i < count; ++i) points.push_back({4 * i, i % 2 == 0 ? 0.0f : -12.0f});
  points.push_back({1000, -6.0f});
  points.push_back({1000, -3.0f});
  return points;
}

float gain_at(const std::vector<Point>& points, int64_t sample) {
  float db = 0.0f;
  for (const Point& point : points) {
    if (point.sample <= sample) db = point.db;
  }
  return std::pow(10.0f, db / 20.0f);
}

enum class Target { Insert, Fader };

// Renders constant L 0.05 / R 0.08 through one strip in blocks of @p block,
// automating @p target with @p points (none when empty).
std::vector<float> render(Target target, const std::vector<Point>& points, int block) {
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "source";
  strip.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "utility.gain", R"({"levelDb":0})"});
  scene.strips.push_back(strip);
  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kMaxBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* handle = sonare_mixer_strip_by_id(mixer, "source");
  REQUIRE(handle != nullptr);
  for (const Point& point : points) {
    const SonareError result =
        target == Target::Insert
            ? sonare_strip_schedule_insert_automation(handle, 0, 0, point.sample, point.db, kHold)
            : sonare_strip_schedule_fader_automation(handle, point.sample, point.db, kHold);
    REQUIRE(result == SONARE_OK);
  }
  std::vector<float> in_l(static_cast<size_t>(block), 0.05f);
  std::vector<float> in_r(static_cast<size_t>(block), 0.08f);
  const float* inputs_l[] = {in_l.data()};
  const float* inputs_r[] = {in_r.data()};
  std::vector<float> out(kLength * 2);
  std::vector<float> out_l(static_cast<size_t>(block));
  std::vector<float> out_r(static_cast<size_t>(block));
  for (int start = 0; start < kLength; start += block) {
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        static_cast<size_t>(block)) == SONARE_OK);
    for (int i = 0; i < block; ++i) {
      out[static_cast<size_t>(2 * (start + i))] = out_l[static_cast<size_t>(i)];
      out[static_cast<size_t>(2 * (start + i) + 1)] = out_r[static_cast<size_t>(i)];
    }
  }
  sonare_mixer_destroy(mixer);
  return out;
}

}  // namespace

TEST_CASE("Dense hold automation lands at every accepted offset for any block size",
          "[mixing][capi][automation]") {
  const std::vector<float> reference = render(Target::Insert, {}, kMaxBlock);
  for (const int count : {127, 128, 129, 200}) {
    const std::vector<Point> points = dense_points(count);
    for (const int block : {kMaxBlock, 64}) {
      INFO("points " << count << " block " << block);
      const std::vector<float> out = render(Target::Insert, points, block);
      for (int sample = 0; sample < kLength; ++sample) {
        const float gain = gain_at(points, sample);
        for (int ch = 0; ch < 2; ++ch) {
          const size_t at = static_cast<size_t>(2 * sample + ch);
          INFO("sample " << sample << " channel " << ch);
          REQUIRE_THAT(out[at], WithinAbs(reference[at] * gain, 1.0e-6f));
        }
      }
    }
  }
}

TEST_CASE("Dense fader automation renders the same for any block size",
          "[mixing][capi][automation]") {
  const std::vector<Point> points = dense_points(200);
  const std::vector<float> whole = render(Target::Fader, points, kMaxBlock);
  const std::vector<float> partitioned = render(Target::Fader, points, 64);
  for (size_t i = 0; i < whole.size(); ++i) {
    INFO("index " << i);
    REQUIRE_THAT(whole[i], WithinAbs(partitioned[i], 1.0e-6f));
  }
}

TEST_CASE("Send automation keeps interpolating across removal of an earlier send",
          "[mixing][capi][automation]") {
  constexpr int kBlock = 128;
  constexpr int kFrames = 4096;
  enum class Order { SingleSend, BothBeforeRemoval, StraddlingRemoval };
  const auto render_send = [&](Order order) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});
    scene.buses.push_back({"dummy", "aux"});
    scene.buses.push_back({"aux", "aux"});
    sonare::mixing::api::Strip source;
    source.id = "source";
    scene.strips.push_back(source);
    scene.connections.push_back({"source", "discard"});
    scene.connections.push_back({"aux", "master"});
    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "source");
    REQUIRE(strip != nullptr);
    size_t index = 0;
    if (order != Order::SingleSend) {
      REQUIRE(sonare_strip_add_send(strip, "to-dummy", "dummy", -120.0f,
                                    SONARE_SEND_TIMING_POST_FADER, &index) == SONARE_OK);
    }
    REQUIRE(sonare_strip_add_send(strip, "to-aux", "aux", 0.0f, SONARE_SEND_TIMING_POST_FADER,
                                  &index) == SONARE_OK);
    REQUIRE(sonare_strip_schedule_send_automation(strip, index, 0, 0.0f, kLinear) == SONARE_OK);
    if (order == Order::BothBeforeRemoval) {
      REQUIRE(sonare_strip_schedule_send_automation(strip, index, 2000, -20.0f, kHold) ==
              SONARE_OK);
    }
    if (order != Order::SingleSend) REQUIRE(sonare_strip_remove_send(strip, 0) == SONARE_OK);
    if (order != Order::BothBeforeRemoval) {
      REQUIRE(sonare_strip_schedule_send_automation(strip, 0, 2000, -20.0f, kHold) == SONARE_OK);
    }
    std::vector<float> in(kBlock, 0.1f);
    const float* inputs[] = {in.data()};
    std::vector<float> out;
    std::vector<float> out_l(kBlock);
    std::vector<float> out_r(kBlock);
    for (int start = 0; start < kFrames; start += kBlock) {
      REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, out_l.data(), out_r.data(),
                                          kBlock) == SONARE_OK);
      out.insert(out.end(), out_l.begin(), out_l.end());
    }
    sonare_mixer_destroy(mixer);
    return out;
  };

  const std::vector<float> control = render_send(Order::SingleSend);
  // Non-vacuity: the ramp is under way well before the second point.
  REQUIRE(control[1000] < control[10] * 0.6f);
  for (const Order order : {Order::BothBeforeRemoval, Order::StraddlingRemoval}) {
    const std::vector<float> out = render_send(order);
    for (size_t i = 0; i < out.size(); ++i) {
      INFO("order " << static_cast<int>(order) << " frame " << i);
      REQUIRE(std::isfinite(out[i]));
      REQUIRE_THAT(out[i], WithinAbs(control[i], 1.0e-6f));
    }
  }
}

TEST_CASE("Scene export carries consumed send, fader and insert automation",
          "[mixing][capi][automation][scene]") {
  constexpr int kBlock = 128;
  const char* compressor = R"({"thresholdDb":-18,"ratio":4,"attackMs":1,"releaseMs":20})";
  const auto probe = sonare::mastering::api::make_insert("dynamics.compressor", compressor);
  REQUIRE(probe != nullptr);
  int threshold_id = -1;
  for (const auto& descriptor : probe->parameter_descriptors()) {
    if (descriptor.key == "thresholdDb") threshold_id = static_cast<int>(descriptor.id);
  }
  REQUIRE(threshold_id >= 0);

  sonare::mixing::api::Scene scene;
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"aux", "aux"});
  sonare::mixing::api::Strip source;
  source.id = "source";
  source.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "dynamics.compressor", compressor});
  source.sends.push_back({"to-aux", "aux", 0.0f, sonare::mixing::api::SendTiming::PostFader});
  scene.strips.push_back(source);
  scene.connections.push_back({"aux", "master"});
  const std::string json = sonare::mixing::api::scene_to_json(scene);

  std::vector<float> in(kBlock, 0.5f);
  const float* inputs[] = {in.data()};
  std::vector<float> out_l(kBlock);
  std::vector<float> out_r(kBlock);
  const auto run = [&](SonareMixer* mixer, int blocks) {
    for (int block = 0; block < blocks; ++block) {
      REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, out_l.data(), out_r.data(),
                                          kBlock) == SONARE_OK);
    }
    return out_l.back();
  };
  const auto export_scene = [](SonareMixer* mixer) {
    char* text = nullptr;
    REQUIRE(sonare_mixer_to_scene_json(mixer, &text) == SONARE_OK);
    const std::string exported(text);
    sonare_free_string(text);
    return sonare::mixing::api::scene_from_json(exported);
  };

  for (const bool via_setter : {false, true}) {
    INFO("direct setters " << via_setter);
    SonareMixer* live = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(live != nullptr);
    SonareStrip* strip = sonare_mixer_strip_by_id(live, "source");
    REQUIRE(strip != nullptr);
    if (via_setter) {
      REQUIRE(sonare_strip_set_send_db(strip, 0, -20.0f) == SONARE_OK);
      REQUIRE(sonare_strip_set_fader_db(strip, -6.0f) == SONARE_OK);
    } else {
      REQUIRE(sonare_strip_schedule_send_automation(strip, 0, 0, -20.0f, kHold) == SONARE_OK);
      REQUIRE(sonare_strip_schedule_fader_automation(strip, 0, -6.0f, kHold) == SONARE_OK);
    }
    REQUIRE(sonare_strip_schedule_insert_automation(
                strip, 0, static_cast<unsigned int>(threshold_id), 0, 0.0f, kHold) == SONARE_OK);
    const float live_level = run(live, 100);

    const sonare::mixing::api::Scene exported = export_scene(live);
    REQUIRE(exported.strips.size() == 1);
    CHECK(exported.strips[0].fader_db == Catch::Approx(-6.0f));
    REQUIRE(exported.strips[0].sends.size() == 1);
    CHECK(exported.strips[0].sends[0].send_db == Catch::Approx(-20.0f));
    REQUIRE(exported.strips[0].inserts.size() == 1);
    const auto params = sonare::util::json::parse(exported.strips[0].inserts[0].params_json);
    CHECK(params["thresholdDb"].as_float() == Catch::Approx(0.0f));
    CHECK(params["ratio"].as_float() == Catch::Approx(4.0f));

    const std::string reloaded_json = sonare::mixing::api::scene_to_json(exported);
    SonareMixer* reloaded = sonare_mixer_from_scene_json(reloaded_json.c_str(), kSr, kBlock);
    REQUIRE(reloaded != nullptr);
    const float reloaded_level = run(reloaded, 100);
    CHECK(reloaded_level == Catch::Approx(live_level).epsilon(1.0e-3));
    sonare_mixer_destroy(reloaded);
    sonare_mixer_destroy(live);
  }
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
