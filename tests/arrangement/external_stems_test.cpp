// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "arrangement/external_stems.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "serialize/project_serializer.h"
#include "transport/tempo_map.h"

namespace {

using sonare::arrangement::AudioContentStore;
using sonare::arrangement::ExternalSeparatedStem;
using sonare::arrangement::ExternalSeparatedStemImportError;
using sonare::arrangement::ExternalSeparatedStemLayout;
using sonare::arrangement::ExternalSeparatedStemSet;
using sonare::arrangement::Project;

ExternalSeparatedStem make_stem(
    std::string name, int64_t start_frame, size_t frames,
    ExternalSeparatedStemLayout layout = ExternalSeparatedStemLayout::kStereo) {
  ExternalSeparatedStem stem;
  stem.name = std::move(name);
  stem.layout = layout;
  stem.start_frame = start_frame;
  const size_t channels = layout == ExternalSeparatedStemLayout::kMono ? 1 : 2;
  stem.planar_samples.assign(channels, std::vector<float>(frames, 0.0f));
  stem.planar_samples.front().front() = 1.0f;
  if (channels == 2) stem.planar_samples[1].front() = -0.5f;
  return stem;
}

ExternalSeparatedStemSet valid_stems() {
  ExternalSeparatedStemSet input;
  input.sample_rate = 48000;
  input.stems.push_back(make_stem("vocals", 113, 31));
  input.stems.back().role = "lead";
  input.stems.push_back(make_stem("ドラム", 113, 31));
  return input;
}

void require_unchanged(const Project& project, const AudioContentStore& audio, size_t tracks,
                       size_t clips, size_t sources, size_t sample_sources) {
  REQUIRE(project.tracks().size() == tracks);
  REQUIRE(project.clips().size() == clips);
  REQUIRE(project.sources().size() == sources);
  REQUIRE(audio.sources.size() == sample_sources);
}

}  // namespace

TEST_CASE("external stem frame conversion round-trips at constant and ramped tempo",
          "[arrangement][external_stems]") {
  for (const std::vector<sonare::transport::TempoSegment>& segments :
       {std::vector<sonare::transport::TempoSegment>{{0.0, 120.0, 0.0}},
        std::vector<sonare::transport::TempoSegment>{{0.0, 96.0, 0.0, 180.0}, {8.0, 180.0, 0.0}}}) {
    sonare::transport::TempoMap map;
    map.prepare(48000.0);
    map.set_segments(segments);
    for (int64_t frame : {int64_t{0}, int64_t{1}, int64_t{113}, int64_t{48000}, int64_t{143999},
                          int64_t{207331}, int64_t{1000000}}) {
      REQUIRE(map.ppq_to_sample(map.sample_to_ppq(frame)) == frame);
    }
  }
}

TEST_CASE("external stems import ordinary tracks, clips, and copied PCM",
          "[arrangement][external_stems]") {
  Project project;
  project.set_sample_rate(48000.0);
  AudioContentStore audio;
  const ExternalSeparatedStemSet input = valid_stems();

  const auto result = sonare::arrangement::import_external_separated_stems(&project, &audio, input);
  REQUIRE(result.ok());
  REQUIRE(result.track_ids.size() == 2);
  REQUIRE(result.clip_ids.size() == 2);
  REQUIRE(project.tracks().size() == 2);
  REQUIRE(project.clips().size() == 2);
  REQUIRE(audio.sources.size() == 2);
  REQUIRE(project.find_track(result.track_ids[0])->name == "vocals");
  REQUIRE(project.find_track(result.track_ids[1])->name == "ドラム");

  const auto compile = sonare::arrangement::compile(project, {}, audio);
  REQUIRE(compile.timeline.has_value());
  REQUIRE(compile.timeline->audio_clips.size() == 2);
  for (const auto& schedule : compile.timeline->audio_clips) {
    REQUIRE(schedule.start_sample == 113);
    REQUIRE(schedule.length_samples == 31);
  }

  const auto* source = project.find_source(project.find_clip(result.clip_ids[0])->source_id);
  REQUIRE(source != nullptr);
  const auto* source_ref = std::get_if<sonare::arrangement::AudioSourceRef>(source);
  REQUIRE(source_ref != nullptr);
  REQUIRE(source_ref->external_stem_role == "lead");
  const auto sample_it = audio.sources.find(source_ref->id);
  REQUIRE(sample_it != audio.sources.end());
  REQUIRE(sample_it->second.channels == input.stems[0].planar_samples);

  const std::string json = sonare::serialize::project_to_json(project, {});
  const auto restored = sonare::serialize::project_from_json(json);
  REQUIRE(restored.ok());
  const auto* restored_source = restored.project->find_source(source_ref->id);
  REQUIRE(restored_source != nullptr);
  const auto* restored_source_ref =
      std::get_if<sonare::arrangement::AudioSourceRef>(restored_source);
  REQUIRE(restored_source_ref != nullptr);
  REQUIRE(restored_source_ref->external_stem_role == "lead");
}

