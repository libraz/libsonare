/// @file routing_latency_test.cpp
/// @brief Routed mixer latency and graph output tests.

#include "routing_test_helpers.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

TEST_CASE("Routed mixer sends reach the bus and return to master", "[mixing][routing]") {
  // Headline regression: the vocalReverbSend preset routes the vocal strip's
  // post-fader send to the "vocal-verb" aux bus, which feeds the plate-reverb
  // return strip, which sums back into master. A correctly routed graph must
  // produce both a dry/early-reflection hit (block 0) and a decaying reverb
  // tail (later blocks). Without the routing fix the tail energy would be ~0.
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;

  char* json = nullptr;
  REQUIRE(sonare_mixing_scene_preset_json("vocalReverbSend", &json) == SONARE_OK);
  REQUIRE(json != nullptr);

  SonareMixer* mixer = sonare_mixer_from_scene_json(json, kSr, kBlock);
  sonare_free_string(json);
  REQUIRE(mixer != nullptr);

  // Strip 0 is "vocal" (verified in src/mixing/api/presets.cpp:make_vocal_reverb_send);
  // strip 1 is the "vocal-verb-return". Feed an impulse into vocal, silence into
  // the return's direct input.
  std::array<float, kBlock> vocal_l{};
  std::array<float, kBlock> vocal_r{};
  vocal_l[0] = 1.0f;
  vocal_r[0] = 1.0f;
  std::array<float, kBlock> silent_l{};
  std::array<float, kBlock> silent_r{};

  std::vector<double> energies;
  for (int block = 0; block < 16; ++block) {
    const float* in_l[] = {vocal_l.data(), silent_l.data()};
    const float* in_r[] = {vocal_r.data(), silent_r.data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    energies.push_back(block_energy(out_l, out_r));

    // After the first block, stop feeding the impulse so any later energy can
    // only come from the reverb return path.
    vocal_l[0] = 0.0f;
    vocal_r[0] = 0.0f;
  }

  // (a) Block 0 carries the dry signal plus early reflections.
  REQUIRE(energies[0] > 1.0e-6);
  // (b) A later block still carries energy: the send routed through the plate
  // reverb on the return strip and summed back into master.
  double tail_energy = 0.0;
  for (size_t i = 4; i < energies.size(); ++i) {
    tail_energy += energies[i];
  }
  REQUIRE(tail_energy > 1.0e-6);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer compensates plugin latency at the master bus", "[mixing][routing]") {
  // Path taken: C++/graph level (the fallback described in the task).
  //
  // Rationale: the only make_insert()-constructible processor that reports a
  // nonzero latency_samples() with default params is eq.linearPhase (256 samples,
  // see LinearPhaseEq::rebuild_kernel -> latency_samples_ = kernel_size/2). But it
  // applies a windowed-FIR identity kernel, which scales and slightly spreads the
  // impulse, so a clean "single coincident impulse of ~2.0" assertion is not
  // reliable through the C/JSON insert path. Instead we build the routing graph
  // manually with two ChannelStrips feeding an FxBus master node (mirroring the
  // StripNode main-path pattern in src/sonare_c_mixing.cpp) and give one strip a
  // FixedLatencyProcessor pre-insert with a known integer latency. This yields a
  // deterministic, exactly-coincident summed impulse for the PDC assertion.
  constexpr int kLatency = 8;
  constexpr int kBlock = 64;
  constexpr double kSr = 48000.0;

  sonare::graph::Graph graph;

  auto dry = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto fx = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  fx->add_pre_insert(std::make_unique<FixedLatencyProcessor>(kLatency));

  // Strips and an FxBus master must be prepared (StripNode forwards no prepare,
  // so prepare the strips here as add_strip() would).
  dry->prepare(kSr, kBlock);
  fx->prepare(kSr, kBlock);
  REQUIRE(fx->latency_samples() == kLatency);

  REQUIRE(graph.add_node("dry", std::move(dry), 2));
  REQUIRE(graph.add_node("fx", std::move(fx), 2));
  REQUIRE(graph.add_node("master", std::make_unique<sonare::mixing::FxBus>(), 2));
  REQUIRE(graph.connect({"dry", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"dry", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"fx", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"fx", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(kSr, kBlock);

  std::array<float, kBlock> impulse{};
  impulse[0] = 1.0f;
  graph.clear_inputs(kBlock);
  graph.set_input("dry", 0, impulse.data(), kBlock);
  graph.set_input("dry", 1, impulse.data(), kBlock);
  graph.set_input("fx", 0, impulse.data(), kBlock);
  graph.set_input("fx", 1, impulse.data(), kBlock);
  graph.process_block(kBlock);

  const float* out_l = graph.output("master", 0);
  const float* out_r = graph.output("master", 1);
  REQUIRE(out_l != nullptr);
  REQUIRE(out_r != nullptr);

  // The graph delays the dry path by kLatency to align it with the fx path.
  // Both contributions must arrive coincidentally at sample kLatency, summing
  // to ~2.0 per channel rather than appearing as two separate peaks.
  REQUIRE_THAT(out_l[kLatency], WithinAbs(2.0f, 0.001f));
  REQUIRE_THAT(out_r[kLatency], WithinAbs(2.0f, 0.001f));

  // No energy before the aligned impulse (no early, un-delayed dry peak at 0).
  double pre_energy = 0.0;
  for (int i = 0; i < kLatency; ++i) {
    pre_energy +=
        static_cast<double>(out_l[i]) * out_l[i] + static_cast<double>(out_r[i]) * out_r[i];
  }
  REQUIRE_THAT(static_cast<float>(pre_energy), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("Routed mixer compensates bus insert latency", "[mixing][routing]") {
  constexpr int kLatency = 8;
  constexpr int kBlock = 64;
  constexpr double kSr = 48000.0;

  sonare::graph::Graph graph;
  auto direct = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto via_bus = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto bus = std::make_unique<sonare::mixing::FxBus>();
  bus->add_insert(std::make_unique<FixedLatencyProcessor>(kLatency));

  REQUIRE(graph.add_node("direct", std::move(direct), 2));
  REQUIRE(graph.add_node("via-bus", std::move(via_bus), 2));
  REQUIRE(graph.add_node("sub", std::move(bus), 2));
  REQUIRE(graph.add_node("master", std::make_unique<sonare::mixing::FxBus>(), 2));
  REQUIRE(graph.connect({"direct", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"direct", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"via-bus", 0, "sub", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"via-bus", 1, "sub", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"sub", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"sub", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(kSr, kBlock);

  std::array<float, kBlock> impulse{};
  impulse[0] = 1.0f;
  graph.clear_inputs(kBlock);
  graph.set_input("direct", 0, impulse.data(), kBlock);
  graph.set_input("direct", 1, impulse.data(), kBlock);
  graph.set_input("via-bus", 0, impulse.data(), kBlock);
  graph.set_input("via-bus", 1, impulse.data(), kBlock);
  graph.process_block(kBlock);

  const float* out_l = graph.output("master", 0);
  const float* out_r = graph.output("master", 1);
  REQUIRE(out_l != nullptr);
  REQUIRE(out_r != nullptr);
  REQUIRE_THAT(out_l[kLatency], WithinAbs(2.0f, 0.001f));
  REQUIRE_THAT(out_r[kLatency], WithinAbs(2.0f, 0.001f));

  double pre_energy = 0.0;
  for (int i = 0; i < kLatency; ++i) {
    pre_energy +=
        static_cast<double>(out_l[i]) * out_l[i] + static_cast<double>(out_r[i]) * out_r[i];
  }
  REQUIRE_THAT(static_cast<float>(pre_energy), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("Routed mixer aligns linear-phase EQ insert while strip automation runs",
          "[mixing][routing][eq]") {
  constexpr int kLatency = 256;
  constexpr int kBlock = 1024;
  constexpr int kSecondPulse = 200;
  constexpr double kSr = 48000.0;
  constexpr float kHalfGainDb = -6.0205999f;

  auto dry = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto eq = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  eq->add_pre_insert(std::make_unique<sonare::mastering::eq::LinearPhaseEq>(
      sonare::mastering::eq::LinearPhaseEqConfig{}));

  dry->prepare(kSr, kBlock);
  eq->prepare(kSr, kBlock);
  REQUIRE(eq->latency_samples() == kLatency);
  REQUIRE(dry->schedule_fader_automation(128, kHalfGainDb));

  sonare::mixing::ChannelStrip* dry_raw = dry.get();
  sonare::mixing::ChannelStrip* eq_raw = eq.get();
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("dry", std::make_unique<TestStripNode>(dry_raw, 0), 2));
  REQUIRE(graph.add_node("eq", std::make_unique<TestStripNode>(eq_raw, 0), 2));
  REQUIRE(graph.add_node("master", std::make_unique<sonare::mixing::FxBus>(), 2));
  REQUIRE(graph.connect({"dry", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"dry", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"eq", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"eq", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(kSr, kBlock);

  std::array<float, kBlock> input{};
  input[0] = 1.0f;
  input[kSecondPulse] = 1.0f;
  graph.clear_inputs(kBlock);
  graph.set_input("dry", 0, input.data(), kBlock);
  graph.set_input("dry", 1, input.data(), kBlock);
  graph.set_input("eq", 0, input.data(), kBlock);
  graph.set_input("eq", 1, input.data(), kBlock);
  graph.process_block(kBlock);

  const float* out_l = graph.output("master", 0);
  const float* out_r = graph.output("master", 1);
  REQUIRE(out_l != nullptr);
  REQUIRE(out_r != nullptr);

  REQUIRE_THAT(out_l[kLatency], WithinAbs(2.0f, 0.01f));
  REQUIRE_THAT(out_r[kLatency], WithinAbs(2.0f, 0.01f));
  REQUIRE_THAT(out_l[kLatency + kSecondPulse], WithinAbs(1.5f, 0.02f));
  REQUIRE_THAT(out_r[kLatency + kSecondPulse], WithinAbs(1.5f, 0.02f));

  double pre_energy = 0.0;
  for (int i = 0; i < kLatency; ++i) {
    pre_energy +=
        static_cast<double>(out_l[i]) * out_l[i] + static_cast<double>(out_r[i]) * out_r[i];
  }
  REQUIRE_THAT(static_cast<float>(pre_energy), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("Graph master output reports LUFS and true peak", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;
  constexpr int kBlocks = 96;

  sonare::graph::Graph graph;
  auto strip = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{-3.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  REQUIRE(graph.add_node("strip", std::move(strip), 2));
  REQUIRE(graph.add_node("master", std::make_unique<sonare::mixing::FxBus>(), 2));
  REQUIRE(graph.connect({"strip", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"strip", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(static_cast<double>(kSr), kBlock);

  sonare::mixing::MeterProcessor meter(sonare::mixing::MeterConfig{true, true, 4, 0.0f});
  meter.prepare(static_cast<double>(kSr), kBlock);

  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  std::array<float, kBlock> meter_l{};
  std::array<float, kBlock> meter_r{};
  for (int block = 0; block < kBlocks; ++block) {
    for (int i = 0; i < kBlock; ++i) {
      const int n = block * kBlock + i;
      const float s = 0.6f * std::sin(sonare::constants::kTwoPi * 440.0f * static_cast<float>(n) /
                                      static_cast<float>(kSr));
      in_l[static_cast<size_t>(i)] = s;
      in_r[static_cast<size_t>(i)] = s;
    }

    graph.clear_inputs(kBlock);
    graph.set_input("strip", 0, in_l.data(), kBlock);
    graph.set_input("strip", 1, in_r.data(), kBlock);
    graph.process_block(kBlock);

    const float* out_l = graph.output("master", 0);
    const float* out_r = graph.output("master", 1);
    REQUIRE(out_l != nullptr);
    REQUIRE(out_r != nullptr);
    std::copy(out_l, out_l + kBlock, meter_l.begin());
    std::copy(out_r, out_r + kBlock, meter_r.begin());
    float* meter_channels[2] = {meter_l.data(), meter_r.data()};
    meter.process(meter_channels, 2, kBlock);
  }

  const sonare::mixing::MeterSnapshot snapshot = meter.snapshot();
  REQUIRE(snapshot.seq >= static_cast<uint64_t>(kBlocks));
  REQUIRE(std::isfinite(snapshot.momentary_lufs));
  REQUIRE(snapshot.momentary_lufs > -70.0f);
  REQUIRE(std::isfinite(snapshot.integrated_lufs));
  REQUIRE(snapshot.integrated_lufs > -70.0f);
  REQUIRE(snapshot.max_true_peak_db > sonare::constants::kFloorDb);
}

TEST_CASE("Routed mixer compensates pre-fader send latency separately from main output",
          "[mixing][routing]") {
  constexpr int kLatency = 8;
  constexpr int kBlock = 64;
  constexpr double kSr = 48000.0;

  auto strip = std::make_unique<sonare::mixing::ChannelStrip>(
      sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip->add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  strip->add_post_insert(std::make_unique<FixedLatencyProcessor>(kLatency));
  strip->prepare(kSr, kBlock);

  REQUIRE(strip->send_latency_samples_q8(0) == 0);
  REQUIRE(strip->post_fader_latency_samples_q8() == (kLatency << 8));

  sonare::mixing::ChannelStrip* raw_strip = strip.get();
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("strip", std::make_unique<TestStripNode>(raw_strip, 1), 4));
  REQUIRE(graph.add_node("master", std::make_unique<sonare::mixing::FxBus>(), 2));
  REQUIRE(graph.connect({"strip", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"strip", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"strip", 2, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"strip", 3, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(kSr, kBlock);

  std::array<float, kBlock> impulse{};
  impulse[0] = 1.0f;
  graph.clear_inputs(kBlock);
  graph.set_input("strip", 0, impulse.data(), kBlock);
  graph.set_input("strip", 1, impulse.data(), kBlock);
  graph.process_block(kBlock);

  const float* out_l = graph.output("master", 0);
  const float* out_r = graph.output("master", 1);
  REQUIRE(out_l != nullptr);
  REQUIRE(out_r != nullptr);
  REQUIRE_THAT(out_l[kLatency], WithinAbs(2.0f, 0.001f));
  REQUIRE_THAT(out_r[kLatency], WithinAbs(2.0f, 0.001f));

  double pre_energy = 0.0;
  for (int i = 0; i < kLatency; ++i) {
    pre_energy +=
        static_cast<double>(out_l[i]) * out_l[i] + static_cast<double>(out_r[i]) * out_r[i];
  }
  REQUIRE_THAT(static_cast<float>(pre_energy), WithinAbs(0.0f, 0.001f));
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
