/// @file edit_compiler_golden_test.cpp
/// @brief compiler / RT snapshot golden + determinism tests.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "engine/realtime_engine.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "util/constants.h"

using sonare::constants::kTwoPi;

namespace {

namespace arr = sonare::arrangement;

constexpr double kProjectSr = 48000.0;
constexpr int kBlock = 128;

// FNV-1a over quantized float samples; matches the determinism style used by the
// engine offline-bounce golden test.
uint64_t hash_samples(const float* samples, size_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < count; ++i) {
    const int32_t q = static_cast<int32_t>(std::lround(samples[i] * 1000000.0f));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= static_cast<uint8_t>((static_cast<uint32_t>(q) >> (byte * 8)) & 0xffu);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

// Deterministic mono sine source (no clock / random).
std::vector<float> make_sine(double sr, double freq, int frames, float amp) {
  std::vector<float> out(static_cast<size_t>(frames), 0.0f);
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / sr;
    out[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * freq * t));
  }
  return out;
}

// Builds a 1-track / 1-clip project at 48 kHz, 120 BPM, with a 48000-sample
// stereo sine source registered in the audio content store.
struct Fixture {
  arr::Project project;
  arr::MidiContentStore midi;
  arr::AudioContentStore audio;
  arr::SourceId source_id = 0;
  arr::TrackId track_id = 0;
  arr::ClipId clip_id = 0;
};

Fixture make_fixture(int source_frames = 48000, double source_sr = kProjectSr) {
  Fixture f;
  f.project.set_sample_rate(kProjectSr);
  f.project.set_tempo_segments({{0.0, 120.0, 0.0}});
  f.project.set_time_signatures({{0.0, {4, 4}}});

  sonare::arrangement::AudioSourceRef ref;
  ref.sample_rate_hint = source_sr;
  ref.channel_count = 2;
  f.source_id = f.project.add_audio_source(ref);

  arr::Track track;
  track.name = "audio";
  track.kind = arr::Track::Kind::kAudio;
  f.track_id = f.project.add_track(track);

  arr::EditClip clip;
  clip.track_id = f.track_id;
  clip.source_id = f.source_id;
  clip.start_ppq = 0.0;
  clip.length_ppq = 2.0;  // two quarter notes
  f.clip_id = f.project.add_clip(clip);

  // Register decoded samples (stereo: 220 Hz / 330 Hz).
  arr::AudioSourceSamples samples;
  samples.sample_rate = source_sr;
  samples.channels.push_back(make_sine(source_sr, 220.0, source_frames, 0.25f));
  samples.channels.push_back(make_sine(source_sr, 330.0, source_frames, 0.18f));
  f.audio.sources.emplace(f.source_id, std::move(samples));
  return f;
}

// Renders a compiled timeline offline and returns interleaved output.
std::vector<float> render(const arr::CompiledTimeline& timeline, int64_t frames) {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kProjectSr, kBlock);
  arr::apply_to_engine(timeline, engine);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> left(static_cast<size_t>(frames), 0.0f);
  std::vector<float> right(static_cast<size_t>(frames), 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.render_offline(channels, 2, frames, kBlock);

  std::vector<float> interleaved(static_cast<size_t>(frames) * 2, 0.0f);
  for (int64_t i = 0; i < frames; ++i) {
    interleaved[static_cast<size_t>(i) * 2] = left[static_cast<size_t>(i)];
    interleaved[static_cast<size_t>(i) * 2 + 1] = right[static_cast<size_t>(i)];
  }
  return interleaved;
}

}  // namespace

TEST_CASE("compile -> offline bounce is deterministic across two renders", "[arrangement]") {
  Fixture f = make_fixture();

  arr::CompileResult r = arr::compile(f.project, f.midi, f.audio);
  REQUIRE_FALSE(r.has_errors());
  REQUIRE(r.timeline.has_value());
  REQUIRE(r.timeline->audio_clips.size() == 1);

  const int64_t frames = 24000;  // one quarter note at 120 BPM / 48 kHz
  const std::vector<float> a = render(*r.timeline, frames);
  const std::vector<float> b = render(*r.timeline, frames);

  REQUIRE(a == b);
  REQUIRE(hash_samples(a.data(), a.size()) == hash_samples(b.data(), b.size()));

  // Recompiling the same project yields an identical bounce too.
  arr::CompileResult r2 = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r2.timeline.has_value());
  const std::vector<float> c = render(*r2.timeline, frames);
  REQUIRE(a == c);
}

