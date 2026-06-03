/// @file c_abi_edit_ops_test.cpp
/// @brief Exercises the headless-DAW edit C-ABI wrappers (clip/track edits,
/// automation lanes, and the MoveClip wrong-kind guard) including undo/redo
/// round-trips and deep-equality (serialized bytes) after undo.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "sonare_c.h"
#include "sonare_c_project.h"

namespace {

// Serializes the project to deterministic JSON for deep-equality comparisons.
std::string serialize(const SonareProject* project) {
  char* json = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_serialize(project, &json, &len) == SONARE_OK);
  REQUIRE(json != nullptr);
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

// Small interleaved stereo buffer so audio clips carry renderable samples.
std::vector<float> make_stereo(int frames) {
  std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
  for (int i = 0; i < frames; ++i) {
    const float v = 0.1f * static_cast<float>(std::sin(0.05 * i));
    out[static_cast<size_t>(i) * 2] = v;
    out[static_cast<size_t>(i) * 2 + 1] = v;
  }
  return out;
}

// Adds an audio track carrying one audio clip with decoded samples. Returns the
// track id via out params.
struct AudioFixture {
  uint32_t track = 0;
  uint32_t clip = 0;
  std::vector<float> audio;
};

AudioFixture add_audio_track_clip(SonareProject* project, double start_ppq, double length_ppq) {
  AudioFixture fx;
  fx.audio = make_stereo(480);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  REQUIRE(sonare_project_add_track(project, &track_desc, &fx.track) == SONARE_OK);
  REQUIRE(fx.track != 0);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = fx.track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = start_ppq;
  clip_desc.length_ppq = length_ppq;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = fx.audio.data();
  clip_desc.audio_frames = static_cast<int64_t>(fx.audio.size() / 2);
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &fx.clip) == SONARE_OK);
  REQUIRE(fx.clip != 0);
  return fx;
}

}  // namespace

