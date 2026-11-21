// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "c_api/project_internal.h"

namespace {

struct StemBuffers {
  std::vector<float> left;
  std::vector<float> right;
  std::array<const float*, 2> planes{};

  explicit StemBuffers(size_t frames) : left(frames, 0.0f), right(frames, 0.0f) {
    left.front() = 1.0f;
    right.front() = -0.5f;
    planes = {left.data(), right.data()};
  }
};

SonareExternalStemImportRequest make_request(int sample_rate, const SonareExternalStemDesc* stems,
                                             size_t stem_count) {
  SonareExternalStemImportRequest request;
  sonare_external_stem_import_request_init(&request);
  request.sample_rate = sample_rate;
  request.stems = stems;
  request.stem_count = stem_count;
  return request;
}

}  // namespace

TEST_CASE("C ABI imports externally separated planar stems atomically",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);

  StemBuffers vocals(31);
  StemBuffers drums(31);
  const SonareExternalStemDesc stems[] = {
      {"vocals", "lead", SONARE_EXTERNAL_STEM_STEREO, vocals.planes.data(), 31, 113},
      {"drums", nullptr, SONARE_EXTERNAL_STEM_STEREO, drums.planes.data(), 31, 113},
  };
  const SonareExternalStemImportRequest request = make_request(48000, stems, 2);
  SonareExternalStemImportResult result{};
  REQUIRE(sonare_project_import_external_stems(project, &request, &result) == SONARE_OK);
  REQUIRE(result.count == 2);
  REQUIRE(result.track_ids[0] != 0);
  REQUIRE(result.track_ids[1] != 0);
  REQUIRE(result.clip_ids[0] != 0);
  REQUIRE(result.clip_ids[1] != 0);

  const auto first_track_id = result.track_ids[0];
  const auto first_clip_id = result.clip_ids[0];
  const auto first_source_id = project->history.project().find_clip(first_clip_id)->source_id;
  REQUIRE(project->audio.sources.size() == 2);
  const auto source_node = project->audio.sources.find(first_source_id);
  REQUIRE(source_node != project->audio.sources.end());
  const auto source_node_address = reinterpret_cast<std::uintptr_t>(&source_node->second);
  const auto left_pcm_address =
      reinterpret_cast<std::uintptr_t>(source_node->second.channels.front().data());
  REQUIRE(source_node->second.channels.front().front() == 1.0f);
  REQUIRE(source_node->second.channels.back().front() == -0.5f);
  sonare_free_external_stem_import_result(&result);
  REQUIRE(result.count == 0);
  REQUIRE(result.track_ids == nullptr);
  REQUIRE(result.clip_ids == nullptr);

  size_t tracks = 0;
  size_t clips = 0;
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clips) == SONARE_OK);
  REQUIRE(tracks == 2);
  REQUIRE(clips == 2);

  // Import is one EditHistory transaction: both project structure and the
  // owned PCM registry must disappear/reappear together across undo/redo.
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clips) == SONARE_OK);
  REQUIRE(tracks == 0);
  REQUIRE(clips == 0);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clips) == SONARE_OK);
  REQUIRE(tracks == 2);
  REQUIRE(clips == 2);
  REQUIRE(project->history.project().find_track(first_track_id) != nullptr);
  REQUIRE(project->history.project().find_clip(first_clip_id) != nullptr);
  const auto redo_source_node = project->audio.sources.find(first_source_id);
  REQUIRE(redo_source_node != project->audio.sources.end());
  REQUIRE(reinterpret_cast<std::uintptr_t>(&redo_source_node->second) == source_node_address);
  REQUIRE(reinterpret_cast<std::uintptr_t>(redo_source_node->second.channels.front().data()) ==
          left_pcm_address);
  REQUIRE(redo_source_node->second.channels.front().front() == 1.0f);

  // A new import after undo is a fresh edit and must clear the old redo branch.
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_import_external_stems(project, &request, &result) == SONARE_OK);
  sonare_free_external_stem_import_result(&result);
  REQUIRE(sonare_project_redo(project) == SONARE_ERROR_INVALID_STATE);

  const SonareExternalStemDesc invalid[] = {
      {"duplicate", nullptr, SONARE_EXTERNAL_STEM_STEREO, vocals.planes.data(), 31, 0},
      {"duplicate", nullptr, SONARE_EXTERNAL_STEM_STEREO, drums.planes.data(), 31, 0},
  };
  const SonareExternalStemImportRequest invalid_request = make_request(48000, invalid, 2);
  REQUIRE(sonare_project_import_external_stems(project, &invalid_request, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clips) == SONARE_OK);
  REQUIRE(tracks == 2);
  REQUIRE(clips == 2);
  REQUIRE(project->audio.sources.size() == 2);
  for (const auto& [source_id, samples] : project->audio.sources) {
    (void)samples;
    bool referenced = false;
    for (const auto& clip : project->history.project().clips()) {
      if (clip.source_id == source_id) referenced = true;
    }
    REQUIRE(referenced);
  }

  sonare_project_destroy(project);
}

TEST_CASE("C ABI external stem import rejects source-id collisions without mutation",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);

  sonare::arrangement::AudioSourceSamples orphan;
  orphan.sample_rate = 48000.0;
  orphan.channels = {{0.75f}};
  project->audio.sources.emplace(1, std::move(orphan));

  StemBuffers stem(4);
  const SonareExternalStemDesc desc = {"collision",        nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                       stem.planes.data(), 4,       0};
  const SonareExternalStemImportRequest request = make_request(48000, &desc, 1);
  SonareExternalStemImportResult result{};
  // Depth before, not zero: every setter above is itself an undoable command.
  // What this asserts is that the REJECTED import added nothing.
  const size_t undo_depth_before = project->history.undo_depth();
  REQUIRE(sonare_project_import_external_stems(project, &request, &result) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(result.count == 0);
  REQUIRE(result.track_ids == nullptr);
  REQUIRE(result.clip_ids == nullptr);
  REQUIRE(project->history.project().tracks().empty());
  REQUIRE(project->history.project().clips().empty());
  REQUIRE(project->history.project().sources().empty());
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(project->audio.sources.find(1) != project->audio.sources.end());
  REQUIRE(project->history.undo_depth() == undo_depth_before);

  sonare_project_destroy(project);
}

