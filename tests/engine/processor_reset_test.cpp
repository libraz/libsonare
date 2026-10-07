/// @file processor_reset_test.cpp
/// @brief Mixer and effect processor reset: prime, bounce and the queued reset start from the
/// prepared state.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "c_api/sonare_c_engine_internal.h"
#include "mastering/api/insert_factory.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPi;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr int kChannels = 2;
constexpr int64_t kClipFrames = 96000;
constexpr uint32_t kTrackId = 10;
constexpr uint32_t kBusId = 1;
constexpr int kDirtyBlocks = 100;
constexpr int64_t kStartSample = 24000;
constexpr int64_t kRenderFrames = 9600;

enum class Processor { kPlate, kGate, kTape, kDelay };
enum class Placement { kLane, kBus, kMaster };
enum class Entry { kBounceRepeat, kFreeze, kQueuedResetPlay };

struct Row {
  Processor processor;
  Placement placement;
  int idle_blocks;
  Entry entry;
};

const char* processor_name(Processor p) {
  switch (p) {
    case Processor::kPlate:
      return "plate";
    case Processor::kGate:
      return "gate";
    case Processor::kTape:
      return "tape";
    case Processor::kDelay:
      return "delay";
  }
  return "?";
}

const char* placement_name(Placement p) {
  switch (p) {
    case Placement::kLane:
      return "lane";
    case Placement::kBus:
      return "bus";
    case Placement::kMaster:
      return "master";
  }
  return "?";
}

const char* entry_name(Entry e) {
  switch (e) {
    case Entry::kBounceRepeat:
      return "bounce_repeat";
    case Entry::kFreeze:
      return "freeze";
    case Entry::kQueuedResetPlay:
      return "queued_reset_play";
  }
  return "?";
}

std::string insert_json(Processor p) {
  switch (p) {
    case Processor::kPlate:
      return R"({"slot":"pre","processor":"effects.reverb.plate","params":)"
             R"({"decaySec":2.0,"modRateHz":1.0,"modDepthSamples":16,"dryWet":0.5}})";
    case Processor::kGate:
      return R"({"slot":"pre","processor":"dynamics.gate","params":)"
             R"({"thresholdDb":-30,"attackMs":1,"holdMs":5,"releaseMs":80}})";
    case Processor::kTape:
      return R"({"slot":"pre","processor":"saturation.tape","params":{"bias":0.5,"driveDb":6}})";
    case Processor::kDelay:
      return R"({"slot":"pre","processor":"effects.delay.stereo","params":)"
             R"({"feedback":0.6,"delayTimeLMs":37,"delayTimeRMs":53,"dryWet":0.5}})";
  }
  return "";
}

// Tone bursts with silent gaps, so a gate opens and closes and every tail is excited.
struct Source {
  std::vector<float> left;
  std::vector<float> right;
  Source() : left(static_cast<size_t>(kClipFrames)), right(static_cast<size_t>(kClipFrames)) {
    for (int64_t i = 0; i < kClipFrames; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
      const bool on = (i % 4800) < 2400;
      const float env = on ? 0.4f : 0.0f;
      left[static_cast<size_t>(i)] = env * std::sin(2.0f * kPi * 330.0f * t);
      right[static_cast<size_t>(i)] = env * std::sin(2.0f * kPi * 495.0f * t);
    }
  }
};

const Source& source() {
  static const Source s;
  return s;
}

struct EngineDeleter {
  void operator()(SonareRealtimeEngine* e) const { sonare_engine_destroy(e); }
};
using EnginePtr = std::unique_ptr<SonareRealtimeEngine, EngineDeleter>;

