/// @file routing_arrival_plan_test.cpp
/// @brief Graph arrival plan: direct inputs, detector taps and recompile state.

#include "mastering/api/insert_factory.h"
#include "routing_test_helpers.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

namespace {

// Reports a Q8 latency on every output and an input tap on one port, but passes
// audio through untouched, so the graph's own compensation is all that moves it.
class ReportedLatency final : public sonare::rt::ProcessorBase {
 public:
  ReportedLatency(int latency_q8, int tap_port = -1, int tap_q8 = 0)
      : latency_q8_(latency_q8), tap_port_(tap_port), tap_q8_(tap_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_q8_; }
  int input_tap_latency_samples_q8(int port) const noexcept override {
    return port == tap_port_ ? tap_q8_ : 0;
  }

 private:
  int latency_q8_;
  int tap_port_;
  int tap_q8_;
};

// 0.16666667 ms of lookahead is exactly 8 samples at 48 kHz; threshold 24 dB
// keeps the limiter's gain at unity, so it is a pure 8-sample delay.
const char* kNeutralLimiter = R"({"thresholdDb":24,"lookaheadMs":0.16666667,"releaseMs":50})";
const char* kRouter = R"({"thresholdDb":-20,"ratio":20,"rangeDb":18,"attackMs":0,"releaseMs":0})";
constexpr int kSr = 48000;
constexpr int kImpulseFrame = 16;
constexpr float kImpulse = 0.5f;

sonare::mixing::api::Insert insert(sonare::mixing::api::InsertSlot slot, const char* name,
                                   const char* params, const char* key = "") {
  return {slot, name, params, key};
}

struct Render {
  std::vector<float> left;
  int latency = -1;
};

// Feeds @p inputs (one mono signal per strip, duplicated to both channels) in
// blocks of @p block and returns the master's left channel.
Render render(SonareMixer* mixer, const std::vector<std::vector<float>>& inputs, int block) {
  const size_t total = inputs.front().size();
  Render out;
  out.left.assign(total, 0.0f);
  std::vector<float> right(static_cast<size_t>(block));
  std::vector<const float*> pointers(inputs.size());
  for (size_t start = 0; start < total; start += static_cast<size_t>(block)) {
    const size_t n = std::min(static_cast<size_t>(block), total - start);
    for (size_t s = 0; s < inputs.size(); ++s) pointers[s] = inputs[s].data() + start;
    REQUIRE(sonare_mixer_process_stereo(mixer, pointers.data(), pointers.data(), inputs.size(),
                                        out.left.data() + start, right.data(), n) == SONARE_OK);
  }
  REQUIRE(sonare_mixer_latency_samples(mixer, &out.latency) == SONARE_OK);
  return out;
}

std::vector<float> impulse(size_t length) {
  std::vector<float> signal(length, 0.0f);
  signal[kImpulseFrame] = kImpulse;
  return signal;
}

size_t peak_index(const std::vector<float>& signal) {
  size_t best = 0;
  for (size_t i = 1; i < signal.size(); ++i) {
    if (std::abs(signal[i]) > std::abs(signal[best])) best = i;
  }
  return best;
}

SonareMixer* mixer_from(const sonare::mixing::api::Scene& scene, int block) {
  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, block);
  REQUIRE(mixer != nullptr);
  return mixer;
}

void require_same(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    INFO("frame " << i);
    REQUIRE_THAT(a[i], WithinAbs(b[i], 1.0e-6f));
  }
}

}  // namespace

