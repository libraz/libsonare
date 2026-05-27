/// @file routing_test.cpp
/// @brief Tests for the mixer's audio routing (sends -> bus -> return -> master)
///        and routed-mixer plugin delay compensation (PDC).

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "graph/graph.h"
#include "mastering/eq/linear_phase.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/meter.h"
#include "rt/processor_base.h"
#include "sonare_c.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {

// Integer-latency pass-through that buffers its input by `latency` samples.
// Copied from graph_test.cpp's FixedLatencyProcessor so this file is
// self-contained; used to give a ChannelStrip pre-insert a known latency that
// the routing graph must compensate.
class FixedLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit FixedLatencyProcessor(int latency) : latency_(latency) {}

  void prepare(double, int) override {
    delay_.assign(static_cast<size_t>(2 * latency_), 0.0f);
    write_index_.fill(0);
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (latency_ == 0) {
      return;
    }
    for (int ch = 0; ch < num_channels; ++ch) {
      const int row = std::min(ch, 1);
      float* channel_delay = delay_.data() + static_cast<size_t>(row * latency_);
      for (int i = 0; i < num_samples; ++i) {
        const float input = channels[ch][i];
        channels[ch][i] = channel_delay[static_cast<size_t>(write_index_[row])];
        channel_delay[static_cast<size_t>(write_index_[row])] = input;
        write_index_[row] = (write_index_[row] + 1) % latency_;
      }
    }
  }

  void reset() override {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    write_index_.fill(0);
  }

  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  std::array<int, 2> write_index_{};
  std::vector<float> delay_;
};

class TestStripNode final : public sonare::rt::ProcessorBase {
 public:
  TestStripNode(sonare::mixing::ChannelStrip* strip, int num_sends)
      : strip_(strip), num_sends_(num_sends) {}

  void prepare(double, int) override {}
  void reset() override { sample_pos_ = 0; }

  void process(float* const* channels, int, int num_samples) override {
    strip_->process_at(channels, 2, num_samples, sample_pos_);
    for (int s = 0; s < num_sends_; ++s) {
      float* send[2] = {channels[2 + 2 * s], channels[3 + 2 * s]};
      std::fill(send[0], send[0] + num_samples, 0.0f);
      std::fill(send[1], send[1] + num_samples, 0.0f);
      strip_->mix_send_at(static_cast<size_t>(s), send, 2, num_samples, sample_pos_);
    }
    sample_pos_ += num_samples;
  }

  int latency_samples_q8() const noexcept override { return strip_->latency_samples_q8(); }
  int output_latency_samples_q8(int output_port) const noexcept override {
    if (output_port >= 2) {
      return strip_->send_latency_samples_q8(static_cast<size_t>((output_port - 2) / 2));
    }
    return strip_->post_fader_latency_samples_q8();
  }

 private:
  sonare::mixing::ChannelStrip* strip_ = nullptr;
  int num_sends_ = 0;
  int64_t sample_pos_ = 0;
};

// Total energy across both stereo channels of a processed block.
double block_energy(const std::vector<float>& left, const std::vector<float>& right) {
  double sum = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    sum += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
  }
  return sum;
}

}  // namespace

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

