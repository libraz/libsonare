#include "engine/graph_runtime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "rt/delay_line.h"
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

class LatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit LatencyProcessor(int latency_q8) : latency_q8_(latency_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples_q8() const noexcept override { return latency_q8_; }

 private:
  int latency_q8_ = 0;
};

// Reports a latency AND actually produces it, unlike LatencyProcessor above
// (which only reports one). A node's bypass compensation can only be checked
// against a processor whose reported and delivered latency agree.
class DelayLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit DelayLatencyProcessor(int latency) : latency_(latency) {}
  void prepare(double, int) override { delay_.prepare(static_cast<size_t>(latency_)); }
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (num_channels <= 0 || channels[0] == nullptr) return;
    for (int i = 0; i < num_samples; ++i) {
      channels[0][i] = delay_.process(channels[0][i]);
    }
  }
  void reset() override { delay_.reset(); }
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  sonare::rt::DelayLine delay_;
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

TEST_CASE("GraphRuntime keeps a bypassed node's latency in the signal path",
          "[engine][graph_runtime]") {
  // compile() bakes each node's per-port latency into the downstream connection
  // delays and never revisits it, so a node that stopped delaying when bypassed
  // would slide forward against every path it was aligned with. The node is a
  // pure delay, so bypassing must leave the rendered block untouched.
  constexpr int kLatency = 3;
  constexpr int kFrames = 8;
  auto render = [kLatency](bool bypassed) {
    sonare::graph::Graph graph;
    REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
    REQUIRE(graph.add_node("latent", std::make_unique<DelayLatencyProcessor>(kLatency), 1));
    REQUIRE(graph.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
    REQUIRE(graph.connect({"in", 0, "latent", 0, sonare::graph::Connection::Mix::Add}));
    REQUIRE(graph.connect({"latent", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
    REQUIRE(graph.compile());
    graph.prepare(48000.0, kFrames);
    REQUIRE(graph.node("latent") != nullptr);
    REQUIRE(graph.node("latent")->processor().set_bypassed(bypassed));
    // The reported latency is the host's PDC input and never consults bypass.
    REQUIRE(graph.node_latency_samples_q8("out") == (kLatency << 8));

    sonare::engine::GraphRuntime runtime;
    REQUIRE(runtime.bind(&graph, "in", "out", 1));
    std::array<float, kFrames> buffer{};
    buffer[0] = 1.0f;
    float* io[] = {buffer.data()};
    runtime.process(io, 1, 0, kFrames);
    return buffer;
  };

  const std::array<float, kFrames> active = render(false);
  const std::array<float, kFrames> bypassed = render(true);
  for (int i = 0; i < kLatency; ++i) {
    REQUIRE(active[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(bypassed[static_cast<size_t>(i)] == 0.0f);
  }
  REQUIRE(active[kLatency] == 1.0f);
  REQUIRE(bypassed[kLatency] == 1.0f);
}

TEST_CASE("GraphRuntime bypass engages without a dropout", "[engine][graph_runtime]") {
  // The substitute delay is fed the node's input on every active block, so
  // engaging bypass mid-stream switches to a warm delay line. A cold one would
  // punch a latency-length hole into the ramp at the toggle.
  constexpr int kLatency = 3;
  constexpr int kBlock = 8;
  constexpr int kBlocks = 4;

  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.add_node("latent", std::make_unique<DelayLatencyProcessor>(kLatency), 1));
  REQUIRE(graph.add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.connect({"in", 0, "latent", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"latent", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, kBlock);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 1));

  std::vector<float> rendered;
  for (int block = 0; block < kBlocks; ++block) {
    REQUIRE(graph.node("latent")->processor().set_bypassed(block >= 2));
    std::array<float, kBlock> buffer{};
    for (int i = 0; i < kBlock; ++i) {
      buffer[static_cast<size_t>(i)] = static_cast<float>(block * kBlock + i + 1);
    }
    float* io[] = {buffer.data()};
    runtime.process(io, 1, 0, kBlock);
    rendered.insert(rendered.end(), buffer.begin(), buffer.end());
  }

  for (int n = 0; n < kBlock * kBlocks; ++n) {
    const float expected = n < kLatency ? 0.0f : static_cast<float>(n - kLatency + 1);
    INFO("sample " << n);
    REQUIRE(rendered[static_cast<size_t>(n)] == expected);
  }
}

TEST_CASE("GraphRuntime reports the selected output path's Q8 latency", "[engine][graph_runtime]") {
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph.add_node("latent", std::make_unique<LatencyProcessor>(7 << 8), 1));
  REQUIRE(graph.add_node("out", std::make_unique<LatencyProcessor>(5 << 8), 1));
  REQUIRE(graph.connect({"in", 0, "latent", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"latent", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, 8);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 1));
  // Graph stores the 7-sample delay at out's input; GraphRuntime must include
  // out's own 5-sample processor delay when reporting the rendered path.
  REQUIRE(runtime.latency_samples_q8() == (12 << 8));
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
