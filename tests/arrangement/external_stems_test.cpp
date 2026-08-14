// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "arrangement/external_stems.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
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

// Models the C ABI external-stem command's sidecar ownership, then injects an
// inverse-allocation failure after apply() has moved a PCM node and changed the
// Project.  The history rollback seam must restore the node without allocating;
// the value Project snapshot then restores counters and structure.
class BadAllocAfterExternalMutation final : public sonare::arrangement::EditCommand,
                                            public sonare::arrangement::EditCommandRollback {
 public:
  BadAllocAfterExternalMutation(AudioContentStore* audio, sonare::arrangement::SourceId id,
                                sonare::arrangement::AudioSourceSamples samples)
      : audio_(audio), id_(id), samples_(std::move(samples)) {}

  bool apply(Project& project, sonare::arrangement::MidiContentStore&) override {
    if (audio_ == nullptr || applied_ || !audio_->sources.emplace(id_, samples_).second) {
      return false;
    }
    applied_ = true;
    sonare::arrangement::Track track;
    track.name = "fault-injected";
    if (project.add_track(std::move(track)) == 0) {
      audio_->sources.erase(id_);
      applied_ = false;
      return false;
    }
    return true;
  }

  sonare::arrangement::EditCommandPtr invert(
      const Project&, const sonare::arrangement::MidiContentStore&) const override {
    throw std::bad_alloc();
  }

  void rollback_apply(Project&, sonare::arrangement::MidiContentStore&) noexcept override {
    if (!applied_ || audio_ == nullptr) return;
    audio_->sources.erase(id_);
    applied_ = false;
  }

  const char* type_name() const noexcept override { return "BadAllocAfterExternalMutation"; }
  size_t retained_bytes() const noexcept override {
    return sonare::arrangement::retained::saturating_add(
        sizeof(*this), sonare::arrangement::retained::dynamic_bytes(samples_.channels));
  }

 private:
  AudioContentStore* audio_ = nullptr;
  sonare::arrangement::SourceId id_ = 0;
  sonare::arrangement::AudioSourceSamples samples_;
  bool applied_ = false;
};

// A redo must keep its forward command in the redo deque until invert() has
// succeeded.  Otherwise a throwing invert can leave a null forward pointer in
// the deque and make the failed redo impossible to retry.
class ThrowingRedoInvertCommand final : public sonare::arrangement::EditCommand {
 public:
  ThrowingRedoInvertCommand(sonare::arrangement::TrackId track_id, std::string replacement)
      : track_id_(track_id), replacement_(std::move(replacement)) {}

  bool apply(Project& project, sonare::arrangement::MidiContentStore&) override {
    auto* track = project.find_track_mutable(track_id_);
    if (track == nullptr) return false;
    track->name = replacement_;
    return true;
  }

  sonare::arrangement::EditCommandPtr invert(
      const Project& before, const sonare::arrangement::MidiContentStore&) const override {
    ++invert_calls_;
    if (invert_calls_ == 2) throw std::bad_alloc();
    const auto* track = before.find_track(track_id_);
    if (track == nullptr) return nullptr;
    return std::make_unique<sonare::arrangement::RenameTrack>(track_id_, track->name);
  }

  const char* type_name() const noexcept override { return "ThrowingRedoInvertCommand"; }
  bool mutates_midi_store() const noexcept override { return false; }
  size_t retained_bytes() const noexcept override {
    return sonare::arrangement::retained::saturating_add(
        sizeof(*this), sonare::arrangement::retained::dynamic_bytes(replacement_));
  }

 private:
  sonare::arrangement::TrackId track_id_ = 0;
  std::string replacement_;
  mutable size_t invert_calls_ = 0;
};

struct SidecarRateFaultPlan {
  int value = 0;
};

