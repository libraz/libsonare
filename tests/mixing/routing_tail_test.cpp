/// @file routing_tail_test.cpp
/// @brief Mixer tail query against the audible path set it renders.

#include "mastering/api/insert_factory.h"
#include "routing_test_helpers.h"

// Every case routes through effects.delay.stereo.
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH) && defined(SONARE_WITH_FX)

namespace {

constexpr int kSr = 48000;
constexpr int kBlock = 128;
constexpr const char* kDelay = "effects.delay.stereo";
constexpr const char* kDelay10 = R"({"delayTimeLMs":10,"delayTimeRMs":10,"feedback":0,"dryWet":1})";
using sonare::mixing::api::InsertSlot;

int standalone_delay_tail(const char* params) {
  auto delay = sonare::mastering::api::make_insert(kDelay, params);
  REQUIRE(delay != nullptr);
  delay->prepare(kSr, kBlock);
  return delay->tail_samples();
}

SonareMixer* mixer_from(const sonare::mixing::api::Scene& scene) {
  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  return mixer;
}

int query_tail(SonareMixer* mixer) {
  int tail = -1;
  REQUIRE(sonare_mixer_tail_samples(mixer, &tail) == SONARE_OK);
  return tail;
}

// Plays a 0.5 impulse at frame 0 into strip 0 (the others silent) for
// @p blocks blocks; returns the last frame whose |sample| exceeds 1e-6, or -1.
int last_audible_frame(SonareMixer* mixer, size_t strips, int blocks) {
  std::vector<std::vector<float>> inputs(strips, std::vector<float>(kBlock, 0.0f));
  inputs[0][0] = 0.5f;
  std::vector<const float*> pointers;
  for (const auto& input : inputs) pointers.push_back(input.data());
  std::vector<float> out_l(kBlock);
  std::vector<float> out_r(kBlock);
  int last = -1;
  for (int block = 0; block < blocks; ++block) {
    REQUIRE(sonare_mixer_process_stereo(mixer, pointers.data(), pointers.data(), strips,
                                        out_l.data(), out_r.data(), kBlock) == SONARE_OK);
    inputs[0][0] = 0.0f;
    for (int i = 0; i < kBlock; ++i) {
      if (std::abs(out_l[static_cast<size_t>(i)]) > 1.0e-6f) last = block * kBlock + i;
    }
  }
  return last;
}

}  // namespace

TEST_CASE("Mixer tail follows the tap a send reads", "[mixing][capi][tail]") {
  const int delay_tail = standalone_delay_tail(kDelay10);
  REQUIRE(delay_tail > 480);
  // The strip's main goes to an unpatched bus; only its send reaches master.
  const auto scene_for = [](bool with_delay, InsertSlot delay_slot, bool pre_send) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});
    scene.buses.push_back({"aux", "aux"});
    sonare::mixing::api::Strip source;
    source.id = "source";
    if (with_delay) source.inserts.push_back({delay_slot, kDelay, kDelay10});
    source.sends.push_back({"to-aux", "aux", 0.0f,
                            pre_send ? sonare::mixing::api::SendTiming::PreFader
                                     : sonare::mixing::api::SendTiming::PostFader});
    scene.strips.push_back(source);
    scene.connections.push_back({"source", "discard"});
    scene.connections.push_back({"aux", "master"});
    return scene;
  };
  struct Case {
    bool with_delay;
    InsertSlot slot;
    bool pre_send;
    int expected_tail;
    int expected_last;
  };
  const Case cases[] = {
      {true, InsertSlot::PostFader, true, 0, 0},
      {true, InsertSlot::PostFader, false, delay_tail, 480},
      {true, InsertSlot::PreFader, true, delay_tail, 480},
      {false, InsertSlot::PostFader, true, 0, 0},
  };
  for (const Case& c : cases) {
    INFO("delay " << c.with_delay << " pre-insert " << (c.slot == InsertSlot::PreFader)
                  << " pre-send " << c.pre_send);
    SonareMixer* mixer = mixer_from(scene_for(c.with_delay, c.slot, c.pre_send));
    const int tail = query_tail(mixer);
    CHECK(tail == c.expected_tail);
    // The 10 ms delay's interpolator spreads the impulse over frames 480-481.
    const int last = last_audible_frame(mixer, 1, 8);
    CHECK(last >= c.expected_last);
    CHECK(last <= tail);
    sonare_mixer_destroy(mixer);
  }
}

