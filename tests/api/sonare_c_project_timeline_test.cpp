/// @file sonare_c_project_timeline_test.cpp
/// @brief Compiled project timelines applied to a caller-owned realtime engine
///        through the C ABI: live render against project bounce, re-apply after
///        edits, handle lifetime, automation coexistence and the stopped-only rule.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 128;
constexpr int64_t kFrames = kBlockSize * 500;
constexpr uint32_t kDestination = 9;
[[maybe_unused]] constexpr uint32_t kOtherAutomationTarget = 0x1234u;
// A marker name long enough to live on the heap rather than in a small-string buffer.
constexpr const char* kLongMarkerName = "verse-two-with-a-name-longer-than-sso";

std::vector<float> stereo_tone(int64_t frames) {
  std::vector<float> interleaved(static_cast<size_t>(frames) * 2);
  for (int64_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    interleaved[static_cast<size_t>(i) * 2] =
        static_cast<float>(0.25 * std::sin(kTwoPiD * 220.0 * t));
    interleaved[static_cast<size_t>(i) * 2 + 1] =
        static_cast<float>(0.2 * std::sin(kTwoPiD * 330.0 * t));
  }
  return interleaved;
}

SonareBuiltinSynthConfig project_synth() {
  SonareBuiltinSynthConfig config{};
  config.waveform = 1;
  config.gain = 0.3f;
  config.attack_ms = 1.0f;
  config.release_ms = 20.0f;
  config.polyphony = 4;
  return config;
}

SonareEngineBuiltinSynthConfig engine_synth() {
  const SonareBuiltinSynthConfig p = project_synth();
  SonareEngineBuiltinSynthConfig config{};
  config.waveform = p.waveform;
  config.gain = p.gain;
  config.attack_ms = p.attack_ms;
  config.decay_ms = p.decay_ms;
  config.sustain = p.sustain;
  config.release_ms = p.release_ms;
  config.polyphony = p.polyphony;
  return config;
}

struct TestProject {
  SonareProject* project = nullptr;
  uint32_t audio_track = 0;
  uint32_t audio_clip = 0;
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
};

/// One audio clip and one MIDI clip routed to kDestination, at 120 BPM.
TestProject build_project(const std::vector<float>& tone, int project_sample_rate = kSampleRate) {
  TestProject built;
  REQUIRE(sonare_project_create(&built.project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(built.project, project_sample_rate) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  REQUIRE(sonare_project_add_track(built.project, &track_desc, &built.audio_track) == SONARE_OK);
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = built.audio_track;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 1.5;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = tone.data();
  clip_desc.audio_frames = static_cast<int64_t>(tone.size() / 2);
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = kSampleRate;
  REQUIRE(sonare_project_add_clip(built.project, &clip_desc, &built.audio_clip) == SONARE_OK);

  REQUIRE(sonare_project_add_midi_clip(built.project, 0.0, 2.0, &built.midi_track,
                                       &built.midi_clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {
      {0.25, 0x20903C60u, 0u},  // note-on, note 60
      {1.0, 0x20803C00u, 0u},   // note-off, note 60
      {1.0, 0x20904060u, 0u},   // note-on, note 64
      {1.75, 0x20804000u, 0u},  // note-off, note 64
  };
  REQUIRE(sonare_project_set_midi_events(built.project, built.midi_clip, events,
                                         std::size(events)) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(built.project, built.midi_track,
                                                    kDestination) == SONARE_OK);
  return built;
}

std::vector<float> project_bounce(SonareProject* project) {
  SonareProjectBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = kBlockSize;
  options.num_channels = 2;
  options.sample_rate = kSampleRate;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config = project_synth();
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out_len == static_cast<size_t>(kFrames) * 2);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  return result;
}

SonareRealtimeEngine* prepared_engine() {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlockSize, 64, 16) == SONARE_OK);
  return engine;
}

/// Renders one block so queued transport commands are adopted.
void process_block(SonareRealtimeEngine* engine) {
  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, kBlockSize) == SONARE_OK);
}

bool transport_playing(SonareRealtimeEngine* engine) {
  SonareTransportState state{};
  REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
  return state.playing != 0;
}