// A transaction stores its children inside an EditCommandGroup. Exercise both
// directions with a child that mutates an external sidecar and then throws:
// the group rollback must visit that child and all earlier children in reverse
// order, while EditHistory restores the Project snapshot and stack entries.
class ThrowingSidecarRateCommand final : public sonare::arrangement::EditCommand,
                                         public sonare::arrangement::EditCommandRollback {
 public:
  ThrowingSidecarRateCommand(SidecarRateFaultPlan* sidecar, double rate, int value,
                             int throw_on_apply_call, int prior_sidecar_value = 0,
                             int return_false_on_apply_call = 0)
      : sidecar_(sidecar),
        rate_(rate),
        value_(value),
        throw_on_apply_call_(throw_on_apply_call),
        return_false_on_apply_call_(return_false_on_apply_call),
        prior_sidecar_value_(prior_sidecar_value) {}

  bool apply(Project& project, sonare::arrangement::MidiContentStore&) override {
    if (sidecar_ == nullptr) return false;
    prior_rate_ = project.sample_rate();
    prior_sidecar_value_ = sidecar_->value;
    project.set_sample_rate(rate_);
    sidecar_->value = value_;
    applied_ = true;
    ++apply_calls_;
    if (throw_on_apply_call_ != 0 && apply_calls_ == throw_on_apply_call_) {
      throw std::runtime_error("synthetic sidecar failure");
    }
    if (return_false_on_apply_call_ != 0 && apply_calls_ == return_false_on_apply_call_) {
      return false;
    }
    return true;
  }

  sonare::arrangement::EditCommandPtr invert(
      const Project& before, const sonare::arrangement::MidiContentStore&) const override {
    return std::make_unique<ThrowingSidecarRateCommand>(
        sidecar_, before.sample_rate(), prior_sidecar_value_, inverse_throw_on_apply_call_, 0,
        inverse_return_false_on_apply_call_);
  }

  void rollback_apply(Project& project, sonare::arrangement::MidiContentStore&) noexcept override {
    if (!applied_ || sidecar_ == nullptr) return;
    project.set_sample_rate(prior_rate_);
    sidecar_->value = prior_sidecar_value_;
    applied_ = false;
  }

  const char* type_name() const noexcept override { return "ThrowingSidecarRateCommand"; }
  bool mutates_midi_store() const noexcept override { return false; }
  size_t retained_bytes() const noexcept override { return sizeof(*this); }

  void set_inverse_throw_on_apply_call(int call) noexcept { inverse_throw_on_apply_call_ = call; }
  void set_inverse_return_false_on_apply_call(int call) noexcept {
    inverse_return_false_on_apply_call_ = call;
  }

 private:
  SidecarRateFaultPlan* sidecar_ = nullptr;
  double rate_ = 0.0;
  int value_ = 0;
  int throw_on_apply_call_ = 0;
  int return_false_on_apply_call_ = 0;
  mutable int inverse_throw_on_apply_call_ = 0;
  mutable int inverse_return_false_on_apply_call_ = 0;
  mutable int prior_sidecar_value_ = 0;
  double prior_rate_ = 0.0;
  int apply_calls_ = 0;
  bool applied_ = false;
};

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

TEST_CASE("external stem import preserves existing PCM nodes and copies caller input",
          "[arrangement][external_stems]") {
  Project project;
  project.set_sample_rate(48000.0);
  AudioContentStore audio;

  sonare::arrangement::AudioSourceRef existing_ref;
  existing_ref.channel_count = 1;
  existing_ref.sample_rate_hint = 48000.0;
  const auto existing_id = project.add_audio_source(existing_ref);
  REQUIRE(existing_id != 0);
  sonare::arrangement::AudioSourceSamples existing_samples;
  existing_samples.sample_rate = 48000.0;
  existing_samples.channels = {{0.25f, -0.25f}};
  const auto [existing_it, inserted] = audio.sources.emplace(existing_id, existing_samples);
  REQUIRE(inserted);
  auto* existing_node = &existing_it->second;
  auto* existing_pcm = existing_it->second.channels.front().data();

  auto input = valid_stems();
  const auto result = sonare::arrangement::import_external_separated_stems(&project, &audio, input);
  REQUIRE(result.ok());
  REQUIRE(audio.sources.size() == 3);
  REQUIRE(&audio.sources.at(existing_id) == existing_node);
  REQUIRE(audio.sources.at(existing_id).channels.front().data() == existing_pcm);

  REQUIRE(!result.clip_ids.empty());
  const auto* imported_clip = project.find_clip(result.clip_ids.front());
  REQUIRE(imported_clip != nullptr);
  const auto imported_source_id = imported_clip->source_id;
  REQUIRE(imported_source_id != existing_id);
  REQUIRE(audio.sources.at(imported_source_id).channels == input.stems.front().planar_samples);

  // The const-input contract is a real ownership boundary: mutating the host
  // vectors after return must not affect the project-owned PCM.
  input.stems.front().planar_samples.front().front() = 99.0f;
  REQUIRE(audio.sources.at(imported_source_id).channels.front().front() == 1.0f);
}