TEST_CASE("compiled snapshot is a fully-allocated immutable RT object", "[arrangement]") {
  Fixture f = make_fixture();
  arr::CompileResult r = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r.timeline.has_value());
  REQUIRE(r.timeline->audio_clips.size() == 1);

  const auto& clip = r.timeline->audio_clips.front();
  // Storage is owned and non-null; buffer.channels point into the storage.
  REQUIRE(clip.storage != nullptr);
  REQUIRE(clip.buffer.num_channels == 2);
  REQUIRE(clip.buffer.channels != nullptr);
  REQUIRE(clip.buffer.channels == clip.storage->channel_ptrs.data());
  for (int ch = 0; ch < clip.buffer.num_channels; ++ch) {
    REQUIRE(clip.buffer.channels[ch] == clip.storage->channels[static_cast<size_t>(ch)].data());
  }

  // Marker name pointers stay valid through a copy (re-pointed into the copy's
  // own storage, no dangling into the source).
  arr::Project p2 = f.project;
  p2.add_marker(1.0, "verse");
  arr::CompileResult rm = arr::compile(p2, f.midi, f.audio);
  REQUIRE(rm.timeline.has_value());
  arr::CompiledTimeline copy = *rm.timeline;
  REQUIRE(copy.markers.size() == 1);
  REQUIRE(copy.markers.front().name != nullptr);
  REQUIRE(std::string(copy.markers.front().name) == "verse");
  // The copied pointer must NOT alias the source snapshot's storage.
  REQUIRE(copy.markers.front().name == copy.marker_names.front().c_str());
  REQUIRE(copy.markers.front().name != rm.timeline->marker_names.front().c_str());
}

TEST_CASE("undo/redo then recompile yields equal compiled snapshot", "[arrangement]") {
  Fixture f = make_fixture();

  auto compile_clips = [&](const arr::Project& p) {
    arr::CompileResult r = arr::compile(p, f.midi, f.audio);
    REQUIRE(r.timeline.has_value());
    return std::move(*r.timeline);
  };

  arr::CompiledTimeline before = compile_clips(f.project);

  // Mutate via a command through EditHistory, then invert back to the original.
  arr::EditHistory history(f.project);
  REQUIRE(history.apply(std::make_unique<arr::SetClipGain>(f.clip_id, 0.5f)));
  arr::CompiledTimeline mutated = compile_clips(history.project());
  REQUIRE(mutated.audio_clips.front().gain == 0.5f);
  REQUIRE(mutated.audio_clips.front().gain != before.audio_clips.front().gain);

  REQUIRE(history.undo());
  arr::CompiledTimeline restored = compile_clips(history.project());

  // Audio clip schedule, markers, and tempo match the original compile.
  REQUIRE(restored.audio_clips.size() == before.audio_clips.size());
  REQUIRE(restored.audio_clips.front().gain == before.audio_clips.front().gain);
  REQUIRE(restored.audio_clips.front().start_sample == before.audio_clips.front().start_sample);
  REQUIRE(restored.audio_clips.front().length_samples == before.audio_clips.front().length_samples);
  REQUIRE(restored.tempo_segments.size() == before.tempo_segments.size());
  REQUIRE(restored.tempo_segments.front().bpm == before.tempo_segments.front().bpm);
  REQUIRE(restored.markers.size() == before.markers.size());

  // And the rendered bounce matches.
  const int64_t frames = 24000;
  REQUIRE(render(restored, frames) == render(before, frames));
}

TEST_CASE("invalid project: dangling source ref returns an error and no timeline",
          "[arrangement]") {
  Fixture f = make_fixture();
  // Drop the decoded samples so the clip's source has no audio.
  f.audio.sources.clear();

  arr::CompileResult r = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r.has_errors());
  REQUIRE_FALSE(r.timeline.has_value());
  bool found = false;
  for (const auto& d : r.diagnostics) {
    if (d.code == arr::Diagnostic::Code::kDanglingSourceRef) found = true;
  }
  REQUIRE(found);
}

