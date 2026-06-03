/// @file binding_project_editing_test.cpp
/// @brief Project editing and compile parity tests.

#include "binding_project_parity_test_helpers.h"

TEST_CASE("undo restores the serialized bytes through the C surface", "[project]") {
  const std::vector<float> audio = make_stereo_sine(4800);
  BuiltProject built = build_project(audio);

  const std::string before = serialize(built.project);

  // An edit that mutates the model, then undo it.
  uint32_t new_clip = 0;
  REQUIRE(sonare_project_split_clip(built.project, built.audio_clip, 1.0, &new_clip) == SONARE_OK);
  const std::string after = serialize(built.project);
  REQUIRE(after != before);

  REQUIRE(sonare_project_undo(built.project) == SONARE_OK);
  const std::string restored = serialize(built.project);
  REQUIRE(restored == before);

  // Redo re-applies.
  REQUIRE(sonare_project_redo(built.project) == SONARE_OK);
  REQUIRE(serialize(built.project) == after);

  sonare_project_destroy(built.project);
}

TEST_CASE("compile surfaces a renderable timeline for a valid project", "[project]") {
  const std::vector<float> audio = make_stereo_sine(4800);
  BuiltProject built = build_project(audio);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(built.project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline == 1);
  sonare_project_free_compile_result(&result);
  REQUIRE(result.diagnostics == nullptr);
  REQUIRE(result.messages == nullptr);

  sonare_project_destroy(built.project);
}

TEST_CASE("malformed deserialize returns an error without crashing", "[project]") {
  const std::string garbage = "{ this is not valid project json ]]";
  SonareProject* project = reinterpret_cast<SonareProject*>(0x1);  // sentinel
  char* diag = nullptr;
  const SonareError err =
      sonare_project_deserialize(garbage.data(), garbage.size(), &project, &diag);
  REQUIRE(err != SONARE_OK);
  REQUIRE(project == nullptr);
  if (diag) sonare_free_string(diag);

  // Truncated empty buffer is also handled.
  SonareProject* empty = reinterpret_cast<SonareProject*>(0x1);
  REQUIRE(sonare_project_deserialize("", 0, &empty, nullptr) != SONARE_OK);
  REQUIRE(empty == nullptr);
}

TEST_CASE("snap_to_grid snaps a near-beat coordinate to the beat line", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  double snapped = -1.0;
  REQUIRE(sonare_project_snap_to_grid(project, 1.02, 1.0, &snapped) == SONARE_OK);
  REQUIRE(snapped == 1.0);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface composite edits roll back on failure", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);

  SonareProjectClipDesc first{};
  first.track_id = track;
  first.start_ppq = 0.0;
  first.length_ppq = 4.0;
  first.source_uri = "asset://a";
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &first, &clip) == SONARE_OK);
  const std::string before_overlap = serialize(project);

  SonareProjectClipDesc overlapping{};
  overlapping.track_id = track;
  overlapping.start_ppq = 2.0;
  overlapping.length_ppq = 1.0;
  overlapping.source_uri = "asset://b";
  uint32_t failed_clip = 123;
  REQUIRE(sonare_project_add_clip(project, &overlapping, &failed_clip) != SONARE_OK);
  REQUIRE(failed_clip == 0);
  REQUIRE(serialize(project) == before_overlap);

  const std::string before_midi = serialize(project);
  uint32_t failed_track = 123;
  failed_clip = 123;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, -1.0, &failed_track, &failed_clip) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(failed_track == 0);
  REQUIRE(failed_clip == 0);
  REQUIRE(serialize(project) == before_midi);

  sonare_project_destroy(project);
}
