#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "util/constants.h"

namespace {

namespace arr = sonare::arrangement;

arr::SourceId add_audio_source(arr::Project* project) {
  arr::AudioSourceRef source;
  source.channel_count = 1;
  source.sample_rate_hint = sonare::constants::kDefaultDawSampleRate;
  return project->add_audio_source(std::move(source));
}

arr::ClipId add_audio_clip(arr::Project* project, arr::TrackId track_id, arr::SourceId source_id,
                           double start_ppq, double length_ppq) {
  arr::EditClip clip;
  clip.track_id = track_id;
  clip.source_id = source_id;
  clip.start_ppq = start_ppq;
  clip.length_ppq = length_ppq;
  return project->add_clip(std::move(clip));
}

arr::TrackId add_audio_track(arr::Project* project, bool mute = false, bool solo = false) {
  arr::Track track;
  track.kind = arr::Track::Kind::kAudio;
  track.mute = mute;
  track.solo = solo;
  return project->add_track(std::move(track));
}

}  // namespace

TEST_CASE("empty project has no arrangement endpoint", "[project][timeline]") {
  const arr::Project project;
  CHECK(arr::arrangement_end_ppq(project) == 0.0);
}

TEST_CASE("arrangement endpoint is the greatest valid clip end", "[project][timeline]") {
  arr::Project project;
  const arr::SourceId source_id = add_audio_source(&project);
  const arr::TrackId track_id = add_audio_track(&project);
  REQUIRE(source_id != 0);
  REQUIRE(track_id != 0);
  REQUIRE(add_audio_clip(&project, track_id, source_id, 8.0, 7.0) != 0);
  REQUIRE(add_audio_clip(&project, track_id, source_id, 3.0, 4.0) != 0);

  CHECK(arr::arrangement_end_ppq(project) == 15.0);
}

TEST_CASE("mute and solo rules filter arrangement endpoints", "[project][timeline]") {
  arr::Project project;
  const arr::SourceId source_id = add_audio_source(&project);
  const arr::TrackId muted_track = add_audio_track(&project, true);
  const arr::TrackId solo_track = add_audio_track(&project, false, true);
  const arr::TrackId other_track = add_audio_track(&project);
  REQUIRE(source_id != 0);
  REQUIRE(muted_track != 0);
  REQUIRE(solo_track != 0);
  REQUIRE(other_track != 0);
  REQUIRE(add_audio_clip(&project, muted_track, source_id, 0.0, 100.0) != 0);
  REQUIRE(add_audio_clip(&project, solo_track, source_id, 5.0, 10.0) != 0);
  REQUIRE(add_audio_clip(&project, other_track, source_id, 0.0, 1000.0) != 0);

  CHECK(arr::arrangement_end_ppq(project) == 15.0);
}

TEST_CASE("an unused take does not change its clip endpoint", "[project][timeline]") {
  arr::Project project;
  const arr::SourceId active_source = add_audio_source(&project);
  arr::MidiSourceRef unused_source_ref;
  const arr::SourceId unused_source = project.add_midi_source(std::move(unused_source_ref));
  const arr::TrackId track_id = add_audio_track(&project);
  REQUIRE(active_source != 0);
  REQUIRE(unused_source != 0);
  REQUIRE(track_id != 0);

  arr::EditClip clip;
  clip.track_id = track_id;
  clip.source_id = active_source;
  clip.start_ppq = 7.0;
  clip.length_ppq = 4.0;
  arr::ClipTake take;
  take.id = 1;
  take.source_id = unused_source;
  take.source_offset_ppq = 900.0;
  clip.takes.push_back(std::move(take));
  REQUIRE(project.add_clip(std::move(clip)) != 0);

  CHECK(arr::arrangement_end_ppq(project) == 11.0);
}

TEST_CASE("invalid typed sources and clip ranges have no endpoint", "[project][timeline]") {
  SECTION("source kind does not match the track") {
    arr::Project project;
    const arr::SourceId audio_source = add_audio_source(&project);
    arr::MidiSourceRef midi_source_ref;
    const arr::SourceId midi_source = project.add_midi_source(std::move(midi_source_ref));
    const arr::TrackId track_id = add_audio_track(&project);
    const arr::ClipId clip_id = add_audio_clip(&project, track_id, audio_source, 0.0, 5.0);
    REQUIRE(audio_source != 0);
    REQUIRE(midi_source != 0);
    REQUIRE(clip_id != 0);
    project.find_clip_mutable(clip_id)->source_id = midi_source;

    CHECK(arr::arrangement_end_ppq(project) == 0.0);
  }

  SECTION("clip endpoint overflows") {
    arr::Project project;
    const arr::SourceId source_id = add_audio_source(&project);
    const arr::TrackId track_id = add_audio_track(&project);
    const arr::ClipId clip_id = add_audio_clip(&project, track_id, source_id, 0.0, 5.0);
    REQUIRE(clip_id != 0);
    arr::EditClip* clip = project.find_clip_mutable(clip_id);
    REQUIRE(clip != nullptr);
    clip->start_ppq = std::numeric_limits<double>::max() * 0.75;
    clip->length_ppq = std::numeric_limits<double>::max() * 0.75;

    CHECK(arr::arrangement_end_ppq(project) == 0.0);
  }
}

TEST_CASE("muted audio schedules remain present while the endpoint excludes them",
          "[project][timeline]") {
  arr::Project project;
  project.set_sample_rate(sonare::constants::kDefaultDawSampleRate);
  const arr::SourceId source_id = add_audio_source(&project);
  const arr::TrackId track_id = add_audio_track(&project, true);
  const arr::ClipId clip_id = add_audio_clip(&project, track_id, source_id, 0.0, 1.0);
  REQUIRE(clip_id != 0);

  arr::AudioContentStore audio;
  arr::AudioSourceSamples samples;
  samples.sample_rate = project.sample_rate();
  samples.channels.push_back({0.25f});
  audio.sources.emplace(source_id, std::move(samples));

  const arr::CompileResult compiled = arr::compile(project, {}, audio);
  REQUIRE(compiled.timeline.has_value());
  REQUIRE(compiled.timeline->audio_clips.size() == 1);
  CHECK(arr::arrangement_end_ppq(project) == 0.0);
}
