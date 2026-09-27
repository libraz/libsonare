/// @file midi_clip_gain_test.cpp
/// @brief The shared fade-curve math (sonare::clip_fade_gain), the pure
///        per-destination MIDI clip envelope evaluator, and the arrangement
///        compiler filling a MidiClipSchedule's gain/fade fields from an
///        EditClip.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "core/fade_curve.h"
#include "engine/clip_player.h"
#include "midi/midi_clip.h"
#include "midi/midi_clip_envelope.h"

namespace {

namespace arr = sonare::arrangement;
using sonare::clip_fade_gain;
using sonare::FadeCurve;
using sonare::engine::ClipAudioBuffer;
using sonare::engine::ClipPlayer;
using sonare::engine::ClipSchedule;
using sonare::midi::midi_clip_envelope_gain;
using sonare::midi::MidiClipSchedule;

// ---------------------------------------------------------------------------
// clip_fade_gain vs. the engine's actual fade output
// ---------------------------------------------------------------------------

TEST_CASE("clip_fade_gain matches ClipPlayer's rendered fade for every curve", "[midi_clip_gain]") {
  constexpr int64_t kLength = 100;
  constexpr int64_t kFadeIn = 20;
  constexpr int64_t kFadeOut = 30;
  const FadeCurve curve = GENERATE(FadeCurve::Linear, FadeCurve::EqualPower, FadeCurve::Exponential,
                                   FadeCurve::Logarithmic);

  std::vector<float> ch0(static_cast<size_t>(kLength), 1.0f);
  std::vector<float> ch1(static_cast<size_t>(kLength), 1.0f);
  const float* channel_ptrs[2] = {ch0.data(), ch1.data()};
  ClipAudioBuffer buffer;
  buffer.channels = channel_ptrs;
  buffer.num_channels = 2;
  buffer.num_samples = kLength;

  ClipSchedule schedule(/*clip_id=*/1, buffer, /*clip_start_ppq=*/0.0, /*clip_start_sample=*/0,
                        /*clip_offset=*/0, /*clip_length=*/kLength, /*clip_loop=*/false,
                        /*clip_gain=*/1.0f, kFadeIn, kFadeOut, curve, curve,
                        /*clip_has_separate_fade_out_curve=*/true);

  ClipPlayer player;
  player.prepare(48000.0, static_cast<int>(kLength));
  player.set_clips({schedule});

  std::vector<float> out0(static_cast<size_t>(kLength), 0.0f);
  std::vector<float> out1(static_cast<size_t>(kLength), 0.0f);
  float* out_ptrs[2] = {out0.data(), out1.data()};
  player.process_at(out_ptrs, 2, static_cast<int>(kLength), /*timeline_sample=*/0);

  for (int64_t i = 0; i < kLength; ++i) {
    const float expected = clip_fade_gain(i, kLength, kFadeIn, kFadeOut, curve, curve);
    CAPTURE(i);
    REQUIRE(out0[static_cast<size_t>(i)] == Catch::Approx(expected).margin(1.0e-6));
    REQUIRE(out1[static_cast<size_t>(i)] == Catch::Approx(expected).margin(1.0e-6));
  }
}

// ---------------------------------------------------------------------------
// midi_clip_envelope_gain: pure per-destination evaluator
// ---------------------------------------------------------------------------

MidiClipSchedule make_clip(uint32_t id, uint32_t destination_id, int64_t start_sample,
                           int64_t length_samples, float gain, int64_t fade_in_samples = 0,
                           int64_t fade_out_samples = 0,
                           FadeCurve fade_in_curve = FadeCurve::Linear,
                           FadeCurve fade_out_curve = FadeCurve::Linear) {
  MidiClipSchedule clip;
  clip.id = id;
  clip.destination_id = destination_id;
  clip.start_sample = start_sample;
  clip.length_samples = length_samples;
  clip.gain = gain;
  clip.fade_in_samples = fade_in_samples;
  clip.fade_out_samples = fade_out_samples;
  clip.fade_in_curve = fade_in_curve;
  clip.fade_out_curve = fade_out_curve;
  return clip;
}

TEST_CASE("midi_clip_envelope_gain is 1.0 before any clip has started", "[midi_clip_gain]") {
  const std::vector<MidiClipSchedule> clips = {make_clip(1, /*dest=*/1, 100, 50, 0.5f)};
  REQUIRE(midi_clip_envelope_gain(clips, 1, 0) == Catch::Approx(1.0f));
  REQUIRE(midi_clip_envelope_gain({}, 1, 0) == Catch::Approx(1.0f));
}

TEST_CASE("midi_clip_envelope_gain applies a single clip's own fade", "[midi_clip_gain]") {
  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, /*dest=*/5, 100, 50, 0.8f, /*fade_in=*/10, /*fade_out=*/10)};
  // Before start.
  REQUIRE(midi_clip_envelope_gain(clips, 5, 50) == Catch::Approx(1.0f));
  // Fade-in: position 0 and 5 of 10.
  REQUIRE(midi_clip_envelope_gain(clips, 5, 100) == Catch::Approx(0.0f));
  REQUIRE(midi_clip_envelope_gain(clips, 5, 105) == Catch::Approx(0.8f * 0.5f));
  // Fade-out: position 45 and 49 of [40, 50).
  REQUIRE(midi_clip_envelope_gain(clips, 5, 145) == Catch::Approx(0.8f * 0.5f));
  REQUIRE(midi_clip_envelope_gain(clips, 5, 149) == Catch::Approx(0.8f * 0.1f).margin(1.0e-6));
  // Held after end: fade-out present, so the end value is 0.
  REQUIRE(midi_clip_envelope_gain(clips, 5, 150) == Catch::Approx(0.0f));
  REQUIRE(midi_clip_envelope_gain(clips, 5, 100000) == Catch::Approx(0.0f));
}

