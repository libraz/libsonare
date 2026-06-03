#include "engine/graph_runtime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "rt/processor_base.h"

namespace {

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

class SidechainProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    process_channels = num_channels;
    process_samples = num_samples;
    if (sidechain_ == nullptr || num_channels <= 0) return;
    for (int i = 0; i < num_samples; ++i) {
      channels[0][i] += sidechain_[0][i] * 0.5f;
    }
  }
  void reset() override {}
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override {
    sidechain_channels = num_channels;
    sidechain_samples = num_samples;
    sidechain_ = channels;
  }

  int process_channels = 0;
  int process_samples = 0;
  int sidechain_channels = 0;
  int sidechain_samples = 0;

 private:
  const float* const* sidechain_ = nullptr;
};

}  // namespace

TEST_CASE("GraphRuntime processes a prepared graph sub-block without string routing",
          "[engine][graph_runtime]") {
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 2));
  REQUIRE(graph.add_node("gain", std::make_unique<GainProcessor>(2.0f), 2));
  REQUIRE(graph.add_node("out", std::make_unique<GainProcessor>(1.0f), 2));
  REQUIRE(graph.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"in", 1, "gain", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 1, "out", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 8);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 2));

  std::array<float, 8> left{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  std::array<float, 8> right{-1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f};
  float* io[] = {left.data(), right.data()};
  runtime.process(io, 2, 2, 4);

  REQUIRE(left[0] == 1.0f);
  REQUIRE(left[1] == 2.0f);
  REQUIRE(left[2] == 6.0f);
  REQUIRE(left[5] == 12.0f);
  REQUIRE(left[6] == 7.0f);
  REQUIRE(right[2] == -6.0f);
  REQUIRE(right[5] == -12.0f);
}

TEST_CASE("GraphRuntime bypasses processor nodes as dry pass-through", "[engine][graph_runtime]") {
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.add_node("gain", std::make_unique<GainProcessor>(4.0f), 1));
  REQUIRE(graph.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 8);
  REQUIRE(graph.node("gain") != nullptr);
  REQUIRE(graph.node("gain")->processor().set_bypassed(true));

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 1));

  std::array<float, 4> buffer{1.0f, 2.0f, 3.0f, 4.0f};
  float* io[] = {buffer.data()};
  runtime.process(io, 1, 0, 4);
  REQUIRE(buffer[0] == 1.0f);
  REQUIRE(buffer[1] == 2.0f);
  REQUIRE(buffer[3] == 4.0f);
}

TEST_CASE("Graph node maps configured sidechain ports to ProcessorBase sidechain",
          "[engine][graph_runtime]") {
  sonare::graph::Graph graph;
  auto sidechain = std::make_unique<SidechainProcessor>();
  SidechainProcessor* raw_sidechain = sidechain.get();
  REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.add_node("key", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.add_node("fx", std::move(sidechain), 2));
  REQUIRE(graph.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.set_node_sidechain_ports("fx", 1, 1));
  REQUIRE(graph.connect({"in", 0, "fx", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"key", 0, "fx", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"fx", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 8);

  std::array<float, 4> main{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> key{10.0f, 20.0f, 30.0f, 40.0f};
  graph.clear_inputs(4);
  graph.set_input("in", 0, main.data(), 4);
  graph.set_input("key", 0, key.data(), 4);
  graph.process_block(4);

  const float* out = graph.output("out", 0);
  REQUIRE(out != nullptr);
  REQUIRE(out[0] == 6.0f);
  REQUIRE(out[1] == 12.0f);
  REQUIRE(out[3] == 24.0f);
  REQUIRE(raw_sidechain->process_channels == 1);
  REQUIRE(raw_sidechain->process_samples == 4);
  REQUIRE(raw_sidechain->sidechain_channels == 1);
  REQUIRE(raw_sidechain->sidechain_samples == 4);
}

TEST_CASE("GraphRuntime swap returns old graph for control-thread reclamation",
          "[engine][graph_runtime]") {
  sonare::graph::Graph first;
  REQUIRE(first.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(first.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(first.connect({"in", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(first.compile());
  first.prepare(48000.0, 8);

  sonare::graph::Graph second;
  REQUIRE(second.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(second.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(second.connect({"in", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(second.compile());
  second.prepare(48000.0, 8);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&first, "in", "out", 1));
  REQUIRE(runtime.swap(&second, "in", "out", 1) == &first);
  REQUIRE(runtime.active_graph() == &second);
}

TEST_CASE("GraphRuntime hot-swap changes the audible processing", "[engine][graph_runtime]") {
  // First graph applies a 2x gain; the second applies an 8x gain. After
  // swapping, the rendered output must reflect the NEW graph's gain, not just
  // report a new active_graph() pointer.
  sonare::graph::Graph first;
  REQUIRE(first.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(first.add_node("gain", std::make_unique<GainProcessor>(2.0f), 1));
  REQUIRE(first.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(first.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(first.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(first.compile());
  first.prepare(48000.0, 8);

  sonare::graph::Graph second;
  REQUIRE(second.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(second.add_node("gain", std::make_unique<GainProcessor>(8.0f), 1));
  REQUIRE(second.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(second.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(second.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(second.compile());
  second.prepare(48000.0, 8);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&first, "in", "out", 1));

  std::array<float, 4> buffer{1.0f, 2.0f, 3.0f, 4.0f};
  float* io[] = {buffer.data()};
  runtime.process(io, 1, 0, 4);
  REQUIRE(buffer[0] == 2.0f);
  REQUIRE(buffer[3] == 8.0f);

  // Swap to the 8x graph; the audio-thread process() adopts it on acquire().
  REQUIRE(runtime.swap(&second, "in", "out", 1) == &first);

  std::array<float, 4> buffer2{1.0f, 2.0f, 3.0f, 4.0f};
  float* io2[] = {buffer2.data()};
  runtime.process(io2, 1, 0, 4);
  REQUIRE(buffer2[0] == 8.0f);
  REQUIRE(buffer2[1] == 16.0f);
  REQUIRE(buffer2[3] == 32.0f);
}