/// Stops the transport and rewinds it to the timeline start.
void stop_and_rewind(SonareRealtimeEngine* engine) {
  REQUIRE(sonare_engine_stop(engine, -1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  process_block(engine);
  REQUIRE_FALSE(transport_playing(engine));
}

/// Plays from the current position and bounces kFrames, then leaves the engine
/// stopped at the start again.
std::vector<float> engine_bounce(SonareRealtimeEngine* engine) {
  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = kFrames;
  options.block_size = kBlockSize;
  options.num_channels = 2;
  options.source_sample_rate = kSampleRate;
  options.target_sample_rate = kSampleRate;
  options.normalize_lufs = 0;
  options.dither = 0;
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.sample_count == static_cast<size_t>(kFrames) * 2);
  std::vector<float> interleaved(result.interleaved, result.interleaved + result.sample_count);
  sonare_free_bounce_result(&result);
  stop_and_rewind(engine);
  return interleaved;
}

/// Compiles @p project into a timeline handle, requiring success.
SonareProjectTimeline* compile_timeline(SonareProject* project) {
  SonareProjectCompileResult result{};
  SonareProjectTimeline* timeline = nullptr;
  REQUIRE(sonare_project_compile_timeline(project, &result, &timeline) == SONARE_OK);
  REQUIRE(result.has_timeline != 0);
  REQUIRE(timeline != nullptr);
  sonare_project_free_compile_result(&result);
  return timeline;
}

/// Compiles, applies and destroys the handle; a fresh instrument is bound first so
/// voice state from an earlier render cannot leak into the next.
void apply_project(SonareRealtimeEngine* engine, SonareProject* project) {
  const SonareEngineBuiltinSynthConfig synth = engine_synth();
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);
  SonareProjectTimeline* timeline = compile_timeline(project);
  REQUIRE(sonare_engine_apply_project_timeline(engine, timeline) == SONARE_OK);
  sonare_project_timeline_destroy(timeline);
}

float peak(const std::vector<float>& data) {
  float value = 0.0f;
  for (float sample : data) value = std::max(value, std::abs(sample));
  return value;
}

size_t marker_count(SonareRealtimeEngine* engine) {
  size_t count = 0;
  REQUIRE(sonare_engine_marker_count(engine, &count) == SONARE_OK);
  return count;
}

size_t clip_count(SonareRealtimeEngine* engine) {
  size_t count = 0;
  REQUIRE(sonare_engine_clip_count(engine, &count) == SONARE_OK);
  return count;
}

[[maybe_unused]] size_t automation_lane_count(SonareRealtimeEngine* engine) {
  size_t count = 0;
  REQUIRE(sonare_engine_automation_lane_count(engine, &count) == SONARE_OK);
  return count;
}

}  // namespace

TEST_CASE("applied timeline renders bit-identically to project bounce across edits",
          "[timeline-apply][c_api]") {
  const std::vector<float> tone = stereo_tone(kSampleRate);
  TestProject built = build_project(tone);
  SonareRealtimeEngine* engine = prepared_engine();

  const auto require_match = [&](const char* step) {
    INFO("after: " << step);
    apply_project(engine, built.project);
    const std::vector<float> live = engine_bounce(engine);
    const std::vector<float> bounced = project_bounce(built.project);
    REQUIRE(peak(bounced) > 0.01f);
    REQUIRE(live == bounced);
    return bounced;
  };

  const std::vector<float> initial = require_match("initial compile");

  REQUIRE(sonare_project_move_clip(built.project, built.audio_clip, 0.5, 0) == SONARE_OK);
  const std::vector<float> moved = require_match("move clip");
  REQUIRE(moved != initial);

  REQUIRE(sonare_project_trim_clip(built.project, built.audio_clip, 0.75, 0.5) == SONARE_OK);
  const std::vector<float> trimmed = require_match("trim clip");
  REQUIRE(trimmed != moved);

  const SonareProjectTempoSegment slower[] = {{0.0, 90.0, 0.0, 0.0}};
  REQUIRE(sonare_project_set_tempo_segments(built.project, slower, 1) == SONARE_OK);
  const std::vector<float> retimed = require_match("tempo change");
  REQUIRE(retimed != trimmed);

  REQUIRE(sonare_project_undo(built.project) == SONARE_OK);
  const std::vector<float> undone = require_match("undo");
  REQUIRE(undone == trimmed);

  sonare_engine_destroy(engine);
  sonare_project_destroy(built.project);
}

