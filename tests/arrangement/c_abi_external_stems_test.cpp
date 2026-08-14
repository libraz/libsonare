// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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
  const SonareExternalStemImportRequest invalid_request{0, 48000, invalid, 2};
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
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  sonare::arrangement::AudioSourceSamples orphan;
  orphan.sample_rate = 48000.0;
  orphan.channels = {{0.75f}};
  project->audio.sources.emplace(1, std::move(orphan));

  StemBuffers stem(4);
  const SonareExternalStemDesc desc = {"collision",        nullptr, SONARE_EXTERNAL_STEM_STEREO,
                                       stem.planes.data(), 4,       0};
  const SonareExternalStemImportRequest request{0, 48000, &desc, 1};
  SonareExternalStemImportResult result{};
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
  REQUIRE(project->history.undo_depth() == 0);

  sonare_project_destroy(project);
}
