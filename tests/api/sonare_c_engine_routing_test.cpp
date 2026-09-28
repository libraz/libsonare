/// @file sonare_c_engine_routing_test.cpp
/// @brief Engine C ABI bus routing (bus output, bus sends), bus/master
///        sidechain keys, and MIDI clip gain/fade conversion.

#include <initializer_list>
#include <vector>

#include "sonare_c_engine_test_helpers.h"
#include "util/db.h"

namespace {

constexpr int kRoutingBlock = 256;
constexpr int kRoutingBlocks = 40;
constexpr int kRoutingFrames = kRoutingBlock * kRoutingBlocks;

#if defined(SONARE_WITH_MIXING)
constexpr const char* kDuckerBusJson =
    R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":"{\"thresholdDb\":-20,\"ratio\":20,\"attackMs\":0.05,\"releaseMs\":80,\"rangeDb\":30}"}]}],"connections":[]})";
constexpr const char* kLimiterBusJson =
    R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"dynamics.limiter","params":"{\"thresholdDb\":24,\"lookaheadMs\":0,\"releaseMs\":50}"}]}],"connections":[]})";
constexpr const char* kDuckerMasterJson =
    R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"dynamics.duckingProcessor","params":"{\"thresholdDb\":-20,\"ratio\":20,\"attackMs\":0.05,\"releaseMs\":80,\"rangeDb\":30}"}]}],"buses":[],"connections":[]})";

// A DC 1.0 mono source shared by every clip; clips never outlive it.
const std::vector<float>& dc_source() {
  static const std::vector<float> source(kRoutingFrames, 1.0f);
  return source;
}

struct DcTrack {
  uint32_t track_id;
  float gain;
};

// Engine with one DC clip per track, prepared for a mono output.
SonareRealtimeEngine* make_routing_engine(std::initializer_list<DcTrack> tracks) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kRoutingBlock, 64, 64) == SONARE_OK);
  static const float* channels[] = {dc_source().data()};
  std::vector<SonareEngineClip> clips;
  uint32_t clip_id = 1;
  for (const DcTrack& track : tracks) {
    SonareEngineClip clip{};
    clip.id = clip_id++;
    clip.track_id = track.track_id;
    clip.channels = channels;
    clip.num_channels = 1;
    clip.num_samples = kRoutingFrames;
    clip.length_samples = kRoutingFrames;
    clip.gain = track.gain;
    clips.push_back(clip);
  }
  REQUIRE(sonare_engine_set_clips(engine, clips.data(), clips.size()) == SONARE_OK);
  return engine;
}

// Plays from the top and returns every rendered mono sample.
std::vector<float> render_all(SonareRealtimeEngine* engine, int blocks = 30) {
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::vector<float> out;
  std::array<float, kRoutingBlock> block{};
  float* io[] = {block.data()};
  for (int b = 0; b < blocks; ++b) {
    block.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kRoutingBlock) == SONARE_OK);
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

float settled(SonareRealtimeEngine* engine) { return render_all(engine).back(); }