TEST_CASE("external stem import rejects an audio-store source-id collision atomically",
          "[arrangement][external_stems]") {
  Project project;
  project.set_sample_rate(48000.0);
  AudioContentStore audio;
  sonare::arrangement::AudioSourceSamples orphan;
  orphan.sample_rate = 48000.0;
  orphan.channels = {{0.5f}};
  audio.sources.emplace(1, std::move(orphan));

  const auto result =
      sonare::arrangement::import_external_separated_stems(&project, &audio, valid_stems());
  REQUIRE(result.error == ExternalSeparatedStemImportError::kProjectMutationFailed);
  REQUIRE(project.tracks().empty());
  REQUIRE(project.clips().empty());
  REQUIRE(project.sources().empty());
  REQUIRE(project.next_source_id() == 1);
  REQUIRE(project.next_track_id() == 1);
  REQUIRE(project.next_clip_id() == 1);
  REQUIRE(audio.sources.size() == 1);
  REQUIRE(audio.sources.find(1) != audio.sources.end());
  REQUIRE(audio.sources.at(1).channels == std::vector<std::vector<float>>{{0.5f}});
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

TEST_CASE("external sidecar rollback survives inverse allocation failure",
          "[arrangement][external_stems][exception_safety]") {
  Project project;
  project.set_sample_rate(48000.0);
  AudioContentStore audio;

  sonare::arrangement::AudioSourceSamples existing_samples;
  existing_samples.sample_rate = 48000.0;
  existing_samples.channels = {{0.25f, -0.25f}};
  const auto [existing_it, existing_inserted] = audio.sources.emplace(7, existing_samples);
  REQUIRE(existing_inserted);
  const auto* existing_node = &existing_it->second;
  const auto* existing_pcm = existing_it->second.channels.front().data();

  sonare::arrangement::AudioSourceSamples imported_samples;
  imported_samples.sample_rate = 48000.0;
  imported_samples.channels = {{1.0f, 0.5f}};

  sonare::arrangement::EditHistory history(project);
  auto command = std::make_unique<BadAllocAfterExternalMutation>(&audio, 99, imported_samples);
  REQUIRE_THROWS_AS(history.apply(std::move(command)), std::bad_alloc);

  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 0);
  REQUIRE(history.project().tracks().empty());
  REQUIRE(history.project().clips().empty());
  REQUIRE(history.project().sources().empty());
  REQUIRE(history.project().next_track_id() == 1);
  REQUIRE(audio.sources.size() == 1);
  REQUIRE(&audio.sources.at(7) == existing_node);
  REQUIRE(audio.sources.at(7).channels.front().data() == existing_pcm);
  REQUIRE(audio.sources.find(99) == audio.sources.end());
}

TEST_CASE("transaction rollback restores sidecar on inverse allocation failure",
          "[arrangement][external_stems][exception_safety]") {
  Project project;
  project.set_sample_rate(48000.0);
  AudioContentStore audio;
  sonare::arrangement::AudioSourceSamples imported_samples;
  imported_samples.sample_rate = 48000.0;
  imported_samples.channels = {{1.0f}};

  sonare::arrangement::EditHistory history(project);
  std::vector<sonare::arrangement::EditCommandPtr> commands;
  sonare::arrangement::Track prior_track;
  prior_track.name = "prior";
  commands.push_back(std::make_unique<sonare::arrangement::AddTrack>(std::move(prior_track)));
  commands.push_back(
      std::make_unique<BadAllocAfterExternalMutation>(&audio, 101, imported_samples));

  REQUIRE_THROWS_AS(history.apply_transaction(std::move(commands)), std::bad_alloc);
  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 0);
  REQUIRE(history.project().tracks().empty());
  REQUIRE(history.project().next_track_id() == 1);
  REQUIRE(audio.sources.empty());
}