TEST_CASE("Mixer tail includes a key the target monitors", "[mixing][capi][tail]") {
  const int delay_tail = standalone_delay_tail(kDelay10);
  enum class Mode { KeyListen, DetectorOnly, DirectToMaster };
  const auto scene_for = [](Mode mode) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});
    sonare::mixing::api::Strip key;
    key.id = "key";
    key.inserts.push_back({InsertSlot::PreFader, kDelay, kDelay10});
    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    bed.inserts.push_back(
        {InsertSlot::PreFader, "dynamics.sidechainRouter",
         mode == Mode::KeyListen ? R"({"keyListen":true})" : R"({"keyListen":false})", "key"});
    scene.strips = {key, bed};
    scene.connections.push_back({"key", mode == Mode::DirectToMaster ? "master" : "discard"});
    scene.connections.push_back({"bed", "master"});
    return scene;
  };

  SonareMixer* listen = mixer_from(scene_for(Mode::KeyListen));
  const int listen_tail = query_tail(listen);
  const int listen_last = last_audible_frame(listen, 2, 8);
  sonare_mixer_destroy(listen);
  CHECK(listen_last >= 480);
  CHECK(listen_tail == delay_tail);
  CHECK(listen_tail >= listen_last);

  SonareMixer* detector = mixer_from(scene_for(Mode::DetectorOnly));
  CHECK(query_tail(detector) == 0);
  CHECK(last_audible_frame(detector, 2, 8) == -1);
  sonare_mixer_destroy(detector);

  SonareMixer* direct = mixer_from(scene_for(Mode::DirectToMaster));
  CHECK(query_tail(direct) == delay_tail);
  sonare_mixer_destroy(direct);
}

TEST_CASE("Mixer tail reflects consumed insert automation", "[mixing][capi][tail]") {
  const int short_tail = standalone_delay_tail(kDelay10);
  const int long_tail =
      standalone_delay_tail(R"({"delayTimeLMs":100,"delayTimeRMs":100,"feedback":0,"dryWet":1})");
  const int shorter_tail =
      standalone_delay_tail(R"({"delayTimeLMs":5,"delayTimeRMs":5,"feedback":0,"dryWet":1})");
  REQUIRE(long_tail >= 4800);
  REQUIRE(shorter_tail < short_tail);

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip source;
  source.id = "source";
  source.inserts.push_back({InsertSlot::PreFader, kDelay, kDelay10});
  scene.strips.push_back(source);

  // target_ms 0 leaves the insert untouched.
  for (const float target_ms : {100.0f, 5.0f, 0.0f}) {
    INFO("target " << target_ms);
    const int expected = target_ms == 100.0f ? long_tail
                         : target_ms == 5.0f ? shorter_tail
                                             : short_tail;
    SonareMixer* mixer = mixer_from(scene);
    SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "source");
    REQUIRE(strip != nullptr);
    CHECK(query_tail(mixer) == short_tail);
    if (target_ms > 0.0f) {
      for (unsigned int id : {0u, 1u}) {
        REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, id, 0, target_ms, 2) ==
                SONARE_OK);
      }
    }
    // Silent blocks consume the event, then a late impulse shows the new delay.
    std::vector<float> silent(kBlock, 0.0f);
    const float* in[] = {silent.data()};
    std::vector<float> out_l(kBlock);
    std::vector<float> out_r(kBlock);
    for (int block = 0; block < 50; ++block) {
      REQUIRE(sonare_mixer_process_stereo(mixer, in, in, 1, out_l.data(), out_r.data(), kBlock) ==
              SONARE_OK);
    }
    const int live = query_tail(mixer);
    CHECK(live == query_tail(mixer));
    CHECK(live == expected);
    REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
    CHECK(query_tail(mixer) == live);
    const int last = last_audible_frame(mixer, 1, 40);
    CHECK(last >= 0);
    CHECK(last <= live);
    sonare_mixer_destroy(mixer);
  }
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH && SONARE_WITH_FX