TEST_CASE("Graph aligns each edge with its destination input tap", "[graph][mixing][pdc]") {
  // Key path 8.5 samples, tap 3.25 behind the node input: the program waits
  // 5.25 and the key lands on the tap with no delay of its own.
  for (const int tap_q8 : {3 * 256 + 64, 12 * 256}) {
    INFO("tap q8 " << tap_q8);
    sonare::graph::Graph graph;
    REQUIRE(graph.add_node("program", std::make_unique<ReportedLatency>(0), 1));
    REQUIRE(graph.add_node("key", std::make_unique<ReportedLatency>(8 * 256 + 128), 1));
    REQUIRE(graph.add_node("dest", std::make_unique<ReportedLatency>(0, 1, tap_q8), 2));
    REQUIRE(graph.connect({"program", 0, "dest", 0}));
    REQUIRE(graph.connect({"key", 0, "dest", 1}));
    REQUIRE(graph.compile());
    const int key_path = 8 * 256 + 128;
    const int arrival = std::max(0, key_path - tap_q8);
    CHECK(graph.node_latency_samples_q8("dest") == arrival);
    CHECK(graph.connection_delay_samples_q8(0) == arrival);
    CHECK(graph.connection_delay_samples_q8(1) == arrival + tap_q8 - key_path);
  }
}

TEST_CASE("Graph rebuild adopts queued delay audio on unchanged edges", "[graph][mixing][pdc]") {
  constexpr int kBlock = 16;
  // Integer and fractional compensation on the dry edge.
  for (const int latent_q8 : {40 * 256, 37 * 256 + 96}) {
    INFO("latency q8 " << latent_q8);
    const auto build = [&](int latency_q8) {
      auto graph = std::make_unique<sonare::graph::Graph>();
      REQUIRE(graph->add_node("dry", std::make_unique<ReportedLatency>(0), 1));
      REQUIRE(graph->add_node("wet", std::make_unique<ReportedLatency>(latency_q8), 1));
      REQUIRE(graph->add_node("out", std::make_unique<ReportedLatency>(0), 1));
      REQUIRE(graph->connect({"dry", 0, "out", 0}));
      REQUIRE(graph->connect({"wet", 0, "out", 0}));
      graph->prepare(kSr, kBlock);
      return graph;
    };
    const auto run = [&](sonare::graph::Graph& graph, int block_index) {
      std::vector<float> in(kBlock, 0.0f);
      for (int i = 0; i < kBlock; ++i)
        in[static_cast<size_t>(i)] = 0.01f * (block_index * kBlock + i);
      graph.clear_inputs(kBlock);
      graph.set_input("dry", 0, in.data(), kBlock);
      graph.process_block(kBlock);
      const float* out = graph.output("out", 0);
      return std::vector<float>(out, out + kBlock);
    };

    auto uninterrupted = build(latent_q8);
    auto rebuilt = build(latent_q8);
    auto changed = build(latent_q8);
    for (int block = 0; block < 4; ++block) {
      run(*uninterrupted, block);
      run(*rebuilt, block);
      run(*changed, block);
    }
    auto replacement = build(latent_q8);
    replacement->adopt_connection_state(*rebuilt);
    // A different compensation delay is a changed edge and starts empty.
    auto retimed = build(latent_q8 + 256);
    retimed->adopt_connection_state(*changed);

    bool retimed_differs = false;
    for (int block = 4; block < 8; ++block) {
      const auto expected = run(*uninterrupted, block);
      const auto got = run(*replacement, block);
      require_same(got, expected);
      const auto fresh = run(*retimed, block);
      if (block == 4) retimed_differs = std::abs(fresh[0]) < std::abs(expected[0]) * 0.5f;
    }
    CHECK(retimed_differs);
  }
}

