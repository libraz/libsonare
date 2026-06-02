#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "automation/automation_engine.h"
#include "engine/capture.h"
#include "engine/clip_player.h"
#ifdef SONARE_WITH_GRAPH
#include "engine/graph_runtime.h"
#include "graph/graph.h"
#endif
#include "engine/meter_telemetry.h"
#include "engine/metronome.h"
#include "engine/mixing_runtime.h"
#include "engine/monitor_runtime.h"
#include "engine/realtime_engine.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/spectrum_registry.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mixing/bus.h"
#include "mixing/channel_strip.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<size_t> g_allocation_count{0};

void note_allocation() noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void* allocate_bytes(std::size_t size) {
  note_allocation();
  if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void* allocate_aligned_bytes(std::size_t size, std::size_t alignment) {
  note_allocation();
  void* ptr = nullptr;
  const std::size_t actual_size = size == 0 ? 1 : size;
#if defined(_WIN32)
  ptr = _aligned_malloc(actual_size, alignment);
  if (ptr != nullptr) {
    return ptr;
  }
#else
  if (posix_memalign(&ptr, alignment, actual_size) != 0) {
    ptr = nullptr;
  }
  if (ptr != nullptr) {
    return ptr;
  }
#endif
  throw std::bad_alloc();
}

// Aligned allocations must be released with the matching deallocator: on Windows
// _aligned_malloc memory requires _aligned_free, whereas posix_memalign memory is
// freed with std::free.
void aligned_free(void* ptr) noexcept {
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

class AllocationGuard {
 public:
  AllocationGuard() {
    g_allocation_count.store(0, std::memory_order_relaxed);
    g_count_allocations.store(true, std::memory_order_relaxed);
  }
  ~AllocationGuard() { g_count_allocations.store(false, std::memory_order_relaxed); }
  size_t count() const noexcept { return g_allocation_count.load(std::memory_order_relaxed); }
};

class ScaleProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit ScaleProcessor(float scale) : scale_(scale) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= scale_;
      }
    }
  }
  void reset() override {}

 private:
  float scale_ = 1.0f;
};

class ParameterCaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    last_param = param_id;
    last_value = value;
    return true;
  }

  unsigned int last_param = 0;
  float last_value = 0.0f;
};

}  // namespace

void* operator new(std::size_t size) { return allocate_bytes(size); }
void* operator new[](std::size_t size) { return allocate_bytes(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }

TEST_CASE("ChannelStrip process performs no heap allocation after prepare", "[mixing][rt]") {
  constexpr int kBlock = 256;
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(
      sonare::mastering::dynamics::CompressorConfig{}));
  strip.prepare(48000.0, kBlock);
  strip.set_polarity_invert(true, false);
  strip.set_channel_delay_samples(3);
  strip.set_input_trim_db(1.5f);
  strip.set_fader_db(-3.0f);
  strip.set_pan(0.2f);
  strip.set_width(1.25f);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.05f;
    right[static_cast<size_t>(i)] = 0.03f;
  }
  float* channels[] = {left.data(), right.data()};

  strip.process(channels, 2, kBlock);
  strip.reset();

  AllocationGuard guard;
  strip.process(channels, 2, kBlock);
  const size_t allocations = guard.count();

  REQUIRE(allocations == 0);
}

