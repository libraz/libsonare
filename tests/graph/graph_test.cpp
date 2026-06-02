#include "graph/graph.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <vector>

#ifdef SONARE_WITH_GRAPH
#include "engine/graph_runtime.h"
#endif
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

TEST_CASE("Graph rejects out-of-range port counts", "[graph]") {
  sonare::graph::Graph graph;
  REQUIRE_FALSE(graph.add_node("zero", pass(), 0));
  REQUIRE_FALSE(graph.add_node("neg", pass(), -1));
  // Above the per-node cap: would overflow num_ports * max_block_size.
  REQUIRE_FALSE(graph.add_node("huge", pass(), sonare::graph::Node::kMaxPorts + 1));
  // At the cap is accepted.
  REQUIRE(graph.add_node("ok", pass(), sonare::graph::Node::kMaxPorts));
}

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

TEST_CASE("Graph topological order of independent nodes follows insertion order", "[graph]") {
  // Independent (unconnected, indegree-0) nodes must compile into a stable,
  // reproducible order derived from add_node insertion order — not the
  // unspecified iteration order of the internal hash map.
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("zeta", pass(), 1));
  REQUIRE(graph.add_node("alpha", pass(), 1));
  REQUIRE(graph.add_node("mike", pass(), 1));
  REQUIRE(graph.add_node("bravo", pass(), 1));
  REQUIRE(graph.compile());

  const std::vector<std::string> expected{"zeta", "alpha", "mike", "bravo"};
  REQUIRE(graph.topo_order_ids() == expected);

  // Recompiling the same topology yields the identical order.
  REQUIRE(graph.compile());
  REQUIRE(graph.topo_order_ids() == expected);
}

TEST_CASE("Graph audio-thread methods are no-ops on an uncompiled graph", "[graph][noexcept]") {
  // Regression: process_block() and clear_inputs() are noexcept and must
  // early-return harmlessly when the graph has not been compiled, so a throw
  // never escapes into the noexcept GraphRuntime::process chain.
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("input", pass(), 1));
  REQUIRE_FALSE(graph.compiled());

  graph.prepare(48000.0, 8);
  REQUIRE_NOTHROW(graph.clear_inputs(8));
  REQUIRE_NOTHROW(graph.process_block(8));
}