TEST_CASE(
    "midi_clip_envelope_gain: a contained clip's latest start wins, then the container "
    "resumes",
    "[midi_clip_gain]") {
  // A = [0, 100), unity, no fades. B = [10, 20), fades out over its own tail.
  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, /*dest=*/1, 0, 100, 1.0f),
      make_clip(2, /*dest=*/1, 10, 10, 1.0f, /*fade_in=*/0, /*fade_out=*/5)};
  REQUIRE(midi_clip_envelope_gain(clips, 1, 5) == Catch::Approx(1.0f));  // A only
  REQUIRE(midi_clip_envelope_gain(clips, 1, 18) ==
          Catch::Approx(0.4f));  // B fading (position 8 of 10)
  REQUIRE(midi_clip_envelope_gain(clips, 1, 20) ==
          Catch::Approx(1.0f));  // B ended; A resumes, unfaded
  REQUIRE(midi_clip_envelope_gain(clips, 1, 99) == Catch::Approx(1.0f));  // still A
}

TEST_CASE("midi_clip_envelope_gain: partial overlap follows the later-starting clip",
          "[midi_clip_gain]") {
  // A = [0, 30) gain 0.6. B = [20, 50) gain 0.9. Neither fades.
  const std::vector<MidiClipSchedule> clips = {make_clip(1, /*dest=*/2, 0, 30, 0.6f),
                                               make_clip(2, /*dest=*/2, 20, 30, 0.9f)};
  REQUIRE(midi_clip_envelope_gain(clips, 2, 10) == Catch::Approx(0.6f));  // A only
  REQUIRE(midi_clip_envelope_gain(clips, 2, 25) ==
          Catch::Approx(0.9f));  // both active, B started later
  REQUIRE(midi_clip_envelope_gain(clips, 2, 35) == Catch::Approx(0.9f));  // A ended, B only
  // Both ended: B's end (50) is later than A's (30), so B's (unfaded) value holds.
  REQUIRE(midi_clip_envelope_gain(clips, 2, 55) == Catch::Approx(0.9f));
}