TEST_CASE("Routed mixer scene round-trip preserves topology", "[mixing][routing]") {
  // from_scene_json(vocalReverbSend) -> to_scene_json must preserve buses,
  // strips, sends (with destination_bus_id), and connections, including the
  // bus -> return edge that makes the send audible.
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;

  char* preset_json = nullptr;
  REQUIRE(sonare_mixing_scene_preset_json("vocalReverbSend", &preset_json) == SONARE_OK);
  REQUIRE(preset_json != nullptr);

  SonareMixer* mixer = sonare_mixer_from_scene_json(preset_json, kSr, kBlock);
  sonare_free_string(preset_json);
  REQUIRE(mixer != nullptr);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string scene(round_trip);
  sonare_free_string(round_trip);

  // Buses: master plus the vocal-verb aux.
  REQUIRE(scene.find("\"id\":\"master\"") != std::string::npos);
  REQUIRE(scene.find("\"role\":\"master\"") != std::string::npos);
  REQUIRE(scene.find("\"vocal-verb\"") != std::string::npos);

  // Strips: the vocal source and the reverb return.
  REQUIRE(scene.find("\"id\":\"vocal\"") != std::string::npos);
  REQUIRE(scene.find("\"id\":\"vocal-verb-return\"") != std::string::npos);

  // Send survives with its destination bus and timing.
  REQUIRE(scene.find("\"destinationBusId\":\"vocal-verb\"") != std::string::npos);
  REQUIRE(scene.find("\"timing\":\"post\"") != std::string::npos);

  // Connections survive, including the bus -> return edge that routes the send.
  REQUIRE(scene.find("\"source\":\"vocal-verb\",\"destination\":\"vocal-verb-return\"") !=
          std::string::npos);
  REQUIRE(scene.find("\"source\":\"vocal-verb-return\",\"destination\":\"master\"") !=
          std::string::npos);
  REQUIRE(scene.find("\"source\":\"vocal\",\"destination\":\"master\"") != std::string::npos);

  // Re-parsing the round-tripped JSON must rebuild a working mixer.
  SonareMixer* restored = sonare_mixer_from_scene_json(scene.c_str(), kSr, kBlock);
  REQUIRE(restored != nullptr);
  REQUIRE(sonare_mixer_compile(restored) == SONARE_OK);
  sonare_mixer_destroy(restored);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer applies scene bus inserts", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  sonare::mixing::api::Bus master("master", "master");
  master.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "saturation.hardClipper", "{\"ceiling\":0.25}"});
  scene.buses.push_back(master);
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> input{};
  input.fill(1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE_THAT(out_l[0], WithinAbs(0.25f, 0.001f));
  REQUIRE_THAT(out_r[0], WithinAbs(0.25f, 0.001f));
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer delivers scene sidechain keys to strip inserts", "[mixing][routing]") {
  static constexpr int kSr = 48000;
  static constexpr int kBlock = 512;

  auto render = [&](bool with_key) {
    sonare::mixing::api::Scene scene;
    scene.buses.push_back({"master", "master"});
    scene.buses.push_back({"discard", "aux"});

    sonare::mixing::api::Strip host;
    host.id = "host";
    scene.strips.push_back(host);

    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    bed.inserts.push_back({sonare::mixing::api::InsertSlot::PostFader, "dynamics.sidechainRouter",
                           "{\"thresholdDb\":-10,\"rangeDb\":18,\"attackMs\":0,\"releaseMs\":50}",
                           with_key ? "host" : ""});
    scene.strips.push_back(bed);
    scene.connections.push_back({"host", "discard"});
    scene.connections.push_back({"bed", "master"});

    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);

    std::array<float, kBlock> host_l{};
    std::array<float, kBlock> host_r{};
    std::array<float, kBlock> bed_l{};
    std::array<float, kBlock> bed_r{};
    host_l.fill(1.0f);
    host_r.fill(1.0f);
    bed_l.fill(0.05f);
    bed_r.fill(0.05f);

    const float* in_l[] = {host_l.data(), bed_l.data()};
    const float* in_r[] = {host_r.data(), bed_r.data()};
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);

    SonareStrip* bed_strip = sonare_mixer_strip_by_id(mixer, "bed");
    REQUIRE(bed_strip != nullptr);
    SonareMixMeterSnapshot meter{};
    REQUIRE(sonare_strip_meter(bed_strip, &meter) == SONARE_OK);
    const double energy = block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()});
    sonare_mixer_destroy(mixer);
    return std::pair<double, float>{energy, meter.gain_reduction_db};
  };

  const auto without_key = render(false);
  const auto with_key = render(true);
  REQUIRE(with_key.first < without_key.first * 0.35);
  REQUIRE(with_key.second < -3.0f);
  REQUIRE(without_key.second > -0.5f);
}

TEST_CASE("Routed mixer delivers scene sidechain keys to bus inserts", "[mixing][routing]") {
  static constexpr int kSr = 48000;
  static constexpr int kBlock = 512;

  auto render = [&](bool with_key) {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Bus master("master", "master");
    master.inserts.push_back(
        {sonare::mixing::api::InsertSlot::PostFader, "dynamics.sidechainRouter",
         "{\"thresholdDb\":-10,\"rangeDb\":18,\"attackMs\":0,\"releaseMs\":50}",
         with_key ? "host" : ""});
    scene.buses.push_back(master);
    scene.buses.push_back({"discard", "aux"});

    sonare::mixing::api::Strip host;
    host.id = "host";
    scene.strips.push_back(host);
    sonare::mixing::api::Strip bed;
    bed.id = "bed";
    scene.strips.push_back(bed);
    scene.connections.push_back({"host", "discard"});
    scene.connections.push_back({"bed", "master"});

    const std::string json = sonare::mixing::api::scene_to_json(scene);
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);

    std::array<float, kBlock> host_l{};
    std::array<float, kBlock> host_r{};
    std::array<float, kBlock> bed_l{};
    std::array<float, kBlock> bed_r{};
    host_l.fill(1.0f);
    host_r.fill(1.0f);
    bed_l.fill(0.05f);
    bed_r.fill(0.05f);

    const float* in_l[] = {host_l.data(), bed_l.data()};
    const float* in_r[] = {host_r.data(), bed_r.data()};
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);

    const double energy = block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()});
    sonare_mixer_destroy(mixer);
    return energy;
  };

  const double without_key = render(false);
  const double with_key = render(true);
  REQUIRE(with_key < without_key * 0.35);
}