TEST_CASE(
    "C API refuses a timeline compiled for a different sample rate without changing the engine",
    "[timeline-apply][c_api]") {
  const std::vector<float> tone = stereo_tone(kSampleRate);
  TestProject base = build_project(tone);
  TestProject mismatch = build_project(tone, 44100);
  uint32_t marker_id = 0;
  REQUIRE(sonare_project_set_marker(base.project, 0, 2.0, "base", &marker_id) == SONARE_OK);

  SonareRealtimeEngine* engine = prepared_engine();
  apply_project(engine, base.project);
  REQUIRE(marker_count(engine) == 1);
  REQUIRE(clip_count(engine) == 1);

  SonareProjectTimeline* timeline = compile_timeline(mismatch.project);
  REQUIRE(sonare_engine_apply_project_timeline(engine, timeline) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_project_timeline_destroy(timeline);

  REQUIRE(marker_count(engine) == 1);
  REQUIRE(clip_count(engine) == 1);
  sonare_engine_destroy(engine);
  sonare_project_destroy(mismatch.project);
  sonare_project_destroy(base.project);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("applied timeline binds a 1:1 scene strip like the project bounce mixes it",
          "[timeline-apply][c_api]") {
  // Same agreement bound as tests/engine/bus_strip_agreement_test.cpp:
  // max|a-b| <= scale * max(1, max|a|), after the synth attack has settled.
  constexpr double kMetricScale = 1.0e-6;
  constexpr size_t kSettleFrames = kBlockSize * 20;

  const std::vector<float> tone = stereo_tone(kSampleRate);
  TestProject built = build_project(tone);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"lead",)"
      R"("faderDb":-4.0,"pan":0.4,"eq":{"enabled":true,"bands":[{"type":"Peak",)"
      R"("frequencyHz":1000.0,"gainDb":6.0,"q":0.7,"enabled":true}]}}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(built.project, scene) == SONARE_OK);
  const std::vector<float> unrouted = project_bounce(built.project);
  REQUIRE(sonare_project_set_track_route(built.project, built.midi_track, "lead", "") == SONARE_OK);
  const std::vector<float> bounced = project_bounce(built.project);

  SonareRealtimeEngine* engine = prepared_engine();
  apply_project(engine, built.project);
  const std::vector<float> live = engine_bounce(engine);
  sonare_engine_destroy(engine);
  sonare_project_destroy(built.project);

  REQUIRE(live.size() == bounced.size());
  double max_diff = 0.0;
  double max_ref = 0.0;
  double strip_effect = 0.0;
  for (size_t i = kSettleFrames * 2; i < bounced.size(); ++i) {
    max_diff = std::max(max_diff, static_cast<double>(std::abs(bounced[i] - live[i])));
    max_ref = std::max(max_ref, static_cast<double>(std::abs(bounced[i])));
    strip_effect = std::max(strip_effect, static_cast<double>(std::abs(bounced[i] - unrouted[i])));
  }
  const double bound = kMetricScale * std::max(1.0, max_ref);
  INFO("live vs bounce: max_diff=" << max_diff << " bound=" << bound
                                   << " strip effect=" << strip_effect);
  // The strip has to be audible for the agreement to say anything about binding it.
  REQUIRE(strip_effect > 100.0 * bound);
  REQUIRE(max_diff <= bound);
}
#endif

TEST_CASE("marker names outlive a destroyed timeline handle", "[timeline-apply][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  uint32_t marker_id = 0;
  REQUIRE(sonare_project_set_marker(project, 0, 2.0, kLongMarkerName, &marker_id) == SONARE_OK);

  SonareRealtimeEngine* engine = prepared_engine();
  SonareProjectTimeline* timeline = compile_timeline(project);
  REQUIRE(sonare_engine_apply_project_timeline(engine, timeline) == SONARE_OK);
  sonare_project_timeline_destroy(timeline);
  sonare_project_destroy(project);

  // Churn the allocator with another timeline's lifetime before reading back.
  SonareProject* other = nullptr;
  REQUIRE(sonare_project_create(&other) == SONARE_OK);
  uint32_t other_marker = 0;
  REQUIRE(sonare_project_set_marker(other, 0, 1.0, "another-marker-name-past-sso", &other_marker) ==
          SONARE_OK);
  sonare_project_timeline_destroy(compile_timeline(other));
  sonare_project_destroy(other);

  REQUIRE(marker_count(engine) == 1);
  SonareEngineMarker marker{};
  REQUIRE(sonare_engine_marker_by_index(engine, 0, &marker) == SONARE_OK);
  REQUIRE(std::string(marker.name) == kLongMarkerName);
  REQUIRE(marker.ppq == 2.0);
  SonareEngineMarker by_id{};
  REQUIRE(sonare_engine_marker(engine, marker_id, &by_id) == SONARE_OK);
  REQUIRE(std::string(by_id.name) == kLongMarkerName);

  // A later low-level marker edit replaces the applied markers cleanly.
  SonareEngineMarker manual{};
  manual.id = 7;
  manual.ppq = 3.0;
  std::snprintf(manual.name, sizeof(manual.name), "%s", "manual");
  REQUIRE(sonare_engine_set_markers(engine, &manual, 1) == SONARE_OK);
  REQUIRE(sonare_engine_marker_by_index(engine, 0, &marker) == SONARE_OK);
  REQUIRE(std::string(marker.name) == "manual");
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("automation set after apply keeps the applied lanes audible", "[timeline-apply][c_api]") {
  const std::vector<float> tone = stereo_tone(kSampleRate);
  TestProject built = build_project(tone);
  // A ramp, so a lost lane is audible: the fader would hold its value at the moment of loss.
  const SonareAutomationPoint ramp[] = {{0.0, 0.0f, SONARE_CURVE_LINEAR},
                                        {1.5, -24.0f, SONARE_CURVE_LINEAR}};
  SonareAutomationLaneDescEx fader{};
  fader.target_param_id = 1;
  fader.target_kind = SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
  fader.points = ramp;
  fader.point_count = std::size(ramp);
  REQUIRE(sonare_project_add_automation_lane_ex(built.project, built.audio_track, &fader,
                                                nullptr) == SONARE_OK);
  const std::vector<float> bounced = project_bounce(built.project);

  SonareRealtimeEngine* engine = prepared_engine();
  apply_project(engine, built.project);
  REQUIRE(automation_lane_count(engine) == 1);
  const std::vector<float> applied = engine_bounce(engine);
  REQUIRE(applied == bounced);

  const SonareAutomationPoint other_points[] = {{0.0, 0.5f, SONARE_CURVE_LINEAR},
                                                {4.0, 0.75f, SONARE_CURVE_LINEAR}};
  REQUIRE(sonare_engine_set_automation_lane(engine, kOtherAutomationTarget, other_points,
                                            std::size(other_points)) == SONARE_OK);
  REQUIRE(automation_lane_count(engine) == 2);
  REQUIRE(engine_bounce(engine) == applied);

  // The fader lane is audible: the same project without it renders differently.
  REQUIRE(sonare_project_undo(built.project) == SONARE_OK);
  const std::vector<float> without_lane = project_bounce(built.project);
  REQUIRE(without_lane != applied);
  // Leave the fader at the ramp's end value, then rewind without rendering.
  REQUIRE(sonare_engine_seek_sample(engine, kSampleRate, -1) == SONARE_OK);
  process_block(engine);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  // Re-applying replaces the whole lane set, the setter's lane included, and the
  // fader the removed lane drove returns to its static value.
  apply_project(engine, built.project);
  REQUIRE(automation_lane_count(engine) == 0);
  REQUIRE(engine_bounce(engine) == without_lane);

  sonare_engine_destroy(engine);
  sonare_project_destroy(built.project);
}
#endif

TEST_CASE("apply is refused while the transport is playing", "[timeline-apply][c_api]") {
  const std::vector<float> tone = stereo_tone(kSampleRate);
  TestProject first = build_project(tone);
  TestProject second = build_project(tone);
  REQUIRE(sonare_project_move_clip(second.project, second.audio_clip, 1.0, 0) == SONARE_OK);
  uint32_t second_marker = 0;
  REQUIRE(sonare_project_set_marker(second.project, 0, 1.0, "second", &second_marker) == SONARE_OK);

  const auto run = [&](bool attempt_while_playing) {
    SonareRealtimeEngine* engine = prepared_engine();
    apply_project(engine, first.project);
    REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
    process_block(engine);
    REQUIRE(transport_playing(engine));
    if (attempt_while_playing) {
      SonareProjectTimeline* timeline = compile_timeline(second.project);
      REQUIRE(sonare_engine_apply_project_timeline(engine, timeline) == SONARE_ERROR_INVALID_STATE);
      sonare_project_timeline_destroy(timeline);
      REQUIRE(marker_count(engine) == 0);
      REQUIRE(clip_count(engine) == 1);
    }
    stop_and_rewind(engine);
    std::vector<float> rendered = engine_bounce(engine);
    sonare_engine_destroy(engine);
    return rendered;
  };

  const std::vector<float> refused = run(true);
  REQUIRE(peak(refused) > 0.01f);
  REQUIRE(refused == run(false));

  sonare_project_destroy(first.project);
  sonare_project_destroy(second.project);
}

TEST_CASE("compile_timeline and apply validate their arguments", "[timeline-apply][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectCompileResult result{};
  result.has_timeline = 7;
  auto* const sentinel = reinterpret_cast<SonareProjectTimeline*>(&result);
  SonareProjectTimeline* timeline = sentinel;
  REQUIRE(sonare_project_compile_timeline(nullptr, &result, &timeline) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(timeline == nullptr);
  REQUIRE(result.has_timeline == 0);
  REQUIRE(result.diagnostics == nullptr);

  timeline = sentinel;
  REQUIRE(sonare_project_compile_timeline(project, nullptr, &timeline) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(timeline == nullptr);

  result.has_timeline = 7;
  REQUIRE(sonare_project_compile_timeline(project, &result, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.has_timeline == 0);

  SonareRealtimeEngine* engine = prepared_engine();
  timeline = compile_timeline(project);
  REQUIRE(sonare_engine_apply_project_timeline(nullptr, timeline) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_apply_project_timeline(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_apply_project_timeline(engine, timeline) == SONARE_OK);
  sonare_project_timeline_destroy(timeline);
  sonare_project_timeline_destroy(nullptr);

  sonare_engine_destroy(engine);
  sonare_project_destroy(project);
}

TEST_CASE("compile_timeline returns no handle and the diagnostics when compile fails",
          "[timeline-apply][c_api]") {
  // A clip pointing at a source id the project does not hold is a compile error.
  const std::string json =
      R"({"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,)"
      R"("channel_strip_ref":"","output_target":"","midi_destination_id":0,)"
      R"("automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,)"
      R"("start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,)"
      R"("fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},)"
      R"("loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0}]})";
  SonareProject* project = nullptr;
  char* warnings = nullptr;
  REQUIRE(sonare_project_deserialize(json.c_str(), json.size(), &project, &warnings) == SONARE_OK);
  sonare_free_string(warnings);

  SonareProjectCompileResult expected{};
  REQUIRE(sonare_project_compile(project, &expected) == SONARE_OK);
  REQUIRE(expected.has_timeline == 0);
  REQUIRE(expected.diagnostic_count > 0);

  SonareProjectCompileResult result{};
  auto* const sentinel = reinterpret_cast<SonareProjectTimeline*>(&result);
  SonareProjectTimeline* timeline = sentinel;
  REQUIRE(sonare_project_compile_timeline(project, &result, &timeline) == SONARE_OK);
  REQUIRE(timeline == nullptr);
  REQUIRE(result.has_timeline == 0);
  REQUIRE(result.diagnostic_count == expected.diagnostic_count);
  for (size_t i = 0; i < result.diagnostic_count; ++i) {
    REQUIRE(result.diagnostics[i].code == expected.diagnostics[i].code);
    REQUIRE(result.diagnostics[i].severity == expected.diagnostics[i].severity);
    REQUIRE(result.diagnostics[i].target_id == expected.diagnostics[i].target_id);
  }
  REQUIRE(std::string(result.messages) == std::string(expected.messages));

  sonare_project_free_compile_result(&result);
  sonare_project_free_compile_result(&expected);
  sonare_project_destroy(project);
}