EnginePtr make_engine(Processor p, Placement placement) {
  SonareRealtimeEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  EnginePtr engine(raw);
  REQUIRE(sonare_engine_prepare(engine.get(), kSampleRate, kBlock, 1024, 1024) == SONARE_OK);

  const float* clip_channels[] = {source().left.data(), source().right.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = kTrackId;
  clip.channels = clip_channels;
  clip.num_channels = kChannels;
  clip.num_samples = kClipFrames;
  clip.length_samples = kClipFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine.get(), &clip, 1) == SONARE_OK);

  if (placement == Placement::kBus) {
    SonareEngineBus bus{};
    bus.bus_id = kBusId;
    bus.channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
    REQUIRE(sonare_engine_set_track_buses(engine.get(), &bus, 1) == SONARE_OK);
  }
  SonareEngineTrackLane lane{};
  lane.track_id = kTrackId;
  lane.output_bus_id = placement == Placement::kBus ? kBusId : 0;
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  REQUIRE(sonare_engine_set_track_lanes(engine.get(), &lane, 1) == SONARE_OK);

  const std::string insert = insert_json(p);
  switch (placement) {
    case Placement::kLane: {
      const std::string json =
          R"({"version":1,"strips":[{"id":"s","inserts":[)" + insert + R"(]}]})";
      REQUIRE(sonare_engine_set_track_strip_json(engine.get(), kTrackId, json.c_str()) ==
              SONARE_OK);
      break;
    }
    case Placement::kBus: {
      const std::string json =
          R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[)" + insert + R"(]}]})";
      REQUIRE(sonare_engine_set_bus_strip_json(engine.get(), kBusId, json.c_str()) == SONARE_OK);
      break;
    }
    case Placement::kMaster: {
      const std::string json =
          R"({"version":1,"strips":[{"id":"master","inserts":[)" + insert + R"(]}]})";
      REQUIRE(sonare_engine_set_master_strip_json(engine.get(), json.c_str()) == SONARE_OK);
      break;
    }
  }
  return engine;
}

struct Planar {
  std::vector<float> left;
  std::vector<float> right;
};

void process_blocks(SonareRealtimeEngine* engine, int blocks, Planar* capture = nullptr) {
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int b = 0; b < blocks; ++b) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, kChannels, kBlock) == SONARE_OK);
    if (capture != nullptr) {
      capture->left.insert(capture->left.end(), left.begin(), left.end());
      capture->right.insert(capture->right.end(), right.begin(), right.end());
    }
  }
}

// Plays from the top so every processor carries state, then stops.
void dirty(SonareRealtimeEngine* engine) {
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  process_blocks(engine, kDirtyBlocks);
  REQUIRE(sonare_engine_stop(engine, -1) == SONARE_OK);
}

Planar bounce(SonareRealtimeEngine* engine) {
  REQUIRE(sonare_engine_seek_sample(engine, kStartSample, -1) == SONARE_OK);
  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = kRenderFrames;
  options.block_size = kBlock;
  options.num_channels = kChannels;
  options.source_sample_rate = static_cast<int>(kSampleRate);
  options.target_sample_rate = static_cast<int>(kSampleRate);
  options.normalize_lufs = 0;
  options.dither = 0;
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.frames == kRenderFrames);
  Planar out;
  for (int64_t f = 0; f < result.frames; ++f) {
    out.left.push_back(result.interleaved[f * kChannels]);
    out.right.push_back(result.interleaved[f * kChannels + 1]);
  }
  sonare_free_bounce_result(&result);
  return out;
}

// Freezes from the start position, then reads the frozen clip back with the lanes
// removed so no insert tail reaches the read-back.
Planar freeze_and_read_back(SonareRealtimeEngine* engine) {
  REQUIRE(sonare_engine_seek_sample(engine, kStartSample, -1) == SONARE_OK);
  SonareEngineFreezeOptions options{};
  options.total_frames = kRenderFrames;
  options.block_size = kBlock;
  options.num_channels = kChannels;
  options.clip_id = 99;
  options.start_ppq = 0.0;
  options.gain = 1.0f;
  SonareEngineFreezeResult result{};
  REQUIRE(sonare_engine_freeze_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_lanes(engine, nullptr, 0) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  Planar out;
  out.left.assign(static_cast<size_t>(kRenderFrames), 0.0f);
  out.right.assign(static_cast<size_t>(kRenderFrames), 0.0f);
  float* channels[] = {out.left.data(), out.right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, kChannels, kRenderFrames, kBlock) ==
          SONARE_OK);
  return out;
}

// Queues seek, optional reset and play together, then renders the span live.
Planar queued_play(SonareRealtimeEngine* engine, bool reset) {
  REQUIRE(sonare_engine_seek_sample(engine, kStartSample, -1) == SONARE_OK);
  if (reset) REQUIRE(engine->engine.reset_processor_state(-1));
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  Planar out;
  process_blocks(engine, static_cast<int>(kRenderFrames / kBlock), &out);
  return out;
}

Planar fresh_reference(const Row& row) {
  EnginePtr engine = make_engine(row.processor, row.placement);
  return row.entry == Entry::kFreeze ? freeze_and_read_back(engine.get()) : bounce(engine.get());
}

struct Diff {
  float max_diff = 0.0f;
  float tolerance = 0.0f;
  float peak = 0.0f;
};