TEST_CASE("Scene-loaded mixer exposes strip meter and goniometer snapshots", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 512;
  constexpr int kBlocks = 96;  // > 1 second, enough for momentary/integrated LUFS.

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip vocal;
  vocal.id = "vocal";
  vocal.input_trim_db = 3.0f;
  vocal.inserts.push_back({sonare::mixing::api::InsertSlot::PreFader, "dynamics.compressor",
                           "{\"thresholdDb\":-30,\"ratio\":4,\"attackMs\":0,\"releaseMs\":50}"});
  scene.strips.push_back(vocal);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"vocal", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_strip_by_id(mixer, "vocal");
  REQUIRE(strip != nullptr);

  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  const float* inputs_l[] = {in_l.data()};
  const float* inputs_r[] = {in_r.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  for (int block = 0; block < kBlocks; ++block) {
    for (int i = 0; i < kBlock; ++i) {
      const int n = block * kBlock + i;
      const float s = 0.7f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n) /
                                      static_cast<float>(kSr));
      in_l[static_cast<size_t>(i)] = s;
      in_r[static_cast<size_t>(i)] = -0.5f * s;
    }
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        kBlock) == SONARE_OK);
  }

  SonareMixMeterSnapshot meter{};
  REQUIRE(sonare_strip_meter(strip, &meter) == SONARE_OK);
  REQUIRE(meter.seq >= static_cast<uint64_t>(kBlocks));
  REQUIRE(meter.gain_reduction_db < -0.1f);
  REQUIRE(meter.max_true_peak_db > sonare::constants::kFloorDb);
  REQUIRE(std::isfinite(meter.momentary_lufs));
  REQUIRE(meter.momentary_lufs > -70.0f);
  REQUIRE(std::isfinite(meter.integrated_lufs));
  REQUIRE(meter.integrated_lufs > -70.0f);

  std::array<SonareMixGoniometerPoint, 8> points{};
  const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), points.size());
  REQUIRE(count == points.size());
  REQUIRE(std::isfinite(points[0].left));
  REQUIRE(std::isfinite(points[0].right));
  REQUIRE(std::abs(points[0].left) > 0.0f);
  REQUIRE(std::abs(points[0].right) > 0.0f);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer scene round-trip preserves VCA groups", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});
  scene.vca_groups.push_back({"lead-vca", -6.0f, {"lead"}});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string restored_json(round_trip);
  sonare_free_string(round_trip);
  sonare_mixer_destroy(mixer);

  const auto restored = sonare::mixing::api::scene_from_json(restored_json);
  REQUIRE(restored.vca_groups.size() == 1);
  REQUIRE(restored.vca_groups[0].id == "lead-vca");
  REQUIRE_THAT(restored.vca_groups[0].gain_db, WithinAbs(-6.0f, 0.001f));
  REQUIRE(restored.vca_groups[0].members == std::vector<std::string>{"lead"});
}

TEST_CASE("Routed mixer applies scene solo to audio output", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip solo;
  solo.id = "solo";
  solo.soloed = true;
  sonare::mixing::api::Strip muted_by_solo;
  muted_by_solo.id = "muted-by-solo";
  scene.strips.push_back(solo);
  scene.strips.push_back(muted_by_solo);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"solo", "master"});
  scene.connections.push_back({"muted-by-solo", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> silent{};
  std::array<float, kBlock> loud{};
  loud.fill(1.0f);
  const float* in_l[] = {silent.data(), loud.data()};
  const float* in_r[] = {silent.data(), loud.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE_THAT(
      static_cast<float>(block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()})),
      WithinAbs(0.0f, 1.0e-6f));
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Routed mixer rejects duplicate scene connections", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer == nullptr);
}