TEST_CASE("Direct strip input aligns with a latent incoming key", "[mixing][capi][pdc]") {
  constexpr size_t kLength = 256;
  // direct: "bed" takes the program directly; upstream: "program" feeds "bed".
  const auto scene_for = [](bool upstream, bool keyed) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});
    sonare::mixing::api::Strip key;
    key.id = "key";
    key.inserts.push_back(
        insert(sonare::mixing::api::InsertSlot::PreFader, "dynamics.limiter", kNeutralLimiter));
    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    bed.inserts.push_back(insert(sonare::mixing::api::InsertSlot::PreFader,
                                 "dynamics.sidechainRouter", kRouter, keyed ? "key" : ""));
    scene.strips.push_back(key);
    if (upstream) {
      sonare::mixing::api::Strip program;
      program.id = "program";
      scene.strips.push_back(program);
      scene.connections.push_back({"program", "bed"});
    }
    scene.strips.push_back(bed);
    scene.connections.push_back({"key", "discard"});
    scene.connections.push_back({"bed", "master"});
    return scene;
  };
  const std::vector<std::vector<float>> inputs{impulse(kLength), impulse(kLength)};

  for (const int block : {64, 13}) {
    INFO("block " << block);
    SonareMixer* direct_mixer = mixer_from(scene_for(false, true), 64);
    SonareMixer* upstream_mixer = mixer_from(scene_for(true, true), 64);
    const Render direct = render(direct_mixer, inputs, block);
    const Render upstream = render(upstream_mixer, inputs, block);
    sonare_mixer_destroy(direct_mixer);
    sonare_mixer_destroy(upstream_mixer);

    CHECK(direct.latency == 8);
    CHECK(upstream.latency == 8);
    CHECK(peak_index(direct.left) == kImpulseFrame + 8);
    // Ducked: well under the 0.5 impulse.
    CHECK(std::abs(direct.left[kImpulseFrame + 8]) < 0.2f);
    CHECK(std::abs(direct.left[kImpulseFrame + 8]) > 0.05f);
    require_same(direct.left, upstream.left);
  }

  // No key: nothing latent reaches the bed, which ducks on its own program.
  SonareMixer* unkeyed = mixer_from(scene_for(false, false), 64);
  const Render self = render(unkeyed, inputs, 64);
  sonare_mixer_destroy(unkeyed);
  CHECK(self.latency == 0);
  CHECK(peak_index(self.left) == static_cast<size_t>(kImpulseFrame));
  CHECK(std::abs(self.left[kImpulseFrame]) < 0.2f);
}

TEST_CASE("Sidechain key aligns with the detector behind earlier target inserts",
          "[mixing][capi][pdc]") {
  constexpr size_t kLength = 256;
  using sonare::mixing::api::InsertSlot;
  enum class Target { StripPre, StripPost, Bus };
  // inside: the limiter precedes the router on the target; otherwise it sits
  // on an upstream strip and the target holds only the router.
  const auto scene_for = [](Target target, bool inside, bool keyed) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});
    sonare::mixing::api::Strip key;
    key.id = "key";
    sonare::mixing::api::Strip program;
    program.id = "program";
    if (!inside) {
      program.inserts.push_back(insert(InsertSlot::PreFader, "dynamics.limiter", kNeutralLimiter));
    }
    std::vector<sonare::mixing::api::Insert> chain;
    if (inside) chain.push_back(insert(InsertSlot::PreFader, "dynamics.limiter", kNeutralLimiter));
    chain.push_back(
        insert(target == Target::StripPost ? InsertSlot::PostFader : InsertSlot::PreFader,
               "dynamics.sidechainRouter", kRouter, keyed ? "key" : ""));
    scene.strips.push_back(key);
    scene.strips.push_back(program);
    scene.connections.push_back({"key", "discard"});
    if (target == Target::Bus) {
      sonare::mixing::api::Bus group{"group", "aux"};
      for (auto& entry : chain) entry.slot = InsertSlot::PostFader;
      group.inserts = chain;
      scene.buses.push_back(group);
      scene.connections.push_back({"program", "group"});
      scene.connections.push_back({"group", "master"});
    } else {
      sonare::mixing::api::Strip bed;
      bed.id = "bed";
      bed.inserts = chain;
      scene.strips.push_back(bed);
      scene.connections.push_back({"program", "bed"});
      scene.connections.push_back({"bed", "master"});
    }
    return scene;
  };
  const std::vector<std::vector<float>> inputs{impulse(kLength), impulse(kLength)};

  for (const Target target : {Target::StripPre, Target::StripPost, Target::Bus}) {
    for (const bool keyed : {true, false}) {
      INFO("target " << static_cast<int>(target) << " keyed " << keyed);
      SonareMixer* inside_mixer = mixer_from(scene_for(target, true, keyed), 64);
      SonareMixer* upstream_mixer = mixer_from(scene_for(target, false, keyed), 64);
      const Render inside = render(inside_mixer, inputs, 64);
      const Render upstream = render(upstream_mixer, inputs, 64);
      sonare_mixer_destroy(inside_mixer);
      sonare_mixer_destroy(upstream_mixer);

      CHECK(inside.latency == 8);
      CHECK(upstream.latency == 8);
      CHECK(peak_index(inside.left) == kImpulseFrame + 8);
      CHECK(std::abs(inside.left[kImpulseFrame + 8]) < 0.2f);
      CHECK(std::abs(inside.left[kImpulseFrame + 8]) > 0.05f);
      require_same(inside.left, upstream.left);
    }
  }
}

