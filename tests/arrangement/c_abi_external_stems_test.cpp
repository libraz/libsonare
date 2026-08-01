// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

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

}  // namespace

TEST_CASE("C ABI imports externally separated planar stems atomically",
          "[arrangement][external_stems][c_api]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  StemBuffers vocals(31);
  StemBuffers drums(31);
  const SonareExternalStemDesc stems[] = {
      {"vocals", "lead", SONARE_EXTERNAL_STEM_STEREO, vocals.planes.data(), 31, 113},
      {"drums", nullptr, SONARE_EXTERNAL_STEM_STEREO, drums.planes.data(), 31, 113},
  };
  const SonareExternalStemImportRequest request{0, 48000, stems, 2};
  SonareExternalStemImportResult result{};
  REQUIRE(sonare_project_import_external_stems(project, &request, &result) == SONARE_OK);
  REQUIRE(result.count == 2);
  REQUIRE(result.track_ids[0] != 0);
  REQUIRE(result.track_ids[1] != 0);
  REQUIRE(result.clip_ids[0] != 0);
  REQUIRE(result.clip_ids[1] != 0);
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

  const SonareExternalStemDesc invalid[] = {
      {"duplicate", nullptr, SONARE_EXTERNAL_STEM_STEREO, vocals.planes.data(), 31, 0},
      {"duplicate", nullptr, SONARE_EXTERNAL_STEM_STEREO, drums.planes.data(), 31, 0},
  };
  const SonareExternalStemImportRequest invalid_request{0, 48000, invalid, 2};
  REQUIRE(sonare_project_import_external_stems(project, &invalid_request, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_track_count(project, &tracks) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clips) == SONARE_OK);
  REQUIRE(tracks == 2);
  REQUIRE(clips == 2);

  sonare_project_destroy(project);
}
