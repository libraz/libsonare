/// @file bus_strip_agreement_test.cpp
/// @brief Cross-implementation agreement for bus/master pan and EQ: the live
///        engine (TrackMixerRuntime), the engine's offline render, and project
///        bounce (BusNode) must all produce the same audio for the same scene.
///
/// Coverwise model (pairwise, seed 42; control case listed first): panMode
/// {balance, stereo, dual} x panLaw {const3db, const6db, linear0db} x panValue
/// {zero, nonzero} x busEq {none, active, disabledBands} x width {one, nonone}
/// x stripEq (applied to both the track strip and the master strip)
/// {none, active} -> 12 cases covering all 93 pairwise tuples.

#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project_edit.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(SONARE_WITH_MIXING)

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
// ~213 ms total, ~53 ms discarded so the synth's attack settles before the
// compared window -- short enough to stay well under the [.][slow] threshold.
constexpr int64_t kTotalFrames = kBlockSize * 80;
constexpr int64_t kSettleFrames = kBlockSize * 20;
constexpr uint32_t kBusId = 1;
constexpr uint32_t kDestination = 77;
// max|a-b| <= kMetricScale * max(1, max|a|), per the design doc's test strategy.
constexpr double kMetricScale = 1.0e-6;

struct BusStripCase {
  const char* name;
  int pan_mode;
  int pan_law;
  float pan;
  const char* bus_eq;  // "none" | "active" | "disabledBands"
  float width;
  bool strip_eq;  // applied identically to the track strip and the master strip
};

// clang-format off
const BusStripCase kCases[] = {
    {"control (all default)",                             0, 0, 0.0f, "none",          1.0f, false},
    {"stereo/const3db/nonzero/active/nonone/active",       1, 0, 0.6f, "active",        1.6f, true},
    {"dual/const6db/nonzero/disabledBands/one/active",     2, 2, 0.6f, "disabledBands", 1.0f, true},
    {"dual/const3db/zero/disabledBands/nonone/none",       2, 0, 0.0f, "disabledBands", 1.6f, false},
    {"stereo/const6db/zero/none/nonone/none",              1, 2, 0.0f, "none",          1.6f, false},
    {"balance/linear0db/nonzero/active/one/none",          0, 3, 0.6f, "active",        1.0f, false},
    {"dual/linear0db/zero/none/nonone/active",             2, 3, 0.0f, "none",          1.6f, true},
    {"balance/const6db/zero/active/nonone/active",         0, 2, 0.0f, "active",        1.6f, true},
    {"stereo/linear0db/zero/disabledBands/one/active",     1, 3, 0.0f, "disabledBands", 1.0f, true},
    {"balance/const6db/nonzero/none/one/none",             0, 2, 0.6f, "none",          1.0f, false},
    {"dual/linear0db/nonzero/active/nonone/active",        2, 3, 0.6f, "active",        1.6f, true},
    {"balance/linear0db/nonzero/disabledBands/nonone/active", 0, 3, 0.6f, "disabledBands", 1.6f, true},
};
// clang-format on

// One clearly audible peaking band, shared by every "active"/"disabledBands" case.
std::string band_fragment() {
  return R"("bands":[{"type":"Peak","frequencyHz":1000.0,"gainDb":6.0,"q":0.7,"enabled":true}])";
}

std::string eq_fragment(const std::string& variant) {
  if (variant == "none") return "";
  if (variant == "active") return std::string(R"(,"eq":{"enabled":true,)") + band_fragment() + "}";
  // "disabledBands": bands are retained but the EQ stage does not run.
  return std::string(R"(,"eq":{"enabled":false,)") + band_fragment() + "}";
}

std::string strip_eq_fragment(bool active) {
  return active ? eq_fragment("active") : std::string();
}

// Bus body shared verbatim between the engine's set_bus_strip_json wrapper and
// the project scene's buses[] entry.
std::string bus_body(const BusStripCase& c) {
  return std::string(R"("id":"fx","panMode":)") + std::to_string(c.pan_mode) + R"(,"panLaw":)" +
         std::to_string(c.pan_law) + R"(,"pan":)" + std::to_string(c.pan) + R"(,"width":)" +
         std::to_string(c.width) + eq_fragment(c.bus_eq);
}

std::string track_strip_body(bool eq_active) {
  return std::string(R"("id":"track")") + strip_eq_fragment(eq_active);
}

// Engine-side master is a Strip (no "role" field); the standalone graph's
// master is the Bus with role == "master". Same eq, two different shapes.
std::string master_strip_body(bool eq_active) {
  return std::string(R"("id":"master")") + strip_eq_fragment(eq_active);
}

std::string master_bus_body(bool eq_active) {
  return std::string(R"("id":"master","role":"master")") + strip_eq_fragment(eq_active);
}

std::string wrap_strip_scene(const std::string& body) {
  return std::string(R"({"version":1,"strips":[{)") + body + R"(}],"buses":[],"connections":[]})";
}

std::string wrap_bus_scene(const std::string& body) {
  return std::string(R"({"version":1,"strips":[],"buses":[{)") + body + R"(}],"connections":[]})";
}