TEST_CASE("Mixer recompile keeps queued compensation audio on unchanged routes",
          "[mixing][capi][pdc]") {
  constexpr int kBlock = 64;
  constexpr size_t kWarm = 2048;
  constexpr size_t kAfter = 1024;
  enum class Refresh { None, Compile, AddSilentStrip };
  // Parallel silent latent bus: the dry route carries the whole compensation.
  // A 5 ms limiter is 240 samples; overdrive's oversampler leaves a fraction.
  const auto overdrive = sonare::mastering::api::make_insert("saturation.overdrive", "{}");
  REQUIRE(overdrive != nullptr);
  overdrive->prepare(kSr, kBlock);
  REQUIRE((overdrive->latency_samples_q8() & 0xff) != 0);

  struct Latent {
    const char* name;
    const char* params;
  };
  const Latent cases[] = {
      {"dynamics.limiter", R"({"thresholdDb":24,"lookaheadMs":5,"releaseMs":50})"},
      {"saturation.overdrive", "{}"},
      {nullptr, nullptr},
  };
  for (const Latent& latent : cases) {
    INFO("latent " << (latent.name ? latent.name : "none"));
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    sonare::mixing::api::Bus aux{"aux", "aux"};
    if (latent.name != nullptr) {
      aux.inserts.push_back(
          insert(sonare::mixing::api::InsertSlot::PostFader, latent.name, latent.params));
    }
    scene.buses.push_back(aux);
    sonare::mixing::api::Strip dry;
    dry.id = "dry";
    sonare::mixing::api::Strip silent;
    silent.id = "silent";
    scene.strips = {dry, silent};
    scene.connections.push_back({"dry", "master"});
    scene.connections.push_back({"silent", "aux"});
    scene.connections.push_back({"aux", "master"});

    const auto run = [&](Refresh refresh) {
      SonareMixer* mixer = mixer_from(scene, kBlock);
      const std::vector<std::vector<float>> warm{std::vector<float>(kWarm, 0.08f),
                                                 std::vector<float>(kWarm, 0.0f)};
      const Render before = render(mixer, warm, kBlock);
      if (refresh == Refresh::Compile) REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
      if (refresh == Refresh::AddSilentStrip) {
        REQUIRE(sonare_mixer_add_strip(mixer, "extra") != nullptr);
      }
      const std::vector<std::vector<float>> held{std::vector<float>(kAfter, 0.08f),
                                                 std::vector<float>(kAfter, 0.0f)};
      const Render steady = render(mixer, held, kBlock);
      const std::vector<std::vector<float>> stopped{std::vector<float>(kAfter, 0.0f),
                                                    std::vector<float>(kAfter, 0.0f)};
      const Render drained = render(mixer, stopped, kBlock);
      sonare_mixer_destroy(mixer);
      CHECK(steady.latency == before.latency);
      std::vector<float> after = steady.left;
      after.insert(after.end(), drained.left.begin(), drained.left.end());
      return std::pair<int, std::vector<float>>{before.latency, after};
    };

    const auto reference = run(Refresh::None);
    if (latent.name != nullptr) {
      CHECK(reference.first > 0);
      // Non-vacuity: the queued tail reaches the output after input stops.
      CHECK(std::abs(reference.second[kAfter]) > 0.07f);
    } else {
      CHECK(reference.first == 0);
    }
    require_same(run(Refresh::Compile).second, reference.second);
    require_same(run(Refresh::AddSilentStrip).second, reference.second);
  }
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
