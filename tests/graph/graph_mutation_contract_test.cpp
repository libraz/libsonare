#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "graph/graph.h"
#include "rt/processor_base.h"

namespace {

class CountingProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit CountingProcessor(int* calls) : calls_(calls) {}

  void prepare(double, int) override {}
  void process(float* const*, int, int) override { ++*calls_; }
  void reset() override {}

 private:
  int* calls_;
};

}  // namespace

TEST_CASE("Graph preserves its compiled plan after a failed disconnect", "[graph][mutation]") {
  sonare::graph::Graph graph;
  int calls = 0;
  REQUIRE(graph.add_node("source", std::make_unique<CountingProcessor>(&calls), 1));
  REQUIRE(graph.add_node("destination", std::make_unique<CountingProcessor>(&calls), 1));
  REQUIRE(graph.connect({"source", 0, "destination", 0, sonare::graph::Connection::Mix::Add}));
  graph.prepare(48000.0, 8);
  REQUIRE(graph.compiled());

  REQUIRE_FALSE(graph.disconnect("source", 0, "missing", 0));
  REQUIRE(graph.compiled());

  graph.process_block(8);
  REQUIRE(calls == 2);

  REQUIRE(graph.disconnect("source", 0, "destination", 0));
  REQUIRE_FALSE(graph.compiled());
}

TEST_CASE("Graph invalidates only when sidechain ports actually change", "[graph][mutation]") {
  sonare::graph::Graph graph;
  int calls = 0;
  REQUIRE(graph.add_node("keyed", std::make_unique<CountingProcessor>(&calls), 2));
  graph.prepare(48000.0, 8);
  REQUIRE(graph.compiled());

  // A zero-port request always normalizes to the default {0, 0} pair.
  REQUIRE(graph.set_node_sidechain_ports("keyed", 99, 0));
  REQUIRE(graph.compiled());

  REQUIRE(graph.set_node_sidechain_ports("keyed", 1, 1));
  REQUIRE_FALSE(graph.compiled());

  REQUIRE(graph.compile());
  REQUIRE(graph.compiled());
  REQUIRE(graph.set_node_sidechain_ports("keyed", 1, 1));
  REQUIRE(graph.compiled());

  REQUIRE(graph.compile());
  REQUIRE(graph.set_node_sidechain_ports("keyed", -5, 0));
  REQUIRE_FALSE(graph.compiled());
}