TEST_CASE("invalid project: negative tempo returns an error and no timeline", "[arrangement]") {
  Fixture f = make_fixture();
  f.project.set_tempo_segments({{0.0, -120.0, 0.0}});

  arr::CompileResult r = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r.has_errors());
  REQUIRE_FALSE(r.timeline.has_value());
  bool found = false;
  for (const auto& d : r.diagnostics) {
    if (d.code == arr::Diagnostic::Code::kInvalidTempo) found = true;
  }
  REQUIRE(found);
}

TEST_CASE("invalid project: overlapping clips under kDisallow returns an error", "[arrangement]") {
  arr::Project project;
  project.set_sample_rate(kProjectSr);
  project.set_overlap_policy(arr::OverlapPolicy::kAllow);  // allow add, force overlap
  project.set_tempo_segments({{0.0, 120.0, 0.0}});

  sonare::arrangement::AudioSourceRef ref;
  ref.sample_rate_hint = kProjectSr;
  const arr::SourceId sid = project.add_audio_source(ref);
  arr::Track track;
  const arr::TrackId tid = project.add_track(track);

  arr::EditClip a;
  a.track_id = tid;
  a.source_id = sid;
  a.start_ppq = 0.0;
  a.length_ppq = 4.0;
  project.add_clip(a);
  arr::EditClip b;
  b.track_id = tid;
  b.source_id = sid;
  b.start_ppq = 2.0;  // overlaps [0,4)
  b.length_ppq = 4.0;
  project.add_clip(b);

  arr::MidiContentStore midi;
  arr::AudioContentStore audio;
  arr::AudioSourceSamples samples;
  samples.sample_rate = kProjectSr;
  samples.channels.push_back(make_sine(kProjectSr, 220.0, 48000, 0.25f));
  audio.sources.emplace(sid, std::move(samples));

  // Now switch policy back to kDisallow so compile enforces it.
  project.set_overlap_policy(arr::OverlapPolicy::kDisallow);
  arr::CompileResult r = arr::compile(project, midi, audio);
  REQUIRE(r.has_errors());
  REQUIRE_FALSE(r.timeline.has_value());
  bool found = false;
  for (const auto& d : r.diagnostics) {
    if (d.code == arr::Diagnostic::Code::kClipOverlap) found = true;
  }
  REQUIRE(found);
}

TEST_CASE("MIDI compile trims events outside the EditClip length", "[arrangement]") {
  arr::Project project;
  project.set_sample_rate(kProjectSr);
  project.set_tempo_segments({{0.0, 120.0, 0.0}});

  arr::MidiSourceRef src;
  const arr::SourceId sid = project.add_midi_source(src);
  arr::Track track;
  track.kind = arr::Track::Kind::kMidi;
  const arr::TrackId tid = project.add_track(track);

  arr::EditClip clip;
  clip.track_id = tid;
  clip.source_id = sid;
  clip.start_ppq = 2.0;
  clip.length_ppq = 1.0;
  clip.source_offset_ppq = 0.5;
  const arr::ClipId cid = project.add_clip(clip);
  REQUIRE(cid != 0);

  arr::MidiContentStore midi;
  const auto on = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const auto off = sonare::midi::make_midi1_note_off(0, 0, 60, 0);
  midi.events[cid] = {
      {0.25, on.words[0], 0},   // before source offset: dropped
      {0.50, on.words[0], 0},   // at clip start: kept
      {1.25, off.words[0], 0},  // inside length after rebasing: kept
      {1.50, off.words[0], 0},  // exactly at clip end after rebasing: dropped
  };

  arr::CompileResult r = arr::compile(project, midi, arr::AudioContentStore{});
  REQUIRE_FALSE(r.has_errors());
  REQUIRE(r.timeline.has_value());
  REQUIRE(r.timeline->midi_clips.size() == 1);
  const auto& events = r.timeline->midi_clips.front().events;
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].render_frame == 48000);
  REQUIRE(events[1].render_frame == 66000);
}