TEST_CASE("midi_clip_envelope_gain: an open-ended clip never fades out", "[midi_clip_gain]") {
  // length_samples == 0 means open-ended (still-recording clip). fade_out_samples
  // is set here specifically to confirm it is ignored, not merely defaulted away.
  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, /*dest=*/3, 0, /*length_samples=*/0, 0.5f, /*fade_in=*/10, /*fade_out=*/999)};
  REQUIRE(midi_clip_envelope_gain(clips, 3, 0) == Catch::Approx(0.0f));
  REQUIRE(midi_clip_envelope_gain(clips, 3, 5) == Catch::Approx(0.5f * 0.5f));
  REQUIRE(midi_clip_envelope_gain(clips, 3, 10) == Catch::Approx(0.5f));
  REQUIRE(midi_clip_envelope_gain(clips, 3, 1000000) == Catch::Approx(0.5f));
}

TEST_CASE("midi_clip_envelope_gain fades a looped clip once over its full length, not per loop",
          "[midi_clip_gain]") {
  MidiClipSchedule clip =
      make_clip(1, /*dest=*/4, 0, /*length_samples=*/100, 1.0f, /*fade_in=*/10, /*fade_out=*/10);
  clip.loop_mode = sonare::midi::MidiLoopMode::kLoop;
  clip.loop_length_samples = 25;  // four internal loop iterations within length_samples.
  const std::vector<MidiClipSchedule> clips = {clip};
  REQUIRE(midi_clip_envelope_gain(clips, 4, 5) == Catch::Approx(0.5f));  // in the one fade-in
  // Loop-local position 30 % 25 == 5 would wrongly re-fade if the fade retriggered
  // per loop iteration; the clip's absolute position (30) is well past fade-in.
  REQUIRE(midi_clip_envelope_gain(clips, 4, 30) == Catch::Approx(1.0f));
  REQUIRE(midi_clip_envelope_gain(clips, 4, 55) == Catch::Approx(1.0f));
  REQUIRE(midi_clip_envelope_gain(clips, 4, 95) == Catch::Approx(0.5f));  // the one fade-out
}

// ---------------------------------------------------------------------------
// edit_compiler: EditClip gain/fade -> MidiClipSchedule
// ---------------------------------------------------------------------------

TEST_CASE("compiler folds a MIDI clip's gain and fades into its MidiClipSchedule",
          "[midi_clip_gain]") {
  constexpr double kSampleRate = 48000.0;
  arr::Project project;
  project.set_sample_rate(kSampleRate);
  project.set_tempo_segments({{0.0, 120.0, 0.0}});  // 120 BPM: 1 quarter note = 24000 samples.

  arr::MidiSourceRef src;
  const arr::SourceId sid = project.add_midi_source(src);
  arr::Track track;
  track.kind = arr::Track::Kind::kMidi;
  const arr::TrackId tid = project.add_track(track);

  arr::EditClip clip;
  clip.track_id = tid;
  clip.source_id = sid;
  clip.start_ppq = 0.0;
  clip.length_ppq = 4.0;
  clip.gain = 0.5f;
  clip.fade_in.length_ppq = 1.0;
  clip.fade_in.curve = arr::FadeCurve::kEqualPower;
  clip.fade_out.length_ppq = 0.5;
  clip.fade_out.curve = arr::FadeCurve::kExponential;
  const arr::ClipId cid = project.add_clip(clip);
  REQUIRE(cid != 0);

  arr::CompileResult r = arr::compile(project, arr::MidiContentStore{}, arr::AudioContentStore{});
  REQUIRE_FALSE(r.has_errors());
  REQUIRE(r.timeline.has_value());
  REQUIRE(r.timeline->midi_clips.size() == 1);

  const MidiClipSchedule& sched = r.timeline->midi_clips.front();
  REQUIRE(sched.gain == Catch::Approx(0.5f));
  REQUIRE(sched.fade_in_samples == 24000);
  REQUIRE(sched.fade_out_samples == 12000);
  REQUIRE(sched.fade_in_curve == FadeCurve::EqualPower);
  REQUIRE(sched.fade_out_curve == FadeCurve::Exponential);
}

}  // namespace