Diff compare(const Planar& a, const Planar& b) {
  REQUIRE(a.left.size() == b.left.size());
  REQUIRE(a.right.size() == b.right.size());
  Diff d;
  for (size_t i = 0; i < a.left.size(); ++i) {
    d.peak = std::max({d.peak, std::fabs(a.left[i]), std::fabs(a.right[i])});
    d.max_diff = std::max(
        {d.max_diff, std::fabs(a.left[i] - b.left[i]), std::fabs(a.right[i] - b.right[i])});
  }
  d.tolerance = 1e-6f * std::max(1.0f, d.peak);
  return d;
}

// Pairwise rows over processor x placement x idle x entry (freeze only on a lane).
constexpr std::array<Row, 18> kRows = {{
    {Processor::kPlate, Placement::kBus, 1, Entry::kBounceRepeat},
    {Processor::kTape, Placement::kLane, 0, Entry::kFreeze},
    {Processor::kDelay, Placement::kBus, 0, Entry::kQueuedResetPlay},
    {Processor::kGate, Placement::kLane, 1, Entry::kQueuedResetPlay},
    {Processor::kGate, Placement::kBus, 37, Entry::kBounceRepeat},
    {Processor::kPlate, Placement::kMaster, 0, Entry::kBounceRepeat},
    {Processor::kTape, Placement::kBus, 37, Entry::kQueuedResetPlay},
    {Processor::kPlate, Placement::kLane, 37, Entry::kQueuedResetPlay},
    {Processor::kTape, Placement::kMaster, 1, Entry::kBounceRepeat},
    {Processor::kTape, Placement::kMaster, 37, Entry::kQueuedResetPlay},
    {Processor::kDelay, Placement::kLane, 1, Entry::kFreeze},
    {Processor::kDelay, Placement::kMaster, 1, Entry::kBounceRepeat},
    {Processor::kGate, Placement::kLane, 0, Entry::kBounceRepeat},
    {Processor::kPlate, Placement::kLane, 0, Entry::kFreeze},
    {Processor::kDelay, Placement::kBus, 37, Entry::kBounceRepeat},
    {Processor::kGate, Placement::kMaster, 1, Entry::kBounceRepeat},
    {Processor::kTape, Placement::kLane, 37, Entry::kFreeze},
    {Processor::kGate, Placement::kLane, 37, Entry::kFreeze},
}};

// Deterministic white noise in [-0.5, 0.5).
float next_noise(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
}

// One sample in: a ring modulator's carrier is zero at sample 0.
constexpr int kImpulseOffset = 1;

std::vector<std::vector<float>> run_insert(sonare::rt::ProcessorBase& insert, bool dirty_first,
                                           int impulse_frames) {
  constexpr int kNoiseBlocks = 64;
  std::array<std::array<float, kBlock>, kChannels> buffer{};
  float* io[] = {buffer[0].data(), buffer[1].data()};
  if (dirty_first) {
    uint32_t state = 182u;
    for (int b = 0; b < kNoiseBlocks; ++b) {
      for (auto& plane : buffer) {
        for (float& v : plane) v = next_noise(state);
      }
      insert.process(io, kChannels, kBlock);
    }
    insert.reset();
  }
  std::vector<std::vector<float>> out(kChannels);
  for (int start = 0; start < impulse_frames; start += kBlock) {
    for (auto& plane : buffer) {
      plane.fill(0.0f);
      if (start == 0) plane[kImpulseOffset] = 1.0f;
    }
    insert.process(io, kChannels, kBlock);
    for (int ch = 0; ch < kChannels; ++ch) {
      out[static_cast<size_t>(ch)].insert(out[static_cast<size_t>(ch)].end(),
                                          buffer[static_cast<size_t>(ch)].begin(),
                                          buffer[static_cast<size_t>(ch)].end());
    }
  }
  return out;
}

}  // namespace

TEST_CASE("processor reset: every entry starts where a fresh engine does",
          "[engine][processor_reset]") {
  for (const Row& row : kRows) {
    INFO(processor_name(row.processor) << " / " << placement_name(row.placement) << " / idle "
                                       << row.idle_blocks << " / " << entry_name(row.entry));
    const Planar expected = fresh_reference(row);
    EnginePtr live = make_engine(row.processor, row.placement);
    dirty(live.get());
    std::vector<Planar> actual;
    switch (row.entry) {
      case Entry::kBounceRepeat:
        actual.push_back(bounce(live.get()));
        process_blocks(live.get(), row.idle_blocks);
        actual.push_back(bounce(live.get()));
        break;
      case Entry::kFreeze:
        process_blocks(live.get(), row.idle_blocks);
        actual.push_back(freeze_and_read_back(live.get()));
        break;
      case Entry::kQueuedResetPlay:
        process_blocks(live.get(), row.idle_blocks);
        actual.push_back(queued_play(live.get(), true));
        break;
    }
    for (const Planar& a : actual) {
      const Diff d = compare(a, expected);
      INFO("max diff " << d.max_diff << " tolerance " << d.tolerance);
      REQUIRE(d.peak > 0.0f);
      REQUIRE(d.max_diff <= d.tolerance);
    }
  }
}