TEST_CASE("external stem import accepts arbitrary two four and six stem sets",
          "[arrangement][external_stems]") {
  for (size_t count : {size_t{2}, size_t{4}, size_t{6}}) {
    Project project;
    project.set_sample_rate(48000.0);
    AudioContentStore audio;
    ExternalSeparatedStemSet input;
    input.sample_rate = 48000;
    for (size_t i = 0; i < count; ++i) {
      input.stems.push_back(make_stem(
          "model-output-" + std::to_string(i), static_cast<int64_t>(i * 17), 8,
          i % 2 == 0 ? ExternalSeparatedStemLayout::kMono : ExternalSeparatedStemLayout::kStereo));
    }
    const auto result =
        sonare::arrangement::import_external_separated_stems(&project, &audio, input);
    REQUIRE(result.ok());
    REQUIRE(result.track_ids.size() == count);
    REQUIRE(project.tracks().size() == count);
    REQUIRE(project.clips().size() == count);
  }
}

TEST_CASE("external stem import rejects a complete invalid set without mutation",
          "[arrangement][external_stems]") {
  const auto expect_invalid = [](ExternalSeparatedStemSet input,
                                 ExternalSeparatedStemImportError expected) {
    Project project;
    project.set_sample_rate(48000.0);
    AudioContentStore audio;
    const size_t tracks = project.tracks().size();
    const size_t clips = project.clips().size();
    const size_t sources = project.sources().size();
    const size_t sample_sources = audio.sources.size();
    const auto result =
        sonare::arrangement::import_external_separated_stems(&project, &audio, input);
    REQUIRE(result.error == expected);
    require_unchanged(project, audio, tracks, clips, sources, sample_sources);
  };

  ExternalSeparatedStemSet empty;
  empty.sample_rate = 48000;
  expect_invalid(std::move(empty), ExternalSeparatedStemImportError::kInvalidArgument);

  auto duplicate = valid_stems();
  duplicate.stems[1].name = duplicate.stems[0].name;
  expect_invalid(std::move(duplicate), ExternalSeparatedStemImportError::kInvalidArgument);

  auto empty_name = valid_stems();
  empty_name.stems[0].name.clear();
  expect_invalid(std::move(empty_name), ExternalSeparatedStemImportError::kInvalidArgument);

  auto unsupported_layout = valid_stems();
  unsupported_layout.stems[0].layout = static_cast<ExternalSeparatedStemLayout>(6);
  expect_invalid(std::move(unsupported_layout), ExternalSeparatedStemImportError::kInvalidArgument);

  auto uneven_planes = valid_stems();
  uneven_planes.stems[0].planar_samples[1].pop_back();
  expect_invalid(std::move(uneven_planes), ExternalSeparatedStemImportError::kInvalidArgument);

  auto non_finite = valid_stems();
  non_finite.stems[0].planar_samples[0][0] = std::numeric_limits<float>::infinity();
  expect_invalid(std::move(non_finite), ExternalSeparatedStemImportError::kInvalidArgument);

  auto negative_start = valid_stems();
  negative_start.stems[0].start_frame = -1;
  expect_invalid(std::move(negative_start), ExternalSeparatedStemImportError::kInvalidArgument);

  auto mismatch = valid_stems();
  mismatch.sample_rate = 44100;
  expect_invalid(std::move(mismatch), ExternalSeparatedStemImportError::kSampleRateMismatch);
}