TEST_CASE("redo preserves its entry when inverse construction throws",
          "[arrangement][external_stems][exception_safety]") {
  Project project;
  sonare::arrangement::Track track;
  track.name = "before";
  const auto track_id = project.add_track(std::move(track));
  REQUIRE(track_id != 0);

  sonare::arrangement::EditHistory history(project);
  REQUIRE(history.apply(std::make_unique<ThrowingRedoInvertCommand>(track_id, "after")));
  REQUIRE(history.project().find_track(track_id)->name == "after");

  REQUIRE(history.undo());
  REQUIRE(history.project().find_track(track_id)->name == "before");
  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 1);

  REQUIRE_THROWS_AS(history.redo(), std::bad_alloc);
  REQUIRE(history.project().find_track(track_id)->name == "before");
  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 1);

  // The failed redo left the original forward command intact, so retrying it
  // can apply the edit and construct its inverse normally.
  REQUIRE(history.redo());
  REQUIRE(history.project().find_track(track_id)->name == "after");
  REQUIRE(history.undo_depth() == 1);
  REQUIRE(history.redo_depth() == 0);
  REQUIRE(history.undo());
  REQUIRE(history.project().find_track(track_id)->name == "before");
}

TEST_CASE("transaction group rolls back throwing sidecars during undo and redo",
          "[arrangement][external_stems][exception_safety]") {
  SidecarRateFaultPlan sidecar;
  Project project;
  project.set_sample_rate(10.0);
  sonare::arrangement::EditHistory history(project);

  auto first = std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0);
  first->set_inverse_throw_on_apply_call(1);
  auto second = std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 30.0, 2, 2);

  std::vector<sonare::arrangement::EditCommandPtr> commands;
  commands.push_back(std::move(first));
  commands.push_back(std::move(second));
  REQUIRE(history.apply_transaction(std::move(commands)));
  REQUIRE(history.project().sample_rate() == 30.0);
  REQUIRE(sidecar.value == 2);
  REQUIRE(history.undo_depth() == 1);
  REQUIRE(history.redo_depth() == 0);

  // Inverse order is second then first. The first inverse mutates sidecar 0
  // before throwing; group rollback must restore both it and second's change.
  REQUIRE_THROWS_AS(history.undo(), std::runtime_error);
  REQUIRE(history.project().sample_rate() == 30.0);
  REQUIRE(sidecar.value == 2);
  REQUIRE(history.undo_depth() == 1);
  REQUIRE(history.redo_depth() == 0);

  // Retry the undo after the one-shot fault. This establishes the redo entry
  // that will exercise the symmetric forward-group failure below.
  REQUIRE(history.undo());
  REQUIRE(history.project().sample_rate() == 10.0);
  REQUIRE(sidecar.value == 0);
  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 1);

  // Redo order is first then second. The second forward child throws after
  // changing the sidecar; both child hooks must restore the pre-redo value.
  REQUIRE_THROWS_AS(history.redo(), std::runtime_error);
  REQUIRE(history.project().sample_rate() == 10.0);
  REQUIRE(sidecar.value == 0);
  REQUIRE(history.undo_depth() == 0);
  REQUIRE(history.redo_depth() == 1);

  REQUIRE(history.redo());
  REQUIRE(history.project().sample_rate() == 30.0);
  REQUIRE(sidecar.value == 2);
  REQUIRE(history.undo_depth() == 1);
  REQUIRE(history.redo_depth() == 0);
}