TEST_CASE("processor reset: without the reset a ringing plate diverges",
          "[engine][processor_reset]") {
  const Row row{Processor::kPlate, Placement::kLane, 37, Entry::kQueuedResetPlay};
  const Planar expected = fresh_reference(row);
  EnginePtr live = make_engine(row.processor, row.placement);
  dirty(live.get());
  process_blocks(live.get(), row.idle_blocks);
  const Diff d = compare(queued_play(live.get(), false), expected);
  INFO("max diff " << d.max_diff << " tolerance " << d.tolerance);
  REQUIRE(d.max_diff > d.tolerance);
}

TEST_CASE("processor reset: reset and play split across a block when the ring is nearly full",
          "[engine][processor_reset]") {
  // drain_commands pops kMaxCommandsPerBlock per block, so 63 pending commands
  // plus the reset fill the first block and play lands in the next one.
  static_assert(sonare::engine::RealtimeEngine::kMaxCommandsPerBlock == 64);
  EnginePtr engine = make_engine(Processor::kPlate, Placement::kLane);
  for (int i = 0; i < 63; ++i) {
    REQUIRE(sonare_engine_seek_sample(engine.get(), kStartSample, -1) == SONARE_OK);
  }
  REQUIRE(engine->engine.reset_processor_state(-1));
  REQUIRE(sonare_engine_play(engine.get(), -1) == SONARE_OK);
  process_blocks(engine.get(), 1);
  CHECK_FALSE(engine->engine.transport().playing());
  process_blocks(engine.get(), 1);
  CHECK(engine->engine.transport().playing());
}

TEST_CASE("processor reset: the queued reset allocates nothing on the audio thread",
          "[engine][processor_reset]") {
  EnginePtr engine = make_engine(Processor::kPlate, Placement::kBus);
  const std::string lane_json = R"({"version":1,"strips":[{"id":"s","inserts":[)" +
                                insert_json(Processor::kTape) + "," +
                                insert_json(Processor::kDelay) + R"(]}]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine.get(), kTrackId, lane_json.c_str()) ==
          SONARE_OK);
  const std::string master_json = R"({"version":1,"strips":[{"id":"master","inserts":[)" +
                                  insert_json(Processor::kGate) + R"(]}]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine.get(), master_json.c_str()) == SONARE_OK);
  dirty(engine.get());
  process_blocks(engine.get(), 4);
  REQUIRE(engine->engine.reset_processor_state(-1));
  REQUIRE(sonare_engine_play(engine.get(), -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    engine->engine.process(io, kChannels, kBlock);
    allocations = guard.count();
  }
  REQUIRE(allocations == 0);
  REQUIRE(engine->engine.transport().playing());
}

TEST_CASE("processor reset: every factory insert resets to its prepared state",
          "[engine][processor_reset]") {
  // Inserts whose output depends on a resource "{}" cannot supply.
  const std::vector<std::string> kNeedsExternalIr = {
      "effects.reverb.convolution",  // convolves with a caller-loaded impulse response
  };
  for (const std::string& name : sonare::mastering::api::insert_factory_names()) {
    INFO(name);
    if (std::find(kNeedsExternalIr.begin(), kNeedsExternalIr.end(), name) !=
        kNeedsExternalIr.end()) {
      continue;
    }
    auto a = sonare::mastering::api::make_insert(name, "{}");
    auto b = sonare::mastering::api::make_insert(name, "{}");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    a->prepare(kSampleRate, kBlock);
    b->prepare(kSampleRate, kBlock);
    const int lookahead = std::max(a->latency_samples(), 0);
    const int frames = ((lookahead + kBlock) / kBlock + 1) * kBlock;
    const auto clean = run_insert(*a, false, frames);
    const auto reset = run_insert(*b, true, frames);
    float peak = 0.0f;
    float max_diff = 0.0f;
    for (size_t ch = 0; ch < clean.size(); ++ch) {
      for (size_t i = 0; i < clean[ch].size(); ++i) {
        peak = std::max(peak, std::fabs(clean[ch][i]));
        max_diff = std::max(max_diff, std::fabs(clean[ch][i] - reset[ch][i]));
      }
    }
    INFO("lookahead " << lookahead << " peak " << peak << " max diff " << max_diff);
    CHECK(peak > 0.0f);
    CHECK(clean == reset);
  }
}