// Direct-to-master level of one DC lane, the unit the routing cases scale.
float direct_level() {
  SonareRealtimeEngine* engine = make_routing_engine({{10, 1.0f}});
  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  const float level = settled(engine);
  sonare_engine_destroy(engine);
  REQUIRE(level > 0.5f);
  return level;
}
#endif

}  // namespace

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine bus output_bus_id routes a bus into another bus", "[c_engine_routing]") {
  const float x = direct_level();
  const float half = sonare::db_to_linear(-6.0f);
  const auto run = [](uint32_t bus1_output) {
    SonareRealtimeEngine* engine = make_routing_engine({{10, 1.0f}});
    SonareEngineBus buses[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, bus1_output, nullptr, 0},
                               {2, -6.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
    REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
    SonareEngineTrackLane lane[] = {{10, nullptr, 0, 1, SONARE_CHANNEL_LAYOUT_STEREO}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
    const float level = settled(engine);
    sonare_engine_destroy(engine);
    return level;
  };
  CHECK(run(0) == Catch::Approx(x).epsilon(1e-3));
  // Through bus 2, its -6 dB gain_db applies on top.
  CHECK(run(2) == Catch::Approx(x * half).epsilon(1e-3));
}

TEST_CASE("sonare_engine bus sends tap before or after the bus gain_db", "[c_engine_routing]") {
  const float x = direct_level();
  const float g1 = sonare::db_to_linear(-12.0f);
  const auto run = [](int timing, float level_db, int enabled) {
    SonareRealtimeEngine* engine = make_routing_engine({{10, 1.0f}});
    SonareEngineTrackSend sends[] = {{2, level_db, enabled, timing}};
    SonareEngineBus buses[] = {{1, -12.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, sends, 1},
                               {2, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
    REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
    SonareEngineTrackLane lane[] = {{10, nullptr, 0, 1, SONARE_CHANNEL_LAYOUT_STEREO}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
    const float level = settled(engine);
    sonare_engine_destroy(engine);
    return level;
  };
  CHECK(run(SONARE_SEND_TIMING_PRE_FADER, 0.0f, 1) == Catch::Approx(x * (g1 + 1.0f)).epsilon(1e-3));
  CHECK(run(SONARE_SEND_TIMING_POST_FADER, 0.0f, 1) == Catch::Approx(x * 2.0f * g1).epsilon(1e-3));
  const float half = sonare::db_to_linear(-6.0f);
  CHECK(run(SONARE_SEND_TIMING_PRE_FADER, -6.0f, 1) ==
        Catch::Approx(x * (g1 + half)).epsilon(1e-3));
  CHECK(run(SONARE_SEND_TIMING_POST_FADER, 0.0f, 0) == Catch::Approx(x * g1).epsilon(1e-2));
}

namespace {

// Track 10 is quiet program (-26 dB, under the duckers' -20 dB threshold) into
// bus 2, which carries a ducker keyed from bus 1. Track 30 is a loud key-only
// source into bus 1, whose -60 dB gain_db keeps it out of the mix while its key
// (tapped before gain_db) stays loud. Bus 1 carries a pass-through limiter so it
// has an insert 0 to key; the master carries a ducker.
SonareRealtimeEngine* make_keyed_rig() {
  SonareRealtimeEngine* engine = make_routing_engine({{10, 0.05f}, {30, 1.0f}});
  SonareEngineBus buses[] = {{1, -60.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0},
                             {2, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 2, SONARE_CHANNEL_LAYOUT_STEREO},
                                   {30, nullptr, 0, 1, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, kLimiterBusJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 2, kDuckerBusJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_json(engine, kDuckerMasterJson) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) ==
          SONARE_OK);
  return engine;
}

}  // namespace

TEST_CASE("sonare_engine refused bus routing and sidechain calls leave the render unchanged",
          "[c_engine_routing]") {
  SonareRealtimeEngine* control = make_keyed_rig();
  SonareRealtimeEngine* engine = make_keyed_rig();
  constexpr SonareError kInvalid = SONARE_ERROR_INVALID_PARAMETER;

  const auto buses_refused = [&](std::vector<SonareEngineBus> buses) {
    CHECK(sonare_engine_set_track_buses(engine, buses.data(), buses.size()) == kInvalid);
  };
  const auto bus = [](uint32_t id, uint32_t output = 0,
                      const SonareEngineTrackSend* sends = nullptr, size_t send_count = 0) {
    return SonareEngineBus{id, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, output, sends, send_count};
  };
  const SonareEngineTrackSend to_bus1[] = {{1, 0.0f, 1, SONARE_SEND_TIMING_PRE_FADER}};
  const SonareEngineTrackSend to_bus9[] = {{9, 0.0f, 1, SONARE_SEND_TIMING_PRE_FADER}};
  const SonareEngineTrackSend twice_to_bus2[] = {{2, 0.0f, 1, SONARE_SEND_TIMING_PRE_FADER},
                                                 {2, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  buses_refused({bus(1), bus(2, 0, nullptr, 1)});        // null sends with a count
  buses_refused({bus(1, 1), bus(2)});                    // self output
  buses_refused({bus(1, 9), bus(2)});                    // undeclared output
  buses_refused({bus(1, 2), bus(2, 1)});                 // output cycle
  buses_refused({bus(1, 0, to_bus1, 1), bus(2)});        // self send
  buses_refused({bus(1, 0, to_bus9, 1), bus(2)});        // undeclared send target
  buses_refused({bus(1, 0, twice_to_bus2, 2), bus(2)});  // duplicate send target
  buses_refused({bus(1), bus(2, 0, to_bus1, 1)});        // send 2 -> 1 against the key 1 -> 2
  buses_refused({bus(1), bus(2, 1)});                    // output 2 -> 1 against the key
  buses_refused({bus(2)});                               // retires bus 1, still a lane output
  buses_refused({SonareEngineBus{1, 0.0f, 99, 0, nullptr, 0}, bus(2)});  // bad layout
  CHECK(sonare_engine_set_track_buses(engine, nullptr, 1) == kInvalid);

  // Bus keys: bus id 0, undeclared bus, undeclared sources, self key,
  // out-of-range insert, a cycle against the existing key, and bad kinds.
  CHECK(sonare_engine_set_bus_sidechain(engine, 0, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) ==
        kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 9, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) ==
        kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 99) ==
        kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 9) == kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) == kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 1, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_BUS, 2) == kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, 2, 30) == kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, -1, 30) == kInvalid);
  CHECK(sonare_engine_set_bus_sidechain(nullptr, 1, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        kInvalid);

  // Master keys: out-of-range insert, undeclared sources, bad kinds.
  CHECK(sonare_engine_set_master_sidechain(engine, 1, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        kInvalid);
  CHECK(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 99) ==
        kInvalid);
  CHECK(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_BUS, 9) == kInvalid);
  CHECK(sonare_engine_set_master_sidechain(engine, 0, 2, 30) == kInvalid);
  CHECK(sonare_engine_set_master_sidechain(engine, 0, -1, 30) == kInvalid);
  CHECK(sonare_engine_set_master_sidechain(nullptr, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
        kInvalid);

  const std::vector<float> expected = render_all(control);
  const std::vector<float> actual = render_all(engine);
  REQUIRE(std::abs(expected.back()) > 1e-3f);
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    REQUIRE(actual[i] == Catch::Approx(expected[i]).margin(1e-6f));
  }
  sonare_engine_destroy(engine);
  sonare_engine_destroy(control);
}