// Track -> fx bus -> master, matching the engine's group-routing lane
// (output_bus_id bypasses the master mix) exactly: an explicit bus keeps its
// authored topology only when connected, so both edges are spelled out.
std::string project_scene_json(const BusStripCase& c) {
  return std::string(R"({"version":1,"strips":[{)") + track_strip_body(c.strip_eq) +
         R"(}],"buses":[{)" + bus_body(c) + R"(},{)" + master_bus_body(c.strip_eq) +
         R"(}],"connections":[)" + R"({"source":"track","destination":"fx"},)" +
         R"({"source":"fx","destination":"master"}]})";
}

SonareEngineBuiltinSynthConfig test_synth_config() {
  SonareEngineBuiltinSynthConfig synth{};
  synth.waveform = 0;  // sine
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

SonareBuiltinSynthConfig project_synth_config() {
  SonareBuiltinSynthConfig synth{};
  synth.waveform = 0;
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

// Builds a fresh engine wired identically to project_scene_json: track -> fx
// bus (group routing via output_bus_id) -> master, one held note.
void configure_engine(SonareRealtimeEngine* engine, const BusStripCase& c, uint32_t track_id) {
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlockSize, 64, 16) == SONARE_OK);
  SonareEngineBus buses[] = {{kBusId, 0.0f, static_cast<uint8_t>(SONARE_CHANNEL_LAYOUT_STEREO)}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  const std::string bus_json = wrap_bus_scene(bus_body(c));
  REQUIRE(sonare_engine_set_bus_strip_json(engine, kBusId, bus_json.c_str()) == SONARE_OK);
  const SonareEngineTrackLane lane[] = {{track_id, nullptr, 0, kBusId, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  const std::string track_json = wrap_strip_scene(track_strip_body(c.strip_eq));
  REQUIRE(sonare_engine_set_track_strip_json(engine, track_id, track_json.c_str()) == SONARE_OK);
  const std::string master_json = wrap_strip_scene(master_strip_body(c.strip_eq));
  REQUIRE(sonare_engine_set_master_strip_json(engine, master_json.c_str()) == SONARE_OK);

  SonareEngineBuiltinSynthConfig synth = test_synth_config();
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);

  const SonareEngineMidiEvent events[] = {
      {0, 0x20903C7Fu, 0u, 0u, 0u, 1u, 0u, 0u, 0u},  // note-on, note 60, vel 127, no note-off
  };
  const SonareEngineMidiClipSchedule clips[] = {
      {1, track_id, 0, 0.0, kTotalFrames * 2, 0, 0, kDestination, events, std::size(events)},
  };
  REQUIRE(sonare_engine_set_midi_clips(engine, clips, std::size(clips)) == SONARE_OK);
}

// Interleaved stereo render via the live block-processing loop (a).
std::vector<float> render_live(const BusStripCase& c, uint32_t track_id) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  configure_engine(engine, c, track_id);
  // Compare settled state: set_bus_strip glides pan and width in, as a live edit should.
  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> interleaved(static_cast<size_t>(kTotalFrames) * 2);
  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
  float* channels[] = {left.data(), right.data()};
  const int64_t num_blocks = kTotalFrames / kBlockSize;
  for (int64_t b = 0; b < num_blocks; ++b) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, channels, 2, kBlockSize) == SONARE_OK);
    for (int i = 0; i < kBlockSize; ++i) {
      const size_t frame = static_cast<size_t>(b * kBlockSize + i);
      interleaved[frame * 2] = left[i];
      interleaved[frame * 2 + 1] = right[i];
    }
  }
  sonare_engine_destroy(engine);
  return interleaved;
}

// Interleaved stereo render via a single sonare_engine_render_offline call (b).
std::vector<float> render_offline(const BusStripCase& c, uint32_t track_id) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  configure_engine(engine, c, track_id);
  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);

  std::vector<float> left(static_cast<size_t>(kTotalFrames));
  std::vector<float> right(static_cast<size_t>(kTotalFrames));
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, kTotalFrames, kBlockSize) == SONARE_OK);
  sonare_engine_destroy(engine);

  std::vector<float> interleaved(static_cast<size_t>(kTotalFrames) * 2);
  for (int64_t i = 0; i < kTotalFrames; ++i) {
    interleaved[static_cast<size_t>(i) * 2] = left[static_cast<size_t>(i)];
    interleaved[static_cast<size_t>(i) * 2 + 1] = right[static_cast<size_t>(i)];
  }
  return interleaved;
}

// Interleaved stereo render via project bounce, which routes through the
// standalone graph's BusNode (c).
std::vector<float> render_project_bounce(const BusStripCase& c) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  REQUIRE(sonare_project_set_mixer_scene_json(project, project_scene_json(c).c_str()) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
  REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "track", "") == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = kTotalFrames;
  options.block_size = kBlockSize;
  options.num_channels = 2;
  options.sample_rate = static_cast<int>(kSampleRate);
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config = project_synth_config();

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out_len == static_cast<size_t>(kTotalFrames) * 2);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return result;
}

struct Agreement {
  double max_diff = 0.0;
  double bound = 0.0;
  bool ok = false;
};

