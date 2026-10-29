#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#ifdef SONARE_WITH_GRAPH
#include <memory>
#endif

#include "engine/realtime_engine.h"
#ifdef SONARE_WITH_GRAPH
#include "graph/graph.h"
#include "rt/processor_base.h"
#endif
#include "util/constants.h"

#ifdef SONARE_WITH_GRAPH
namespace {

class SampleRateProbe final : public sonare::rt::ProcessorBase {
 public:
  explicit SampleRateProbe(double* prepared_rate) : prepared_rate_(prepared_rate) {}

  void prepare(double sample_rate, int) override { *prepared_rate_ = sample_rate; }
  void process(float* const*, int, int) override {}
  void reset() override {}

 private:
  double* prepared_rate_;
};

}  // namespace
#endif

TEST_CASE("RealtimeEngine uses the finite default for invalid sample rates",
          "[engine][realtime][validation]") {
  const double default_rate = sonare::constants::kDefaultDawSampleRate;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();

  for (const double requested_rate : {nan, infinity, -1.0}) {
    CAPTURE(requested_rate);
    sonare::engine::RealtimeEngine engine;
    engine.prepare(requested_rate, 8);
    REQUIRE(engine.sample_rate() == default_rate);
  }

  sonare::engine::RealtimeEngine engine;
  engine.prepare(44100.0, 8);
  REQUIRE(engine.sample_rate() == 44100.0);
}

#ifdef SONARE_WITH_GRAPH
TEST_CASE("Graph prepares processors at a finite fallback sample rate", "[graph][validation]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const double default_rate = sonare::constants::kDefaultDawSampleRate;

  for (const double requested_rate : {nan, infinity, -1.0, 44100.0}) {
    CAPTURE(requested_rate);
    const double expected_rate =
        requested_rate > 0.0 && std::isfinite(requested_rate) ? requested_rate : default_rate;
    double processor_rate = 0.0;
    sonare::graph::Graph graph;
    REQUIRE(graph.add_node("probe", std::make_unique<SampleRateProbe>(&processor_rate), 1));
    graph.prepare(requested_rate, 8);
    REQUIRE(processor_rate == expected_rate);
  }
}
#endif
