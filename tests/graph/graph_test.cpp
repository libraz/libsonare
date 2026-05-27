#include "graph/graph.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <vector>

#ifdef SONARE_WITH_MIXING
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#endif
#include "rt/processor_base.h"

using Catch::Matchers::WithinAbs;

namespace {

class PassthroughProcessor : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
};

class GainProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit GainProcessor(float gain) : gain_(gain) {}

  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}

 private:
  float gain_ = 1.0f;
};

class FixedLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit FixedLatencyProcessor(int latency) : latency_(latency) {}

  void prepare(double, int) override { delay_.assign(static_cast<size_t>(latency_), 0.0f); }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (latency_ == 0) {
      return;
    }
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        const float input = channels[ch][i];
        channels[ch][i] = delay_[static_cast<size_t>(write_index_)];
        delay_[static_cast<size_t>(write_index_)] = input;
        write_index_ = (write_index_ + 1) % latency_;
      }
    }
  }

  void reset() override {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    write_index_ = 0;
  }

  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  int write_index_ = 0;
  std::vector<float> delay_;
};

class FixedQ8LatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit FixedQ8LatencyProcessor(int latency_q8) : latency_q8_(latency_q8) {}

  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_q8_; }

 private:
  int latency_q8_ = 0;
};

std::unique_ptr<sonare::rt::ProcessorBase> pass() {
  return std::make_unique<PassthroughProcessor>();
}

}  // namespace

TEST_CASE("Graph compiles acyclic routing in topological order", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("input", pass(), 1));
  REQUIRE(graph.add_node("gain", std::make_unique<GainProcessor>(2.0f), 1));
  REQUIRE(graph.add_node("out", pass(), 1));
  REQUIRE(graph.connect({"input", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 4);

  const std::array<float, 4> input{1.0f, 2.0f, 3.0f, 4.0f};
  graph.clear_inputs(4);
  graph.set_input("input", 0, input.data(), 4);
  graph.process_block(4);

  const float* output = graph.output("out", 0);
  REQUIRE(output != nullptr);
  REQUIRE_THAT(output[0], WithinAbs(2.0f, 0.0001f));
  REQUIRE_THAT(output[3], WithinAbs(8.0f, 0.0001f));
}

TEST_CASE("Graph rejects feedback cycles", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("a", pass(), 1));
  REQUIRE(graph.add_node("b", pass(), 1));
  REQUIRE(graph.connect({"a", 0, "b", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"b", 0, "a", 0, sonare::graph::Connection::Mix::Add}));

  REQUIRE_FALSE(graph.compile());
  REQUIRE_FALSE(graph.compiled());
}

TEST_CASE("Graph supports replace and add connection mixing", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("a", pass(), 1));
  REQUIRE(graph.add_node("b", pass(), 1));
  REQUIRE(graph.add_node("out", pass(), 1));
  REQUIRE(graph.connect({"a", 0, "out", 0, sonare::graph::Connection::Mix::Replace}));
  REQUIRE(graph.connect({"b", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 4);

  const std::array<float, 4> a{1.0f, 1.0f, 1.0f, 1.0f};
  const std::array<float, 4> b{2.0f, 2.0f, 2.0f, 2.0f};
  graph.clear_inputs(4);
  graph.set_input("a", 0, a.data(), 4);
  graph.set_input("b", 0, b.data(), 4);
  graph.process_block(4);

  const float* output = graph.output("out", 0);
  REQUIRE(output != nullptr);
  REQUIRE_THAT(output[0], WithinAbs(3.0f, 0.0001f));
}

TEST_CASE("Graph latency compensation aligns parallel paths", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("fast", pass(), 1));
  REQUIRE(graph.add_node("slow", std::make_unique<FixedLatencyProcessor>(2), 1));
  REQUIRE(graph.add_node("sum", pass(), 1));
  REQUIRE(graph.connect({"fast", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"slow", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  REQUIRE(graph.connection_delay_samples(0) == 2);
  REQUIRE(graph.connection_delay_samples(1) == 0);
  graph.prepare(48000.0, 8);

  const std::array<float, 8> impulse{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  graph.clear_inputs(8);
  graph.set_input("fast", 0, impulse.data(), 8);
  graph.set_input("slow", 0, impulse.data(), 8);
  graph.process_block(8);

  const float* output = graph.output("sum", 0);
  REQUIRE(output != nullptr);
  REQUIRE_THAT(output[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(output[1], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(output[2], WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("Graph exposes Q8 connection delay compensation", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("fast", pass(), 1));
  REQUIRE(graph.add_node("slow", std::make_unique<FixedQ8LatencyProcessor>((2 << 8) + 128), 1));
  REQUIRE(graph.add_node("sum", pass(), 1));
  REQUIRE(graph.connect({"fast", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"slow", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());

  REQUIRE(graph.connection_delay_samples_q8(0) == ((2 << 8) + 128));
  REQUIRE(graph.connection_delay_samples(0) == 2);
  REQUIRE(graph.connection_delay_samples_q8(1) == 0);
}

TEST_CASE("Graph applies fractional Q8 connection delay", "[graph]") {
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("fast", pass(), 1));
  REQUIRE(graph.add_node("slow", std::make_unique<FixedQ8LatencyProcessor>((1 << 8) + 128), 1));
  REQUIRE(graph.add_node("sum", pass(), 1));
  REQUIRE(graph.connect({"fast", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"slow", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  REQUIRE(graph.connection_delay_samples_q8(0) == ((1 << 8) + 128));
  graph.prepare(48000.0, 8);

  const std::array<float, 8> impulse{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  graph.clear_inputs(8);
  graph.set_input("fast", 0, impulse.data(), 8);
  graph.set_input("slow", 0, impulse.data(), 8);
  graph.process_block(8);

  const float* output = graph.output("sum", 0);
  REQUIRE(output != nullptr);
  REQUIRE(output[0] > 0.9f);
  REQUIRE(output[0] < 1.0f);
  float delayed_energy = 0.0f;
  for (int i = 1; i < 8; ++i) {
    delayed_energy += std::abs(output[i]);
  }
  REQUIRE(delayed_energy > 0.5f);
}

#ifdef SONARE_WITH_MIXING
TEST_CASE("Graph processes a ChannelStrip through an FxBus to master", "[graph][mixing]") {
  sonare::graph::Graph graph;

  auto strip = std::make_unique<sonare::mixing::ChannelStrip>();
  strip->set_fader_db(-6.0f);
  strip->set_pan(0.25f);

  auto fx = std::make_unique<sonare::mixing::FxBus>();
  fx->add_insert(std::make_unique<GainProcessor>(0.5f));

  REQUIRE(graph.add_node("strip", std::move(strip), 2));
  REQUIRE(graph.add_node("fx", std::move(fx), 2));
  REQUIRE(graph.add_node("master", pass(), 2));
  REQUIRE(graph.connect({"strip", 0, "fx", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"strip", 1, "fx", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"fx", 0, "master", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"fx", 1, "master", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 8);

  const std::array<float, 8> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  const std::array<float, 8> right{0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
  graph.clear_inputs(8);
  graph.set_input("strip", 0, left.data(), 8);
  graph.set_input("strip", 1, right.data(), 8);
  graph.process_block(8);

  const float* out_l = graph.output("master", 0);
  const float* out_r = graph.output("master", 1);
  REQUIRE(out_l != nullptr);
  REQUIRE(out_r != nullptr);
  REQUIRE(out_l[7] > 0.0f);
  REQUIRE(out_l[7] < left[7]);
  REQUIRE(out_r[7] > 0.0f);
  REQUIRE(out_r[7] < right[7]);
}
#endif
