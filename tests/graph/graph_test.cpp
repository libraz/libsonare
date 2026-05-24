#include "graph/graph.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <memory>
#include <vector>

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