TEST_CASE("RealtimeEngine process performs no heap allocation after prepare", "[engine][rt]") {
  constexpr int kBlock = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  engine.process(channels, 2, kBlock);

  AllocationGuard guard;
  engine.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MeterTelemetryTap process performs no heap allocation after prepare",
          "[engine][meter_telemetry][rt]") {
  constexpr int kBlock = 128;
  sonare::engine::MeterTelemetryTap tap;
  tap.prepare(48000.0, kBlock, 3, 16);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.5f);
  right.fill(0.25f);
  float* channels[] = {left.data(), right.data()};

  tap.process(channels, 2, kBlock, 0);

  AllocationGuard guard;
  tap.process(channels, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("AutomationEngine apply performs no heap allocation after prepare", "[automation][rt]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::automation::AutomationLane lane(7);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {1.0, 1.0f, sonare::automation::CurveType::Linear}});

  ParameterCaptureProcessor processor;
  sonare::automation::AutomationEngine engine;
  engine.prepare(48000.0, &tempo);
  engine.set_lanes({lane});
  engine.acquire_lanes();
  engine.bind_target(7, &processor);

  sonare::transport::TransportState state{};
  state.sample_position = 0;
  engine.apply(state, 12000, 64);

  AllocationGuard guard;
  engine.apply(state, 12000, 64);
  REQUIRE(guard.count() == 0);
  REQUIRE(processor.last_param == 7);
  REQUIRE(processor.last_value > 0.49f);
}

TEST_CASE("MixingRuntime process performs no heap allocation after prepare",
          "[engine][mixing][rt]") {
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  sonare::engine::MixingRuntime runtime;
  REQUIRE(runtime.bind(&strip));
  runtime.prepare(48000.0, kBlock);
  REQUIRE(runtime.set_parameter(sonare::engine::MixingRuntime::kFaderDb, -3.0f));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};

  runtime.process_at(channels, 2, kBlock, 0);

  AllocationGuard guard;
  runtime.process_at(channels, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MonitorRuntime process performs no heap allocation after prepare",
          "[engine][monitor][rt]") {
  constexpr int kBlock = 128;
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kBlock);

  sonare::engine::MonitorRuntime runtime;
  runtime.prepare(48000.0, kBlock, 5.0f);
  REQUIRE(runtime.add_strip(&strip));
  runtime.set_monitor_mode(0, sonare::engine::MonitorMode::kAfl);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  std::array<float, kBlock> mon_l{};
  std::array<float, kBlock> mon_r{};
  left.fill(1.0f);
  right.fill(1.0f);
  float* channels[] = {left.data(), right.data()};
  float* monitor[] = {mon_l.data(), mon_r.data()};

  runtime.process_strip(0, channels, 2, kBlock, 0, monitor);

  AllocationGuard guard;
  runtime.process_strip(0, channels, 2, kBlock, kBlock, monitor);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("ClipPlayer process performs no heap allocation after prepare", "[engine][clip][rt]") {
  constexpr int kBlock = 128;
  std::array<float, kBlock> source_l{};
  std::array<float, kBlock> source_r{};
  source_l.fill(0.5f);
  source_r.fill(-0.5f);
  const float* source[] = {source_l.data(), source_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, kBlock);
  // Looping clip several blocks long so the steady-state (mid-playback) path is
  // exercised when the timeline advances past the first block.
  player.set_clips({{1, {source, 2, kBlock}, 0.0, 0, 0, kBlock * 4, true, 1.0f, 4, 4}});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* out[] = {left.data(), right.data()};
  player.process_at(out, 2, kBlock, 0);

  AllocationGuard guard;
  // Advance the timeline so the second render is steady-state mid-playback
  // (past the fade-in, across a loop wrap), not a replay from position 0.
  player.process_at(out, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("CaptureSink process performs no heap allocation after prepare",
          "[engine][capture][rt]") {
  constexpr int kBlock = 128;
  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  in_l.fill(0.25f);
  in_r.fill(-0.25f);
  const float* input[] = {in_l.data(), in_r.data()};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  float* capture[] = {out_l.data(), out_r.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 2, kBlock});
  sink.arm(true);
  sink.process(input, 2, kBlock, 0);

  AllocationGuard guard;
  sink.process(input, 2, kBlock, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Metronome process performs no heap allocation after prepare",
          "[engine][metronome][rt]") {
  constexpr int kBlock = 256;
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, 16});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  metro.process(channels, 2, kBlock, 0);

  AllocationGuard guard;
  metro.process(channels, 2, kBlock, 24000);
  REQUIRE(guard.count() == 0);
}

#ifdef SONARE_WITH_GRAPH
TEST_CASE("GraphRuntime process performs no heap allocation after prepare", "[engine][graph][rt]") {
  constexpr int kBlock = 128;
  sonare::graph::Graph graph;
  REQUIRE(graph.add_node("in", std::make_unique<ScaleProcessor>(1.0f), 2));
  REQUIRE(graph.add_node("gain", std::make_unique<ScaleProcessor>(0.5f), 2));
  REQUIRE(graph.add_node("out", std::make_unique<ScaleProcessor>(1.0f), 2));
  REQUIRE(graph.connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"in", 1, "gain", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.connect({"gain", 1, "out", 1, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph.compile());
  graph.prepare(48000.0, kBlock);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(&graph, "in", "out", 2));

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(1.0f);
  right.fill(-1.0f);
  float* channels[] = {left.data(), right.data()};

  runtime.process(channels, 2, 0, kBlock);

  AllocationGuard guard;
  runtime.process(channels, 2, 0, kBlock);
  REQUIRE(guard.count() == 0);
}
#endif

TEST_CASE("BusProcessor post-sum inserts perform no heap allocation after prepare",
          "[mixing][rt]") {
  constexpr int kBlock = 256;
  sonare::mixing::BusProcessor bus;
  bus.add_insert(std::make_unique<ScaleProcessor>(0.5f));
  bus.prepare(48000.0, kBlock);

  std::array<float, kBlock> in_l{};
  std::array<float, kBlock> in_r{};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  in_l.fill(1.0f);
  in_r.fill(1.0f);
  float* input[] = {in_l.data(), in_r.data()};
  float* output[] = {out_l.data(), out_r.data()};
  const std::vector<float* const*> inputs{input};

  bus.sum_inputs(inputs, output, 2, kBlock);
  bus.process(output, 2, kBlock);

  AllocationGuard guard;
  bus.sum_inputs(inputs, output, 2, kBlock);
  bus.process(output, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("TruePeakLimiter process performs no heap allocation after prepare",
          "[mastering][maximizer][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::maximizer::TruePeakLimiter limiter({-1.0f, 1.0f, 20.0f, 4});
  limiter.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.2f : 0.25f;
    right[static_cast<size_t>(i)] = i == 1 ? -1.1f : -0.2f;
  }
  float* channels[] = {left.data(), right.data()};

  limiter.process(channels, 2, kBlock);
  limiter.reset();

  AllocationGuard guard;
  limiter.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandExciter process performs no heap allocation after prepare",
          "[mastering][saturation][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::saturation::MultibandExciter exciter;
  exciter.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};

  exciter.process(channels, 2, kBlock);
  exciter.reset();

  AllocationGuard guard;
  exciter.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("PultecEq process performs no heap allocation after prepare", "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::PultecEq eq;
  // Engage the WDF component model so the per-channel component state path runs.
  eq.set_component_model(sonare::mastering::eq::PultecComponentModel::Eqp1aWdf);
  eq.set_output_drive(2.0f);
  eq.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.6f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};

  eq.process(channels, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandSaturation process performs no heap allocation after prepare",
          "[mastering][saturation][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::multiband::MultibandSaturation sat;
  sat.prepare(48000.0, kBlock);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};
  sat.process(channels, 2, kBlock);
  sat.reset();
  AllocationGuard guard;
  sat.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 8000.0f, -2.0f, 0.8f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard stereo_guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(stereo_guard.count() == 0);

  eq.reset();
  float* mono[] = {left.data()};
  AllocationGuard mono_guard;
  eq.process(mono, 1, kBlock);
  REQUIRE(mono_guard.count() == 0);
}

TEST_CASE("EqualizerProcessor Mid/Side placement performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand mid{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f, 1.0f,
                                    true};
  mid.placement = sonare::mastering::eq::StereoPlacement::Mid;
  sonare::mastering::eq::EqBand side{sonare::mastering::eq::EqBandType::Peak, 3000.0f, -2.0f, 1.2f,
                                     true};
  side.placement = sonare::mastering::eq::StereoPlacement::Side;
  eq.set_band(0, mid);
  eq.set_band(1, side);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor dynamic bands perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 0.0f, 2.0f,
                                     true};
  band.dyn.enabled = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(23, band);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.5f;
    right[static_cast<size_t>(i)] = 0.5f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor external sidechain performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 0.0f, 2.0f,
                                     true};
  band.dyn.enabled = true;
  band.dyn.external_sidechain = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(0, band);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  std::array<float, kBlock> key{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.02f;
    right[static_cast<size_t>(i)] = 0.02f;
    key[static_cast<size_t>(i)] = 0.5f;
  }
  float* stereo[] = {left.data(), right.data()};
  const float* sidechain[] = {key.data()};

  eq.set_sidechain(sidechain, 1, kBlock);
  eq.process(stereo, 2, kBlock);
  eq.reset();

  eq.set_sidechain(sidechain, 1, kBlock);
  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor LinearPhase bands perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand linear{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 4.0f, 1.0f,
                                       true};
  linear.phase = sonare::mastering::eq::PhaseMode::LinearPhase;
  eq.set_band(0, linear);
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 8000.0f, -2.0f, 0.8f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor E6 features perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  eq.set_auto_gain_enabled(true);
  sonare::mastering::eq::EqBand tilt{sonare::mastering::eq::EqBandType::TiltShelf, 1000.0f, 6.0f,
                                     1.0f, true};
  sonare::mastering::eq::EqBand solo{sonare::mastering::eq::EqBandType::Peak, 2500.0f, 9.0f, 3.0f,
                                     true};
  solo.soloed = true;
  solo.proportional_q = true;
  eq.set_band(0, tilt);
  eq.set_band(1, solo);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("SpectrumRegistry publish read and collisions perform no heap allocation",
          "[mastering][eq][rt]") {
  auto& registry = sonare::mastering::eq::SpectrumRegistry::instance();
  registry.reset();

  sonare::mastering::eq::SpectrumProfile first;
  first.instance_id = 9001;
  first.active = true;
  first.seq = 1;
  first.band_db.fill(-120.0f);
  first.band_db[4] = -18.0f;

  sonare::mastering::eq::SpectrumProfile second;
  second.instance_id = 9002;
  second.active = true;
  second.seq = 1;
  second.band_db.fill(-120.0f);
  second.band_db[5] = -16.0f;

  registry.publish(first);
  registry.publish(second);

  AllocationGuard guard;
  registry.publish(first);
  sonare::mastering::eq::SpectrumProfile out;
  REQUIRE(registry.read(9001, out));
  const auto report = registry.collisions(9001, 9002, -60.0f);
  registry.remove(9001);
  REQUIRE(report.count == 1);
  REQUIRE(guard.count() == 0);

  registry.reset();
}