TEST_CASE("C-ABI remove_clip removes and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);

  const std::string before = serialize(project);

  REQUIRE(sonare_project_remove_clip(project, fx.clip) == SONARE_OK);
  REQUIRE(serialize(project) != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) != before);

  // Invalid params.
  REQUIRE(sonare_project_remove_clip(nullptr, fx.clip) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_clip(project, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_clip(project, 999999) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_gain incl. gain=0 mute and undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);

  const std::string before = serialize(project);

  // Explicit gain = 0 (muted) is accepted (add_clip coerces 0 -> 1.0; this is
  // the path that lets a caller actually mute a clip).
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, 0.0f) == SONARE_OK);
  const std::string muted = serialize(project);
  REQUIRE(muted != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // A non-zero gain round-trips through undo too.
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, 0.5f) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: negative / non-finite / unknown clip.
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, -0.1f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, std::nanf("")) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, 0, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, 999999, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_fade applies and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string before = serialize(project);

  SonareProjectClipFade fade_in{};
  fade_in.length_ppq = 0.5;
  fade_in.curve = SONARE_FADE_CURVE_EQUAL_POWER;
  SonareProjectClipFade fade_out{};
  fade_out.length_ppq = 1.0;
  fade_out.curve = SONARE_FADE_CURVE_EXPONENTIAL;

  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &fade_in, &fade_out) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Zero-length fade is valid (no fade).
  SonareProjectClipFade none{};
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &none, &none) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // Invalid: null descs, negative length, out-of-range curve, bad clip.
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, nullptr, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &fade_in, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectClipFade bad_len{};
  bad_len.length_ppq = -1.0;
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &bad_len, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectClipFade bad_curve{};
  bad_curve.curve = 99;
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &bad_curve, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_fade(project, 999999, &fade_in, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_loop applies and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 2.0) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Turning loop off with length 0 is valid.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_OFF, 0.0) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // Invalid: looping with non-positive length, bad mode, bad clip.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 0.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, 7, 2.0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_loop(project, 999999, SONARE_LOOP_MODE_LOOP, 2.0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_source rebinds and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  // Two audio clips on two tracks => two distinct audio sources.
  AudioFixture a = add_audio_track_clip(project, 0.0, 4.0);
  AudioFixture b = add_audio_track_clip(project, 0.0, 4.0);

  // Discover b's source id by rebinding a's clip to it: we can't read ids
  // directly through the C surface, but source ids are allocated monotonically;
  // clip b's source is the second registered audio source (id 2).
  const std::string before = serialize(project);
  REQUIRE(sonare_project_set_clip_source(project, a.clip, /*source_id=*/2) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: unknown source / clip, zero ids.
  REQUIRE(sonare_project_set_clip_source(project, a.clip, 999999) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, 999999, 2) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, a.clip, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, 0, 2) == SONARE_ERROR_INVALID_PARAMETER);
  (void)b;

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI duplicate_clip allocates a new id and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 2.0);
  const std::string before = serialize(project);

  uint32_t dup = 0;
  REQUIRE(sonare_project_duplicate_clip(project, fx.clip, 4.0, &dup) == SONARE_OK);
  REQUIRE(dup != 0);
  REQUIRE(dup != fx.clip);
  REQUIRE(serialize(project) != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: bad clip / start, zero id.
  uint32_t dud = 123;
  REQUIRE(sonare_project_duplicate_clip(project, 999999, 4.0, &dud) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(dud == 0);
  REQUIRE(sonare_project_duplicate_clip(project, fx.clip, -1.0, &dud) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_duplicate_clip(project, 0, 4.0, &dud) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI rename_track / remove_track with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_rename_track(project, fx.track, "lead-vox") == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // NULL name clears to empty (still a valid command).
  REQUIRE(sonare_project_rename_track(project, fx.track, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // remove_track removes track (and its clips); undo restores everything.
  REQUIRE(sonare_project_remove_track(project, fx.track) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid params.
  REQUIRE(sonare_project_rename_track(project, 0, "x") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_rename_track(project, 999999, "x") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_track(project, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_track(project, 999999) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_track_route with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_track_route(project, fx.track, "strip:vox", "bus:main") == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Empty / NULL args clear the route (still a valid command, no-op here).
  REQUIRE(sonare_project_set_track_route(project, fx.track, "", nullptr) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid track.
  REQUIRE(sonare_project_set_track_route(project, 0, "a", "b") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_route(project, 999999, "a", "b") ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI automation lane add / edit / remove with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string empty = serialize(project);

  SonareAutomationPoint points[3];
  points[0].ppq = 0.0;
  points[0].value = 0.0f;
  points[0].curve_to_next = SONARE_CURVE_LINEAR;
  points[1].ppq = 4.0;
  points[1].value = 1.0f;
  points[1].curve_to_next = SONARE_CURVE_SCURVE;
  points[2].ppq = 8.0;
  points[2].value = 0.5f;
  points[2].curve_to_next = SONARE_CURVE_HOLD;

  SonareAutomationLaneDesc desc{};
  desc.target_param_id = 42;
  desc.points = points;
  desc.point_count = 3;

  size_t lane_index = 999;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &desc, &lane_index) == SONARE_OK);
  REQUIRE(lane_index == 0);
  const std::string with_lane = serialize(project);
  REQUIRE(with_lane != empty);

  // Edit the lane in place (different target + points).
  SonareAutomationPoint edited[2];
  edited[0].ppq = 0.0;
  edited[0].value = 0.25f;
  edited[0].curve_to_next = SONARE_CURVE_EXPONENTIAL;
  edited[1].ppq = 8.0;
  edited[1].value = 0.75f;
  edited[1].curve_to_next = SONARE_CURVE_LINEAR;
  SonareAutomationLaneDesc edit_desc{};
  edit_desc.target_param_id = 7;
  edit_desc.points = edited;
  edit_desc.point_count = 2;
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 0, &edit_desc) == SONARE_OK);
  REQUIRE(serialize(project) != with_lane);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_lane);

  // Remove the lane; undo restores it.
  REQUIRE(sonare_project_remove_automation_lane(project, fx.track, 0) == SONARE_OK);
  REQUIRE(serialize(project) == empty);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_lane);

  // A lane with zero breakpoints is valid (empty desc.points / count 0).
  SonareAutomationLaneDesc empty_lane{};
  empty_lane.target_param_id = 1;
  size_t empty_index = 999;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &empty_lane, &empty_index) ==
          SONARE_OK);
  REQUIRE(empty_index == 1);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // Invalid: bad track, bad lane index, null desc, bad point fields.
  REQUIRE(sonare_project_add_automation_lane(project, 999999, &desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 99, &desc) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_automation_lane(project, fx.track, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareAutomationPoint bad[1];
  bad[0].ppq = -1.0;
  bad[0].value = 0.0f;
  bad[0].curve_to_next = SONARE_CURVE_LINEAR;
  SonareAutomationLaneDesc bad_desc{};
  bad_desc.points = bad;
  bad_desc.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &bad_desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  bad[0].ppq = 0.0;
  bad[0].curve_to_next = 99;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &bad_desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI move_clip rejects a cross-kind move without mutating state",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  // Audio track + audio clip.
  AudioFixture fx = add_audio_track_clip(project, 0.0, 2.0);

  // A MIDI track (kind mismatch target for the audio clip).
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &midi_track, &midi_clip) == SONARE_OK);
  REQUIRE(midi_track != 0);

  const std::string before = serialize(project);

  // Moving the audio clip onto the MIDI track must fail cleanly and leave the
  // project unchanged (so it still compiles).
  REQUIRE(sonare_project_move_clip(project, fx.clip, 0.0, midi_track) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == before);

  // Likewise the MIDI clip cannot move onto the audio track.
  REQUIRE(sonare_project_move_clip(project, midi_clip, 0.0, fx.track) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == before);

  // A same-track move (new_track_id == 0) still works and round-trips.
  REQUIRE(sonare_project_move_clip(project, fx.clip, 1.0, 0) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // The project still compiles after the rejected cross-kind moves.
  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline == 1);
  sonare_project_free_compile_result(&result);

  sonare_project_destroy(project);
}
