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