TEST_CASE("EqualizerProcessor spectrum publishing performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  auto& registry = sonare::mastering::eq::SpectrumRegistry::instance();
  registry.reset();

  sonare::mastering::eq::EqualizerProcessor eq({2, 7007});
  eq.prepare(48000.0, kBlock);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  sonare::mastering::eq::SpectrumProfile profile;
  REQUIRE(registry.read(7007, profile));
  REQUIRE(profile.active);
  REQUIRE(guard.count() == 0);

  registry.reset();
}

TEST_CASE("CutFilter process performs no heap allocation after prepare", "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::CutFilter eq;
  eq.prepare(48000.0, kBlock);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ,
                   sonare::mastering::eq::CutFilterSlope::Db96PerOct);
  eq.set_low_pass(12000.0f, sonare::constants::kButterworthQ,
                  sonare::mastering::eq::CutFilterSlope::Db96PerOct);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("CutFilter brickwall process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::CutFilter eq;
  eq.prepare(48000.0, kBlock);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ,
                   sonare::mastering::eq::CutFilterSlope::Brickwall);
  eq.set_low_pass(12000.0f, sonare::constants::kButterworthQ,
                  sonare::mastering::eq::CutFilterSlope::Brickwall);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MinimumPhaseEq process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::MinimumPhaseEq eq;
  eq.prepare(48000.0, kBlock);
  eq.prepare_channels(2);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 12000.0f, 4.0f, 0.8f, true});
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 9000.0f, -3.0f, 1.0f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Gate channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Gate gate({-50.0f, 2.0f, 80.0f, -80.0f, 0.0f, -50.0f, 120.0f});
  gate.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  gate.process(mono, 1, kBlock);
  AllocationGuard guard;
  gate.process(stereo, 2, kBlock);
  gate.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("TransientShaper channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::TransientShaper shaper;
  shaper.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.0f;
    right[static_cast<size_t>(i)] = i == 8 ? 0.5f : 0.0f;
  }
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  shaper.process(mono, 1, kBlock);
  AllocationGuard guard;
  shaper.process(stereo, 2, kBlock);
  shaper.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("DeEsser channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::DeEsser deesser;
  deesser.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  deesser.process(mono, 1, kBlock);
  AllocationGuard guard;
  deesser.process(stereo, 2, kBlock);
  deesser.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Compressor channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Compressor compressor;
  compressor.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  compressor.process(mono, 1, kBlock);
  AllocationGuard guard;
  compressor.process(stereo, 2, kBlock);
  compressor.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Expander channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Expander expander;
  expander.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  expander.process(mono, 1, kBlock);
  AllocationGuard guard;
  expander.process(stereo, 2, kBlock);
  expander.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("UpwardCompressor channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::UpwardCompressor compressor;
  compressor.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  compressor.process(mono, 1, kBlock);
  AllocationGuard guard;
  compressor.process(stereo, 2, kBlock);
  compressor.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("UpwardExpander channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::UpwardExpander expander;
  expander.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  expander.process(mono, 1, kBlock);
  AllocationGuard guard;
  expander.process(stereo, 2, kBlock);
  expander.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

// ============================================================================
// P0-D regression tests: ChannelStrip insert/automation cap enforcement
// ============================================================================

TEST_CASE("ChannelStrip schedule_insert_automation enforces kMaxInsertAutomationLanes cap",
          "[mixing][rt-safety]") {
  // Regression guard for P0-D: insert_automation_ is reserved in the
  // constructor for kMaxInsertAutomationLanes entries so that control-thread
  // push_back never triggers a reallocation that would race with the audio
  // thread iterating the same vector.  This test exhausts the cap via the
  // public control-thread API and confirms that:
  //   (a) all lanes up to the cap succeed (return true), and
  //   (b) the N+1th lane returns false instead of reallocating.
  //
  // schedule_insert_automation requires an actual insert at the given
  // insert_index.  We add one ScaleProcessor so index 0 resolves.
  // Each distinct param_id maps to an independent AutomationTarget, creating
  // a new InsertAutomationLane entry (the de-dup path only merges identical
  // (insert_index, param_id) pairs).
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f));

  const size_t cap = sonare::mixing::ChannelStrip::kMaxInsertAutomationLanes;
  size_t success_count = 0;
  for (size_t i = 0; i < cap; ++i) {
    const unsigned int param_id = static_cast<unsigned int>(i);
    const bool ok = strip.schedule_insert_automation(0, param_id, 0, 0.5f);
    if (ok) {
      ++success_count;
    }
  }
  REQUIRE(success_count == cap);

  // The cap+1th call must be rejected (return false).
  const bool overflow =
      strip.schedule_insert_automation(0, static_cast<unsigned int>(cap), 0, 0.5f);
  REQUIRE_FALSE(overflow);
}

TEST_CASE("ChannelStrip add_insert enforces kMaxInserts cap", "[mixing][rt-safety]") {
  // Regression guard for P0-D: pre_inserts_ and post_inserts_ are reserved
  // to kMaxInserts in the constructor.  add_pre_insert / add_post_insert
  // must throw SonareException(InvalidState) on the N+1th insert rather than triggering
  // a push_back that reallocates behind the audio thread.
  sonare::mixing::ChannelStrip strip;

  const size_t cap = sonare::mixing::ChannelStrip::kMaxInserts;

  // Fill the strip with ScaleProcessor inserts split evenly across pre/post
  // to reach exactly kMaxInserts total (all pre for simplicity).
  for (size_t i = 0; i < cap; ++i) {
    REQUIRE_NOTHROW(strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f)));
  }
  REQUIRE(strip.num_pre_inserts() + strip.num_post_inserts() == cap);

  // The N+1th insert (pre or post) must throw.
  REQUIRE_THROWS_AS(strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f)),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(strip.add_post_insert(std::make_unique<ScaleProcessor>(1.0f)),
                    sonare::SonareException);
}