TEST_CASE("C ABI external stem import refuses an argument by its header name",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  const auto message = [] { return std::string(sonare_last_error_message()); };

  StemBuffers buffers(8);
  const SonareExternalStemDesc stem = {
      "vocals", nullptr, SONARE_EXTERNAL_STEM_STEREO, buffers.planes.data(), 8, 0};
  SonareExternalStemImportRequest request = make_request(48000, &stem, 1);
  SonareExternalStemImportResult result{};

  REQUIRE(sonare_project_import_external_stems(nullptr, &request, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "project must not be NULL");
  // Two NULL pointers: the first one in header order is named.
  REQUIRE(sonare_project_import_external_stems(project, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request must not be NULL");
  REQUIRE(sonare_project_import_external_stems(project, &request, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "out must not be NULL");

  SonareExternalStemImportRequest bad = request;
  bad.struct_size = 0;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.struct_size must be set by sonare_external_stem_import_request_init");
  bad = request;
  bad.stems = nullptr;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.stems must not be NULL when request.stem_count is not 0");
  bad = request;
  bad.stem_count = 0;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.stem_count must be an integer in [1, 500000000]");

  SonareExternalStemDesc bad_stem = stem;
  bad = request;
  bad.stems = &bad_stem;
  bad_stem.name = nullptr;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.stems[0].name must not be NULL");
  bad_stem = stem;
  bad_stem.layout = 7;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.stems[0].layout must be an integer in [1, 2]");
  bad_stem = stem;
  bad_stem.frame_count = 0;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.stems[0].frame_count must be an integer of at least 1");

  bad = request;
  bad.sample_rate = 44100;
  REQUIRE(sonare_project_import_external_stems(project, &bad, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "request.sample_rate must equal the sample rate of the project");

  sonare_project_destroy(project);
}

TEST_CASE("C ABI external stem import names the stem field it refuses",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  StemBuffers a(8);
  StemBuffers b(8);
  SonareExternalStemImportResult result{};
  const auto refused = [&](const SonareExternalStemDesc* stems, size_t count, int sample_rate) {
    const SonareExternalStemImportRequest request{sizeof(SonareExternalStemImportRequest),
                                                  sample_rate, stems, count};
    REQUIRE(sonare_project_import_external_stems(project, &request, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    const char* message = sonare_last_error_message();
    return std::string(message != nullptr ? message : "");
  };

  const SonareExternalStemDesc twice[] = {
      {"same", nullptr, SONARE_EXTERNAL_STEM_STEREO, a.planes.data(), 8, 0},
      {"same", nullptr, SONARE_EXTERNAL_STEM_STEREO, b.planes.data(), 8, 0},
  };
  CHECK_THAT(refused(twice, 2, 48000),
             Catch::Matchers::ContainsSubstring(
                 "request.stems[1].name must not repeat the name of another stem"));

  const SonareExternalStemDesc early = {"early",         nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                        a.planes.data(), 8,       -1};
  CHECK_THAT(refused(&early, 1, 48000),
             Catch::Matchers::ContainsSubstring("request.stems[0].start_frame must be"));

  b.right[5] = std::numeric_limits<float>::quiet_NaN();
  const SonareExternalStemDesc nan = {"nan",           nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                      b.planes.data(), 8,       0};
  CHECK_THAT(refused(&nan, 1, 48000),
             Catch::Matchers::ContainsSubstring(
                 "request.stems[0].planar_samples[1] contains a non-finite sample at index 5"));

  const SonareExternalStemDesc bad_utf8 = {"\xff",          nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                           a.planes.data(), 8,       0};
  CHECK_THAT(refused(&bad_utf8, 1, 48000),
             Catch::Matchers::ContainsSubstring(
                 "request.stems must carry names and roles that are valid UTF-8"));

  const SonareExternalStemDesc fine = {"fine",          nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                       a.planes.data(), 8,       0};
  CHECK_THAT(refused(&fine, 1, 0),
             Catch::Matchers::ContainsSubstring("request.sample_rate must be"));

  size_t tracks = 0;
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  CHECK(tracks == 0);
  sonare_project_destroy(project);
}

TEST_CASE("C ABI external stem import reads its request through struct_size",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  StemBuffers stem(4);
  const SonareExternalStemDesc desc = {
      "stem", nullptr, SONARE_EXTERNAL_STEM_STEREO, stem.planes.data(), 4, 0};
  SonareExternalStemImportResult result{};

  SonareExternalStemImportRequest initialised;
  sonare_external_stem_import_request_init(&initialised);
  REQUIRE(initialised.struct_size == sizeof(SonareExternalStemImportRequest));

  SECTION("a request that was not initialised is refused by name") {
    SonareExternalStemImportRequest request = make_request(48000, &desc, 1);
    request.struct_size = 0;
    REQUIRE(sonare_project_import_external_stems(project, &request, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).find("request.struct_size") !=
            std::string::npos);
  }

  SECTION("a sample rate of 0 is read literally and does not match the project") {
    SonareExternalStemImportRequest request = make_request(0, &desc, 1);
    REQUIRE(sonare_project_import_external_stems(project, &request, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.count == 0);
  }

  sonare_project_destroy(project);
}