TEST_CASE("Routed mixer does not auto-route explicit unpatched aux buses", "[mixing][routing]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 64;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip lead;
  lead.id = "lead";
  lead.fader_db = -120.0f;
  lead.sends.push_back(
      {"lead-to-aux", "explicit-aux", 0.0f, sonare::mixing::api::SendTiming::PreFader});
  scene.strips.push_back(lead);
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"explicit-aux", "aux"});
  scene.connections.push_back({"lead", "explicit-aux"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  std::array<float, kBlock> input{};
  input.fill(1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);

  REQUIRE(block_energy({out_l.begin(), out_l.end()}, {out_r.begin(), out_r.end()}) < 1.0e-8);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API strip runtime setters validate NULL and bad enums", "[mixing][capi]") {
  // NULL strip handle must be rejected by every setter.
  REQUIRE(sonare_strip_set_soloed(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_solo_safe(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_polarity_invert(nullptr, 1, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_pan_law(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_channel_delay_samples(nullptr, 4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_vca_offset_db(nullptr, -3.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(nullptr, 0, -6.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(nullptr, 0, 0.5f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_width_automation(nullptr, 0, 0.5f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_send_automation(nullptr, 0, 0, -6.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareMixMeterSnapshot snapshot{};
  REQUIRE(sonare_strip_meter_tap(nullptr, SONARE_METER_TAP_PRE_FADER, &snapshot) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareMixer* mixer = sonare_mixer_create(48000, 64);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "s");
  REQUIRE(strip != nullptr);

  // Valid calls on a real strip succeed.
  REQUIRE(sonare_strip_set_soloed(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_solo_safe(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_polarity_invert(strip, 1, 0) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan_law(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 3) == SONARE_OK);
  REQUIRE(sonare_strip_set_vca_offset_db(strip, -2.0f) == SONARE_OK);

  // NULL out-pointer for meter tap is rejected even with a valid strip.
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_PRE_FADER, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Both valid taps succeed.
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_PRE_FADER, &snapshot) == SONARE_OK);
  REQUIRE(sonare_strip_meter_tap(strip, SONARE_METER_TAP_POST_FADER, &snapshot) == SONARE_OK);

  // Invalid enum values are rejected.
  REQUIRE(sonare_strip_set_pan_law(strip, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_meter_tap(strip, 99, &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 0, -6.0f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(strip, 0, 0.5f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_width_automation(strip, 0, 0.5f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_send_automation(strip, 0, 0, -6.0f, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("C-API strip setters reflect into scene JSON for cached fields", "[mixing][capi]") {
  // Load a two-strip scene, mutate a strip through the runtime setters, then
  // serialize and re-parse. Fields that the C layer caches into scene_strip
  // (soloed, solo_safe, polarity, pan_law, channel_delay) must round-trip.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip a;
  a.id = "a";
  sonare::mixing::api::Strip b;
  b.id = "b";
  scene.strips.push_back(a);
  scene.strips.push_back(b);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"a", "master"});
  scene.connections.push_back({"b", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, 64);
  REQUIRE(mixer != nullptr);

  SonareStrip* strip = sonare_mixer_strip_at(mixer, 0);
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_soloed(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_solo_safe(strip, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_polarity_invert(strip, 1, 1) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan_law(strip, 2) == SONARE_OK);  // Const6dB
  REQUIRE(sonare_strip_set_channel_delay_samples(strip, 11) == SONARE_OK);

  char* round_trip = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &round_trip) == SONARE_OK);
  REQUIRE(round_trip != nullptr);
  const std::string restored_json(round_trip);
  sonare_free_string(round_trip);
  sonare_mixer_destroy(mixer);

  const auto parsed = sonare::mixing::api::scene_from_json(restored_json);
  REQUIRE(parsed.strips.size() == 2);
  const auto& out = parsed.strips[0];
  REQUIRE(out.id == "a");
  REQUIRE(out.soloed);
  REQUIRE(out.solo_safe);
  REQUIRE(out.polarity_invert_left);
  REQUIRE(out.polarity_invert_right);
  REQUIRE(out.pan_law == 2);
  REQUIRE(out.channel_delay_samples == 11);
}

TEST_CASE("C-API solo and solo-safe gate the audio output", "[mixing][capi]") {
  // Three strips with distinct constant levels feed the master. Soloing one
  // strip silences the others, except a solo-safe strip which must keep
  // contributing (the regression this guards).
  constexpr int kSr = 48000;
  // Large block so the per-strip pan smoothers settle to their steady gains
  // before the contributions are read at the block tail.
  constexpr int kBlock = 4096;

  sonare::mixing::api::Scene scene;
  for (const char* id : {"a", "b", "c"}) {
    sonare::mixing::api::Strip strip;
    strip.id = id;
    strip.pan_law = 3;  // Linear0dB so the steady L gain is exactly 1.0 (no -3 dB pan).
    scene.strips.push_back(strip);
    scene.connections.push_back({id, "master"});
  }
  scene.buses.push_back({"master", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);

  // Distinct constant levels so each strip's contribution to the master is
  // separable: a -> 1.0, b -> 0.1, c -> 0.01. Summed at the master, the steady
  // L output identifies exactly which strips contributed.
  std::vector<float> a_in(kBlock, 1.0f);
  std::vector<float> b_in(kBlock, 0.1f);
  std::vector<float> c_in(kBlock, 0.01f);

  // Returns the fully-settled master L level (last sample of the block).
  auto process_settled = [&](SonareMixer* mixer) {
    const float* in_l[] = {a_in.data(), b_in.data(), c_in.data()};
    const float* in_r[] = {a_in.data(), b_in.data(), c_in.data()};
    std::vector<float> out_l(kBlock, 0.0f);
    std::vector<float> out_r(kBlock, 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 3, out_l.data(), out_r.data(), kBlock) ==
            SONARE_OK);
    return out_l[kBlock - 1];
  };

  SECTION("no solo lets every strip through") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.11f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }

  SECTION("soloing one strip silences the others") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    SonareStrip* a_strip = sonare_mixer_strip_by_id(mixer, "a");
    REQUIRE(a_strip != nullptr);
    REQUIRE(sonare_strip_set_soloed(a_strip, 1) == SONARE_OK);
    // Only a (1.0) contributes; b and c are implied-muted.
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.0f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }

  SECTION("a solo-safe strip survives another strip's solo") {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
    REQUIRE(mixer != nullptr);
    SonareStrip* a_strip = sonare_mixer_strip_by_id(mixer, "a");
    SonareStrip* c_strip = sonare_mixer_strip_by_id(mixer, "c");
    REQUIRE(a_strip != nullptr);
    REQUIRE(c_strip != nullptr);
    // c is solo-safe; soloing a must NOT mute c, but must mute b.
    REQUIRE(sonare_strip_set_solo_safe(c_strip, 1) == SONARE_OK);
    REQUIRE(sonare_strip_set_soloed(a_strip, 1) == SONARE_OK);
    // a (1.0) + c (0.01) contribute, b (0.1) is silenced.
    REQUIRE_THAT(process_settled(mixer), WithinAbs(1.01f, 1e-3f));
    sonare_mixer_destroy(mixer);
  }
}

TEST_CASE("C-API fader automation changes the strip's effective gain",
          "[mixing][capi][automation]") {
  // Scheduling fader automation and processing past the scheduled sample
  // position must lower the strip's output. -120 dB at sample 0 effectively
  // mutes a constant input.
  constexpr int kSr = 48000;
  // Large block so the pan/fader smoothers fully settle within one block.
  constexpr int kBlock = 4096;

  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "lead";
  strip.pan_law = 3;  // Linear0dB so the settled unity fader yields unity L output.
  scene.strips.push_back(strip);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"lead", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);
  SonareStrip* lead = sonare_mixer_strip_by_id(mixer, "lead");
  REQUIRE(lead != nullptr);

  std::vector<float> input(kBlock, 1.0f);
  const float* in_l[] = {input.data()};
  const float* in_r[] = {input.data()};
  std::vector<float> out_l(kBlock, 0.0f);
  std::vector<float> out_r(kBlock, 0.0f);

  // Baseline block at unity fader passes the signal through at full level once
  // the smoothers have settled by the block tail.
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  REQUIRE(out_l[kBlock - 1] > 0.99f);

  // Schedule a steep drop. The mixer advances its sample position across
  // process calls; place the event at the start of the next block's range.
  REQUIRE(sonare_strip_schedule_fader_automation(lead, kBlock, -120.0f, 0) == SONARE_OK);
  REQUIRE(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, out_l.data(), out_r.data(), kBlock) ==
          SONARE_OK);
  // By the end of the block the smoothed fader has fallen far below unity.
  REQUIRE(out_l[kBlock - 1] < 0.1f);

  sonare_mixer_destroy(mixer);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