#ifdef SONARE_WITH_VOICE_CHANGER
#include "editing/voice_changer/realtime_voice_changer.h"

TEST_CASE("RealtimeVoiceChanger process_block performs no heap allocation after prepare",
          "[voice_changer][rt]") {
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  sonare::editing::voice_changer::RealtimeVoiceChanger changer(
      sonare::editing::voice_changer::realtime_voice_changer_preset(
          sonare::editing::voice_changer::VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(kSampleRate, kBlock, 1);

  std::array<float, kBlock> input{};
  std::array<float, kBlock> output{};
  input.fill(0.01f);

  // Warm up: drive one block so any lazy first-block work (initial snapshot
  // adoption, derived-state computation) completes before we start counting.
  changer.process_block(input.data(), output.data(), kBlock);

  AllocationGuard guard;
  changer.process_block(input.data(), output.data(), kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("RealtimeVoiceChanger planar process_block performs no heap allocation after prepare",
          "[voice_changer][rt]") {
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  sonare::editing::voice_changer::RealtimeVoiceChanger changer(
      sonare::editing::voice_changer::realtime_voice_changer_preset(
          sonare::editing::voice_changer::VoiceCharacterPreset::BrightIdol));
  changer.prepare(kSampleRate, kBlock, 2);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(-0.05f);
  float* channels[] = {left.data(), right.data()};

  // Warm up block.
  changer.process_block(channels, 2, kBlock);

  AllocationGuard guard;
  changer.process_block(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}
#endif  // SONARE_WITH_VOICE_CHANGER