TEST_CASE("sonare_engine bus and master sidechain keys duck and clear", "[c_engine_routing]") {
  // Unkeyed reference: the same rig with its bus key cleared before any render.
  SonareRealtimeEngine* reference = make_keyed_rig();
  REQUIRE(sonare_engine_set_bus_sidechain(reference, 2, 0, SONARE_SIDECHAIN_SOURCE_BUS, 0) ==
          SONARE_OK);
  const float unkeyed = settled(reference);
  sonare_engine_destroy(reference);
  REQUIRE(unkeyed > 0.02f);

  SonareRealtimeEngine* engine = make_keyed_rig();
  // Bus 1's loud key ducks bus 2, which carries nearly all of the mix.
  CHECK(settled(engine) < unkeyed * 0.3f);
  // A track-sourced key on bus 2 ducks it the same way.
  REQUIRE(sonare_engine_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
          SONARE_OK);
  CHECK(settled(engine) < unkeyed * 0.3f);
  // The master ducker keyed from a track or a bus pulls the whole mix down.
  REQUIRE(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 30) ==
          SONARE_OK);
  CHECK(settled(engine) < unkeyed * 0.3f);
  REQUIRE(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_BUS, 1) ==
          SONARE_OK);
  CHECK(settled(engine) < unkeyed * 0.3f);

  // source_id 0 clears a binding: the master one leaves bus 2 keyed alone, and
  // clearing that too returns the render to the unkeyed level.
  REQUIRE(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_BUS, 0) ==
          SONARE_OK);
  CHECK(settled(engine) < unkeyed * 0.3f);
  REQUIRE(sonare_engine_set_bus_sidechain(engine, 2, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 0) ==
          SONARE_OK);
  // Replay until the duckers' 80 ms release has run out.
  for (int pass = 0; pass < 8; ++pass) settled(engine);
  CHECK(settled(engine) == Catch::Approx(unkeyed).epsilon(1e-4));
  sonare_engine_destroy(engine);
}
#else
TEST_CASE("sonare_engine bus and master sidechain need the mixing feature", "[c_engine_routing]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  CHECK(sonare_engine_set_bus_sidechain(engine, 1, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_engine_set_master_sidechain(engine, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 10) ==
        SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

constexpr uint32_t kMidiDestination = 9;

SonareEngineMidiClipSchedule held_note_clip(const SonareEngineMidiEvent* events) {
  SonareEngineMidiClipSchedule clip{};
  clip.id = 42;
  clip.track_id = kMidiDestination;
  clip.length_samples = kRoutingFrames;
  clip.destination_id = kMidiDestination;
  clip.events = events;
  clip.event_count = 1;
  clip.gain = 1.0f;
  return clip;
}

std::vector<float> render_midi_clip(const SonareEngineMidiClipSchedule& clip) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kRoutingBlock, 16, 16) == SONARE_OK);
  SonareEngineBuiltinSynthConfig synth{};
  synth.gain = 0.5f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kMidiDestination, &synth) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::vector<float> out;
  std::array<float, kRoutingBlock> left{};
  std::array<float, kRoutingBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int b = 0; b < 8; ++b) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 2, kRoutingBlock) == SONARE_OK);
    out.insert(out.end(), left.begin(), left.end());
  }
  sonare_engine_destroy(engine);
  return out;
}

}  // namespace