TEST_CASE("MIDI compile expands looped clips on the control thread", "[arrangement]") {
  arr::Project project;
  project.set_sample_rate(kProjectSr);
  project.set_tempo_segments({{0.0, 120.0, 0.0}});

  arr::MidiSourceRef src;
  const arr::SourceId sid = project.add_midi_source(src);
  arr::Track track;
  track.kind = arr::Track::Kind::kMidi;
  const arr::TrackId tid = project.add_track(track);

  arr::EditClip clip;
  clip.track_id = tid;
  clip.source_id = sid;
  clip.start_ppq = 0.0;
  clip.length_ppq = 3.0;
  clip.loop_mode = arr::LoopMode::kLoop;
  clip.loop_length_ppq = 1.0;
  const arr::ClipId cid = project.add_clip(clip);
  REQUIRE(cid != 0);

  arr::MidiContentStore midi;
  const auto on = sonare::midi::make_midi1_note_on(0, 0, 64, 100);
  midi.events[cid] = {
      {0.0, on.words[0], 0},
      {0.75, on.words[0], 0},
      {1.0, on.words[0], 0},  // exactly at loop end: not part of the loop body
  };

  arr::CompileResult r = arr::compile(project, midi, arr::AudioContentStore{});
  REQUIRE_FALSE(r.has_errors());
  REQUIRE(r.timeline.has_value());
  REQUIRE(r.timeline->midi_clips.size() == 1);
  const auto& events = r.timeline->midi_clips.front().events;
  REQUIRE(events.size() == 6);
  REQUIRE(events[0].render_frame == 0);
  REQUIRE(events[1].render_frame == 18000);
  REQUIRE(events[2].render_frame == 24000);
  REQUIRE(events[3].render_frame == 42000);
  REQUIRE(events[4].render_frame == 48000);
  REQUIRE(events[5].render_frame == 66000);
}

TEST_CASE("apply_to_engine installs full tempo and time-signature maps", "[arrangement]") {
  arr::CompiledTimeline timeline;
  timeline.tempo_segments = {{0.0, 120.0, 0.0}, {1.0, 60.0, 0.0}};
  timeline.time_signatures = {{0.0, {4, 4}}, {4.0, {3, 4}}};

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kProjectSr, kBlock);
  arr::apply_to_engine(timeline, engine);

  REQUIRE(engine.bpm_at_sample(0) == 120.0);
  REQUIRE(engine.bpm_at_sample(24000) == 60.0);

  const auto before = engine.time_signature_at_ppq(3.99);
  REQUIRE(before.numerator == 4);
  REQUIRE(before.denominator == 4);

  const auto after = engine.time_signature_at_ppq(4.0);
  REQUIRE(after.numerator == 3);
  REQUIRE(after.denominator == 4);
}

TEST_CASE("source SR != project SR resamples deterministically", "[arrangement]") {
  // 44100 Hz source into a 48000 Hz project: the compiler bakes a resampled,
  // fixed-length buffer with repeatable contents.
  Fixture f = make_fixture(/*source_frames=*/44100, /*source_sr=*/44100.0);

  arr::CompileResult r1 = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r1.timeline.has_value());
  REQUIRE(r1.timeline->audio_clips.size() == 1);
  const auto& clip1 = r1.timeline->audio_clips.front();
  REQUIRE(clip1.storage != nullptr);
  REQUIRE(clip1.storage->channels.size() == 2);

  // r8brain resamples 44100 -> 48000: expected length round(44100 * 48000/44100).
  const size_t expected_len = static_cast<size_t>(std::lround(44100.0 * 48000.0 / 44100.0));
  REQUIRE(clip1.storage->channels[0].size() == expected_len);

  // Re-compiling produces the same baked length and a stable hash.
  arr::CompileResult r2 = arr::compile(f.project, f.midi, f.audio);
  REQUIRE(r2.timeline.has_value());
  const auto& clip2 = r2.timeline->audio_clips.front();
  REQUIRE(clip2.storage->channels[0].size() == clip1.storage->channels[0].size());

  const uint64_t h1 =
      hash_samples(clip1.storage->channels[0].data(), clip1.storage->channels[0].size());
  const uint64_t h2 =
      hash_samples(clip2.storage->channels[0].data(), clip2.storage->channels[0].size());
  REQUIRE(h1 == h2);
}