#ifdef SONARE_WITH_GRAPH
TEST_CASE("GraphRuntime refuses to swap in an uncompiled graph", "[graph][noexcept]") {
  // The swap boundary must reject a non-compiled topology so it can never be
  // published to the audio thread.
  sonare::graph::Graph uncompiled;
  REQUIRE(uncompiled.add_node("in", pass(), 1));
  REQUIRE(uncompiled.add_node("out", pass(), 1));
  REQUIRE(uncompiled.connect({"in", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE_FALSE(uncompiled.compiled());

  sonare::engine::GraphRuntime runtime;
  REQUIRE_FALSE(runtime.bind(&uncompiled, "in", "out", 1));
  REQUIRE(runtime.swap(&uncompiled, "in", "out", 1) == nullptr);
  REQUIRE(runtime.active_graph() == nullptr);
}
#endif

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

TEST_CASE("Graph PDC delays are unchanged after the O(V+E) incoming-list pass", "[graph]") {
  // The plugin-delay-compensation longest-path pass now visits each node's
  // incoming edges via a prebuilt adjacency list (O(V+E)) instead of rescanning
  // every connection for every node (O(V*E)). The computed per-connection
  // delays must be byte-for-byte identical. This pins the delays for a graph
  // with multiple convergent paths of differing latency so any regression in
  // the longest-path computation is caught.
  sonare::graph::Graph graph;

  // Topology (latencies in samples):
  //   in --> a(10) --> sum
  //   in --> b(30) --> sum
  //   in -------------> sum   (direct, 0)
  // Longest path into sum is via b: 30. So:
  //   in->a delay = 30 - (0)        ... no, alignment is per *destination*.
  // Each connection's delay aligns its source-path latency up to the dest's
  // max incoming latency.
  REQUIRE(graph.add_node("in", pass(), 1));
  REQUIRE(graph.add_node("a", std::make_unique<FixedLatencyProcessor>(10), 1));
  REQUIRE(graph.add_node("b", std::make_unique<FixedLatencyProcessor>(30), 1));
  REQUIRE(graph.add_node("sum", pass(), 1));
  REQUIRE(graph.connect({"in", 0, "a", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"in", 0, "b", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"a", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"b", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"in", 0, "sum", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());

  // Connection order matches insertion order. Max incoming latency at sum is 30
  // (the a-path contributes 10, the b-path 30, the direct in-path 0).
  //   conn0 in->a : both endpoints at 0 incoming, source latency irrelevant -> 0
  //   conn1 in->b : 0
  //   conn2 a->sum: source path latency 10, align to 30 -> delay 20
  //   conn3 b->sum: source path latency 30, align to 30 -> delay 0
  //   conn4 in->sum: source path latency 0, align to 30 -> delay 30
  REQUIRE(graph.connection_delay_samples(0) == 0);
  REQUIRE(graph.connection_delay_samples(1) == 0);
  REQUIRE(graph.connection_delay_samples(2) == 20);
  REQUIRE(graph.connection_delay_samples(3) == 0);
  REQUIRE(graph.connection_delay_samples(4) == 30);

  // Recompiling must yield identical delays (idempotent, no stale adjacency).
  REQUIRE(graph.compile());
  REQUIRE(graph.connection_delay_samples(2) == 20);
  REQUIRE(graph.connection_delay_samples(4) == 30);
}

TEST_CASE("Graph PDC computes correctly on a larger chained-and-parallel graph", "[graph]") {
  // A larger graph exercises the O(V+E) pass over many nodes/edges and confirms
  // the longest-path alignment still holds end-to-end. A chain of latency nodes
  // feeds a sum alongside a zero-latency bypass; the bypass edge must be delayed
  // by the full accumulated chain latency so both arrive aligned.
  sonare::graph::Graph graph;

  REQUIRE(graph.add_node("src", pass(), 1));
  // Chain: src -> c0(5) -> c1(7) -> c2(11) -> sink
  REQUIRE(graph.add_node("c0", std::make_unique<FixedLatencyProcessor>(5), 1));
  REQUIRE(graph.add_node("c1", std::make_unique<FixedLatencyProcessor>(7), 1));
  REQUIRE(graph.add_node("c2", std::make_unique<FixedLatencyProcessor>(11), 1));
  REQUIRE(graph.add_node("sink", pass(), 1));

  REQUIRE(graph.connect({"src", 0, "c0", 0, sonare::graph::Connection::Mix::Add}));   // 0
  REQUIRE(graph.connect({"c0", 0, "c1", 0, sonare::graph::Connection::Mix::Add}));    // 1
  REQUIRE(graph.connect({"c1", 0, "c2", 0, sonare::graph::Connection::Mix::Add}));    // 2
  REQUIRE(graph.connect({"c2", 0, "sink", 0, sonare::graph::Connection::Mix::Add}));  // 3
  // Parallel bypass straight from src to sink (0 latency on this path).
  REQUIRE(graph.connect({"src", 0, "sink", 0, sonare::graph::Connection::Mix::Add}));  // 4

  REQUIRE(graph.compile());

  // Accumulated chain latency into sink along the c-path is 5+7+11 = 23. The
  // chain edge (conn3) already carries the full path latency, so it needs no
  // extra delay; the bypass edge (conn4) must be delayed by 23 to align.
  REQUIRE(graph.connection_delay_samples(3) == 0);
  REQUIRE(graph.connection_delay_samples(4) == 23);
  // Intermediate chain links are on the single longest path, so no padding.
  REQUIRE(graph.connection_delay_samples(0) == 0);
  REQUIRE(graph.connection_delay_samples(1) == 0);
  REQUIRE(graph.connection_delay_samples(2) == 0);

  // End-to-end: an impulse on src must emerge aligned at sink. The bypass copy
  // and the chained copy both land at sample 23 (summed to 2.0), proving the
  // compensation delays are applied consistently with the computed values.
  graph.prepare(48000.0, 32);
  std::array<float, 32> impulse{};
  impulse[0] = 1.0f;
  graph.clear_inputs(32);
  graph.set_input("src", 0, impulse.data(), 32);
  graph.process_block(32);

  const float* out = graph.output("sink", 0);
  REQUIRE(out != nullptr);
  for (int i = 0; i < 23; ++i) {
    REQUIRE_THAT(out[i], WithinAbs(0.0f, 0.0001f));
  }
  REQUIRE_THAT(out[23], WithinAbs(2.0f, 0.0001f));
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