TEST_CASE("EditHistory rolls back false-returning sidecars on every replay path",
          "[arrangement][external_stems][exception_safety]") {
  SECTION("apply") {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);

    REQUIRE_FALSE(
        history.apply(std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0, 0, 1)));
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
    REQUIRE(history.undo_depth() == 0);
    REQUIRE(history.redo_depth() == 0);
  }

  SECTION("transaction current child") {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);
    std::vector<sonare::arrangement::EditCommandPtr> commands;
    commands.push_back(std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0));
    commands.push_back(std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 30.0, 2, 0, 0, 1));

    REQUIRE_FALSE(history.apply_transaction(std::move(commands)));
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
    REQUIRE(history.undo_depth() == 0);
    REQUIRE(history.redo_depth() == 0);
  }

  SECTION("undo") {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);
    auto command = std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0);
    command->set_inverse_return_false_on_apply_call(1);
    REQUIRE(history.apply(std::move(command)));

    REQUIRE_FALSE(history.undo());
    REQUIRE(history.project().sample_rate() == 20.0);
    REQUIRE(sidecar.value == 1);
    REQUIRE(history.undo_depth() == 1);
    REQUIRE(history.redo_depth() == 0);
    REQUIRE(history.undo());
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
  }

  SECTION("redo") {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);
    REQUIRE(
        history.apply(std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0, 0, 2)));
    REQUIRE(history.undo());

    REQUIRE_FALSE(history.redo());
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
    REQUIRE(history.undo_depth() == 0);
    REQUIRE(history.redo_depth() == 1);
    REQUIRE(history.redo());
    REQUIRE(history.project().sample_rate() == 20.0);
    REQUIRE(sidecar.value == 1);
  }

  SECTION("transaction group undo and redo") {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);
    auto first = std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0);
    first->set_inverse_return_false_on_apply_call(1);
    std::vector<sonare::arrangement::EditCommandPtr> commands;
    commands.push_back(std::move(first));
    commands.push_back(std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 30.0, 2, 0, 0, 2));
    REQUIRE(history.apply_transaction(std::move(commands)));

    REQUIRE_FALSE(history.undo());
    REQUIRE(history.project().sample_rate() == 30.0);
    REQUIRE(sidecar.value == 2);
    REQUIRE(history.undo_depth() == 1);
    REQUIRE(history.redo_depth() == 0);
    REQUIRE(history.undo());
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);

    REQUIRE_FALSE(history.redo());
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
    REQUIRE(history.undo_depth() == 0);
    REQUIRE(history.redo_depth() == 1);
    REQUIRE(history.redo());
    REQUIRE(history.project().sample_rate() == 30.0);
    REQUIRE(sidecar.value == 2);
  }
}

TEST_CASE("transaction rollback compensates a failed inverse sidecar",
          "[arrangement][external_stems][exception_safety]") {
  // A succeeds, then B changes the same sidecar and fails. Rolling A back
  // invokes its inverse; that inverse can itself mutate the sidecar and fail.
  // Its own hook restores the A-applied state, so EditHistory must also invoke
  // A's paired forward hook to restore the pre-transaction state. Exercise
  // both false and throw signals at both failure seams.
  const auto run = [](bool inverse_throws, bool later_throws) {
    SidecarRateFaultPlan sidecar;
    Project project;
    project.set_sample_rate(10.0);
    sonare::arrangement::EditHistory history(project);

    auto first = std::make_unique<ThrowingSidecarRateCommand>(&sidecar, 20.0, 1, 0);
    if (inverse_throws) {
      first->set_inverse_throw_on_apply_call(1);
    } else {
      first->set_inverse_return_false_on_apply_call(1);
    }
    std::vector<sonare::arrangement::EditCommandPtr> commands;
    commands.push_back(std::move(first));
    commands.push_back(std::make_unique<ThrowingSidecarRateCommand>(
        &sidecar, 30.0, 2, later_throws ? 1 : 0, 0, later_throws ? 0 : 1));

    if (later_throws) {
      REQUIRE_THROWS_AS(history.apply_transaction(std::move(commands)), std::runtime_error);
    } else {
      REQUIRE_FALSE(history.apply_transaction(std::move(commands)));
    }
    REQUIRE(history.project().sample_rate() == 10.0);
    REQUIRE(sidecar.value == 0);
    REQUIRE(history.undo_depth() == 0);
    REQUIRE(history.redo_depth() == 0);
  };

  SECTION("false inverse after a false later child") { run(false, false); }
  SECTION("throwing inverse after a false later child") { run(true, false); }
  SECTION("false inverse after a throwing later child") { run(false, true); }
  SECTION("throwing inverse after a throwing later child") { run(true, true); }
}