// max|reference - other| <= kMetricScale * max(1, max|reference|), evaluated
// only from kSettleFrames onward (after the synth attack and any pan/EQ
// construction transient have settled).
Agreement check_agreement(const std::vector<float>& reference, const std::vector<float>& other) {
  Agreement result;
  const size_t begin = static_cast<size_t>(kSettleFrames) * 2;
  const size_t n = std::min(reference.size(), other.size());
  double max_ref = 0.0;
  for (size_t i = begin; i < n; ++i) {
    result.max_diff =
        std::max(result.max_diff, static_cast<double>(std::abs(reference[i] - other[i])));
    max_ref = std::max(max_ref, static_cast<double>(std::abs(reference[i])));
  }
  result.bound = kMetricScale * std::max(1.0, max_ref);
  result.ok = result.max_diff <= result.bound;
  return result;
}

}  // namespace

TEST_CASE("bus and master pan+EQ agree across live engine, offline render, and project bounce",
          "[engine][mixing]") {
  constexpr uint32_t kTrackId = 10;
  for (size_t i = 0; i < std::size(kCases); ++i) {
    const BusStripCase& c = kCases[i];
    DYNAMIC_SECTION("case " << i << ": " << c.name) {
      if (i == 0) {
        INFO(
            "This is the control case (every new field at its default). Per the design "
            "doc's test strategy, a failure here is a disagreement outside this feature's "
            "scope -- report it rather than treating it as this test's defect.");
      }
      const std::vector<float> live = render_live(c, kTrackId);
      const std::vector<float> offline = render_offline(c, kTrackId);
      const std::vector<float> project = render_project_bounce(c);

      const Agreement live_vs_offline = check_agreement(offline, live);
      INFO("live vs offline: max_diff=" << live_vs_offline.max_diff
                                        << " bound=" << live_vs_offline.bound);
      CHECK(live_vs_offline.ok);

      const Agreement offline_vs_project = check_agreement(offline, project);
      INFO("offline vs project bounce: max_diff=" << offline_vs_project.max_diff
                                                  << " bound=" << offline_vs_project.bound);
      CHECK(offline_vs_project.ok);
    }
  }
}

TEST_CASE("project serialize/deserialize preserves bus and master pan+EQ through project bounce",
          "[engine][mixing]") {
  // The project C ABI has no getter for its mixer scene JSON, so this cannot
  // literally re-derive set_track_strip/set_bus_strip/set_master_strip from the
  // reloaded project and re-run the engine's offline render, as the design doc
  // describes. It instead re-bounces the reloaded PROJECT and compares against
  // the pre-reload bounce, which still exercises the same round trip (success
  // condition 1) at the audio level for the fields this feature adds.
  const BusStripCase cases[] = {kCases[0], kCases[1]};
  for (size_t i = 0; i < std::size(cases); ++i) {
    const BusStripCase& c = cases[i];
    DYNAMIC_SECTION("case " << i << ": " << c.name) {
      SonareProject* project = nullptr;
      REQUIRE(sonare_project_create(&project) == SONARE_OK);
      REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
      REQUIRE(sonare_project_set_mixer_scene_json(project, project_scene_json(c).c_str()) ==
              SONARE_OK);
      uint32_t track = 0;
      uint32_t clip = 0;
      REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);
      const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
      REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) ==
              SONARE_OK);
      REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
      REQUIRE(sonare_project_set_track_route(project, track, "track", "") == SONARE_OK);

      char* json = nullptr;
      size_t json_len = 0;
      REQUIRE(sonare_project_serialize(project, &json, &json_len) == SONARE_OK);

      SonareProject* reloaded = nullptr;
      REQUIRE(sonare_project_deserialize(json, json_len, &reloaded, nullptr) == SONARE_OK);
      sonare_free_string(json);

      SonareProjectBounceOptions options{};
      options.total_frames = kTotalFrames;
      options.block_size = kBlockSize;
      options.num_channels = 2;
      options.sample_rate = static_cast<int>(kSampleRate);
      SonareBuiltinInstrumentBinding binding{};
      binding.destination_id = kDestination;
      binding.config = project_synth_config();

      float* before_out = nullptr;
      size_t before_len = 0;
      REQUIRE(sonare_project_bounce_with_builtin_instruments(
                  project, &options, &binding, 1, &before_out, &before_len) == SONARE_OK);
      std::vector<float> before(before_out, before_out + before_len);
      sonare_free_floats(before_out);

      float* after_out = nullptr;
      size_t after_len = 0;
      REQUIRE(sonare_project_bounce_with_builtin_instruments(reloaded, &options, &binding, 1,
                                                             &after_out, &after_len) == SONARE_OK);
      std::vector<float> after(after_out, after_out + after_len);
      sonare_free_floats(after_out);

      const Agreement agreement = check_agreement(before, after);
      INFO("before vs after reload: max_diff=" << agreement.max_diff
                                               << " bound=" << agreement.bound);
      CHECK(agreement.ok);

      sonare_project_destroy(reloaded);
      sonare_project_destroy(project);
    }
  }
}

#endif  // defined(SONARE_WITH_MIXING)