TEST_CASE("sonare_engine MIDI clip gain scales the destination's rendered audio",
          "[c_engine_routing]") {
  const SonareEngineMidiEvent events[] = {{0, midi1_word(0x9, 0, 60, 100), 0, 0, 0, 1, 0, 0, 0}};
  SonareEngineMidiClipSchedule unity = held_note_clip(events);
  SonareEngineMidiClipSchedule half = unity;
  half.gain = 0.5f;
  const std::vector<float> a = render_midi_clip(unity);
  const std::vector<float> b = render_midi_clip(half);
  float peak = 0.0f;
  for (float v : a) peak = std::max(peak, std::abs(v));
  REQUIRE(peak > 0.01f);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(b[i] == Catch::Approx(0.5f * a[i]).margin(1e-6f));
  }
}

TEST_CASE("sonare_engine_set_midi_clips refuses a bad gain or fade", "[c_engine_routing]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kRoutingBlock, 16, 16) == SONARE_OK);
  const SonareEngineMidiEvent events[] = {{0, midi1_word(0x9, 0, 60, 100), 0, 0, 0, 1, 0, 0, 0}};
  const SonareEngineMidiClipSchedule good = held_note_clip(events);
  constexpr SonareError kInvalid = SONARE_ERROR_INVALID_PARAMETER;

  SonareEngineMidiClipSchedule clip = good;
  clip.fade_in_samples = 256;
  clip.fade_out_samples = 256;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  clip = good;
  clip.gain = 0.0f;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);

  clip = good;
  clip.gain = -0.5f;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  clip.gain = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  clip.gain = std::numeric_limits<float>::infinity();
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  clip = good;
  clip.fade_in_samples = -1;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  clip = good;
  clip.fade_out_samples = -1;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  // An open-ended clip has no end to fade out towards; a fade-in is still fine.
  clip = good;
  clip.length_samples = 0;
  clip.fade_out_samples = 256;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == kInvalid);
  clip.fade_out_samples = 0;
  clip.fade_in_samples = 256;
  CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  sonare_engine_destroy(engine);
}
#endif
