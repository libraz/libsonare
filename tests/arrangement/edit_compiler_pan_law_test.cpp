/// @file edit_compiler_pan_law_test.cpp
/// @brief A track's pan compiles to the same gains on both fold paths.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "rt/pan_law.h"
#include "support/audio_fixtures.h"

#if defined(SONARE_WITH_MIXING)

using Catch::Matchers::WithinAbs;
using sonare::rt::compute_pan_gains;
using sonare::rt::pan_law_from_index;
using sonare::rt::PanGains;
using sonare::rt::PanNormalization;

namespace {

namespace arr = sonare::arrangement;

constexpr double kProjectSr = 48000.0;
constexpr float kTrackPan = 0.5f;

struct Fixture {
  arr::Project project;
  arr::MidiContentStore midi;
  arr::AudioContentStore audio;
  arr::TrackId exclusive_track = 0;
  arr::TrackId shared_track = 0;
};

// Three audio tracks carrying the same pan over the same source: one bound to a
// strip of its own, two bound to a single strip they share. Only the number of
// tracks on the strip differs, which is what selects the fold path.
Fixture make_fixture(int shared_pan_law) {
  Fixture f;
  f.project.set_sample_rate(kProjectSr);
  f.project.set_tempo_segments({{0.0, 120.0, 0.0}});
  f.project.set_time_signatures({{0.0, {4, 4}}});

  arr::AudioSourceRef ref;
  ref.sample_rate_hint = kProjectSr;
  ref.channel_count = 2;
  const arr::SourceId source_id = f.project.add_audio_source(ref);

  arr::AudioSourceSamples samples;
  samples.sample_rate = kProjectSr;
  samples.channels.push_back(
      sonare::test::generate_sine_samples(220.0f, static_cast<int>(kProjectSr), 24000, 0.25f));
  samples.channels.push_back(
      sonare::test::generate_sine_samples(220.0f, static_cast<int>(kProjectSr), 24000, 0.25f));
  f.audio.sources.emplace(source_id, std::move(samples));

  sonare::mixing::api::Strip exclusive;
  exclusive.id = "exclusive";
  exclusive.pan_law = shared_pan_law;
  sonare::mixing::api::Strip shared;
  shared.id = "shared";
  shared.pan_law = shared_pan_law;
  f.project.scene().strips.push_back(exclusive);
  f.project.scene().strips.push_back(shared);

  const auto add_track = [&](const std::string& name, const std::string& strip_ref) {
    arr::Track track;
    track.name = name;
    track.kind = arr::Track::Kind::kAudio;
    track.pan = kTrackPan;
    track.channel_strip_ref = strip_ref;
    const arr::TrackId id = f.project.add_track(track);

    arr::EditClip clip;
    clip.track_id = id;
    clip.source_id = source_id;
    clip.start_ppq = 0.0;
    clip.length_ppq = 2.0;
    f.project.add_clip(clip);
    return id;
  };

  f.exclusive_track = add_track("exclusive", "exclusive");
  f.shared_track = add_track("shared a", "shared");
  add_track("shared b", "shared");
  return f;
}

// The id is taken by value: a reference parameter bound to a string literal
// makes GCC read the returned reference as possibly dangling.
const sonare::mixing::api::Strip& find_strip(const arr::CompiledTimeline& timeline,
                                             std::string_view id) {
  for (const sonare::mixing::api::Strip& strip : timeline.mixer.scene.strips) {
    if (strip.id == id) return strip;
  }
  FAIL("strip not found: " << id);
  return timeline.mixer.scene.strips.front();
}

const sonare::engine::ClipSchedule& find_clip(const arr::CompiledTimeline& timeline,
                                              arr::TrackId track_id) {
  for (const sonare::engine::ClipSchedule& clip : timeline.audio_clips) {
    if (clip.track_id == track_id) return clip;
  }
  FAIL("no compiled clip for track " << track_id);
  return timeline.audio_clips.front();
}

}  // namespace

TEST_CASE("track pan compiles to the same gains whether or not its strip is shared",
          "[arrangement][pan]") {
  // Sweep the laws a strip can carry: the two fold paths have to agree under
  // each of them, not just under the default.
  for (int law_index = 0; law_index <= 3; ++law_index) {
    Fixture f = make_fixture(law_index);
    const arr::CompileResult result = arr::compile(f.project, f.midi, f.audio);
    REQUIRE_FALSE(result.has_errors());
    REQUIRE(result.timeline.has_value());
    const arr::CompiledTimeline& timeline = *result.timeline;

    // Exclusive strip: the track's pan was folded into the strip, so the gains
    // are whatever the strip's panner will produce from it.
    const sonare::mixing::api::Strip& strip = find_strip(timeline, "exclusive");
    REQUIRE_THAT(strip.pan, WithinAbs(kTrackPan, 1e-6f));
    const PanGains strip_gains = compute_pan_gains(strip.pan, pan_law_from_index(strip.pan_law),
                                                   PanNormalization::NearUnity);

    // Shared strip: the track's pan was folded into its own clips instead. The
    // schedule has to carry the strip's law, not a law of its own.
    const sonare::engine::ClipSchedule& clip = find_clip(timeline, f.shared_track);
    REQUIRE_THAT(clip.pan, WithinAbs(kTrackPan, 1e-6f));
    REQUIRE(static_cast<int>(clip.pan_law) == static_cast<int>(pan_law_from_index(law_index)));
    const PanGains clip_gains =
        compute_pan_gains(clip.pan, clip.pan_law, PanNormalization::NearUnity);

    REQUIRE_THAT(clip_gains.left, WithinAbs(strip_gains.left, 1e-6f));
    REQUIRE_THAT(clip_gains.right, WithinAbs(strip_gains.right, 1e-6f));
  }
}

TEST_CASE("a clip on an exclusive strip carries no folded pan", "[arrangement][pan]") {
  // The exclusive path must not fold twice: the strip carries the pan, so the
  // clip stays centered and its law is irrelevant.
  Fixture f = make_fixture(0);
  const arr::CompileResult result = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(result.timeline.has_value());
  const sonare::engine::ClipSchedule& clip = find_clip(*result.timeline, f.exclusive_track);
  REQUIRE_THAT(clip.pan, WithinAbs(0.0f, 1e-6f));
}

#endif  // SONARE_WITH_MIXING
