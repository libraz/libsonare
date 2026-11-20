/// @file c_entry_refusal_names_test.cpp
/// @brief Project clip-edit and engine capture/render entries name the refused argument.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

#if defined(SONARE_WITH_ARRANGEMENT)

/// A project holding one audio track and one metadata-only audio clip.
struct ClipProject {
  SonareProject* project = nullptr;
  uint32_t track_id = 0;
  uint32_t clip_id = 0;

  ClipProject() {
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    SonareProjectTrackDesc track{};
    track.kind = SONARE_TRACK_AUDIO;
    track.name = "audio";
    REQUIRE(sonare_project_add_track(project, &track, &track_id) == SONARE_OK);
    SonareProjectClipDesc clip{};
    clip.track_id = track_id;
    clip.length_ppq = 4.0;
    clip.gain = 1.0f;
    REQUIRE(sonare_project_add_clip(project, &clip, &clip_id) == SONARE_OK);
  }

  ~ClipProject() { sonare_project_destroy(project); }

  ClipProject(const ClipProject&) = delete;
  ClipProject& operator=(const ClipProject&) = delete;
};

#endif  // defined(SONARE_WITH_ARRANGEMENT)

/// A prepared engine, released on scope exit.
struct PreparedEngine {
  SonareEngine* engine = nullptr;

  PreparedEngine() {
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 8, 8) == SONARE_OK);
  }

  ~PreparedEngine() { sonare_engine_destroy(engine); }

  PreparedEngine(const PreparedEngine&) = delete;
  PreparedEngine& operator=(const PreparedEngine&) = delete;
};

}  // namespace

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("a source id of 0 is refused by name on the three source entries", "[c_api][project]") {
  ClipProject built;
  const float sample = 0.0f;

  CHECK(sonare_project_set_clip_source(built.project, built.clip_id, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must not be 0");

  CHECK(sonare_project_set_source_audio(built.project, 0, &sample, 1, 1, 48000) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must not be 0");

  CHECK(sonare_project_set_audio_source_metadata(built.project, 0, "hash", "role") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must not be 0");
}

TEST_CASE("an unknown source or clip id is refused as an id of this project", "[c_api][project]") {
  ClipProject built;
  const float sample = 0.0f;

  CHECK(sonare_project_set_clip_source(built.project, built.clip_id, 987654) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must be the id of a source in this project");

  CHECK(sonare_project_set_source_audio(built.project, 987654, &sample, 1, 1, 48000) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "source_id must be the id of a source in this project");

  CHECK(sonare_project_set_clip_source(built.project, 424242, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip_id must be the id of a clip in this project");
}

TEST_CASE("an unknown take id is refused as an id of its clip or takes", "[c_api][project]") {
  ClipProject built;

  SonareProjectClipCompSegment segment{};
  segment.start_ppq = 0.0;
  segment.end_ppq = 1.0;
  segment.take_id = 77;
  CHECK(sonare_project_set_clip_comp_segments(built.project, built.clip_id, &segment, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "segments[0].take_id must be the id of a take in this clip");

  SonareProjectClipTake take{};
  take.id = 1;
  CHECK(sonare_project_set_clip_takes(built.project, built.clip_id, &take, 1, 55) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "active_take_id must be the id of a take in takes");
}

TEST_CASE("a NULL project is refused by its parameter name on a clip edit", "[c_api][project]") {
  uint32_t out_clip_id = 0;
  CHECK(sonare_project_split_clip(nullptr, 1, 1.0, &out_clip_id) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "project must not be NULL");
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("a bounce with no channels or no frames is refused by name", "[c_api][engine]") {
  PreparedEngine prepared;
  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 256;
  options.num_channels = 2;

  SonareEngineBounceResult result{};
  options.num_channels = 0;
  CHECK(sonare_engine_bounce_offline(prepared.engine, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::Equals("options.num_channels must be an integer in [1, 2147483647]"));

  options.num_channels = 2;
  options.total_frames = 0;
  CHECK(sonare_engine_bounce_offline(prepared.engine, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::Equals("options.total_frames must be an integer of at least 1"));
  CHECK(result.interleaved == nullptr);
}

TEST_CASE("engine capture and render entries refuse a NULL engine by name", "[c_api][engine]") {
  CHECK(sonare_engine_arm_capture(nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::Equals("engine must not be NULL"));

  CHECK(sonare_engine_finish_offline_render(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::Equals("engine must not be NULL"));
}

TEST_CASE("an enabled empty capture punch window is refused by name", "[c_api][engine]") {
  PreparedEngine prepared;
  CHECK(sonare_engine_set_capture_punch(prepared.engine, 100, 100, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::Equals(
                                 "end_sample must not equal start_sample when enabled is not 0"));
}
