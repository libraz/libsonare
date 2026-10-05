/// @file timeline_apply_test.cpp
/// @brief apply_to_engine atomicity, re-apply replacement and strip binding.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "automation/automation_lane.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/types.h"

namespace {

namespace arr = sonare::arrangement;
using sonare::ErrorCode;
using sonare::engine::RealtimeEngine;

constexpr double kSr = 48000.0;
constexpr int kBlock = 128;
constexpr int64_t kFrames = 4096;
constexpr int kSourceFrames = 48000;

struct ProjectSpec {
  double bpm = 120.0;
  int numerator = 4;
  size_t track_count = 1;
  size_t marker_count = 0;
  float tone_hz = 220.0f;
  /// Gain of every track after the first; anything but 1 gives it a 1:1 synthesized strip.
  float later_track_gain = 1.0f;
  /// Typed fader automation on the first track (dB); 0 adds none.
  float fader_automation_db = 0.0f;
  size_t midi_clip_count = 0;
};

sonare::midi::MidiClipSchedule make_midi_clip(uint32_t id, uint32_t track_id) {
  sonare::midi::MidiClipSchedule clip;
  clip.id = id;
  clip.track_id = track_id;
  clip.length_samples = 1000;
  sonare::midi::MidiEvent note;
  note.render_frame = 10;
  note.ump = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  clip.events = {note};
  return clip;
}

arr::CompiledTimeline make_timeline(const ProjectSpec& spec) {
  arr::Project project;
  arr::MidiContentStore midi;
  arr::AudioContentStore audio;
  project.set_sample_rate(kSr);
  project.set_tempo_segments({{0.0, spec.bpm, 0.0}});
  project.set_time_signatures({{0.0, {spec.numerator, 4}}});

  arr::AudioSourceRef ref;
  ref.sample_rate_hint = kSr;
  ref.channel_count = 2;
  const arr::SourceId source = project.add_audio_source(ref);
  arr::AudioSourceSamples samples;
  samples.sample_rate = kSr;
  samples.channels.push_back(sonare::test::generate_sine_samples(
      spec.tone_hz, static_cast<int>(kSr), kSourceFrames, 0.25f));
  samples.channels.push_back(sonare::test::generate_sine_samples(
      spec.tone_hz * 1.5f, static_cast<int>(kSr), kSourceFrames, 0.2f));
  audio.sources.emplace(source, std::move(samples));

  std::vector<arr::TrackId> tracks;
  for (size_t i = 0; i < spec.track_count; ++i) {
    arr::Track track;
    track.name = "audio " + std::to_string(i);
    track.kind = arr::Track::Kind::kAudio;
    if (i > 0) track.gain = spec.later_track_gain;
    tracks.push_back(project.add_track(track));
    arr::EditClip clip;
    clip.track_id = tracks.back();
    clip.source_id = source;
    clip.start_ppq = 0.0;
    clip.length_ppq = 2.0;
    project.add_clip(clip);
  }
  for (size_t i = 0; i < spec.marker_count; ++i) {
    project.add_marker(static_cast<double>(i), "m" + std::to_string(i));
  }
  if (spec.fader_automation_db != 0.0f) {
    sonare::automation::AutomationLane lane(
        900, sonare::automation::AutomationTargetKind::kTrackFaderDb);
    lane.set_points({{0.0, spec.fader_automation_db, sonare::automation::CurveType::Hold}});
    REQUIRE(arr::AddAutomationLane(tracks.front(), lane).apply(project, midi));
  }

  arr::CompileResult result = arr::compile(project, midi, audio);
  REQUIRE_FALSE(result.has_errors());
  REQUIRE(result.timeline.has_value());
  arr::CompiledTimeline timeline = std::move(*result.timeline);
  for (size_t i = 0; i < spec.midi_clip_count; ++i) {
    timeline.midi_clips.push_back(make_midi_clip(static_cast<uint32_t>(100 + i), tracks.front()));
  }
  return timeline;
}

/// Timeline A: two tracks, markers, typed automation, a MIDI clip, 100 BPM in 3/4.
arr::CompiledTimeline timeline_a() {
  ProjectSpec spec;
  spec.bpm = 100.0;
  spec.numerator = 3;
  spec.track_count = 2;
  spec.marker_count = 2;
  spec.later_track_gain = 0.5f;
  spec.fader_automation_db = -6.0f;
  spec.midi_clip_count = 1;
  return make_timeline(spec);
}

/// Timeline B: one track, one marker, no automation, two MIDI clips, 140 BPM in 4/4.
arr::CompiledTimeline timeline_b() {
  ProjectSpec spec;
  spec.bpm = 140.0;
  spec.marker_count = 1;
  spec.tone_hz = 440.0f;
  spec.midi_clip_count = 2;
  return make_timeline(spec);
}

RealtimeEngine* prepared(RealtimeEngine& engine) {
  engine.prepare(kSr, kBlock);
  return &engine;
}

std::vector<float> render(RealtimeEngine& engine) {
  engine.prime_offline_parameters(2, kBlock);
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::vector<float> left(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> right(static_cast<size_t>(kFrames), 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.render_offline(channels, 2, kFrames, kBlock);
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

struct EngineState {
  size_t clips = 0;
  size_t midi_clips = 0;
  size_t markers = 0;
  size_t automation_lanes = 0;
  std::vector<uint32_t> lane_ids;
  double bpm_start = 0.0;
  double bpm_late = 0.0;
  int numerator = 0;
  int denominator = 0;

  bool operator==(const EngineState& o) const {
    return clips == o.clips && midi_clips == o.midi_clips && markers == o.markers &&
           automation_lanes == o.automation_lanes && lane_ids == o.lane_ids &&
           bpm_start == o.bpm_start && bpm_late == o.bpm_late && numerator == o.numerator &&
           denominator == o.denominator;
  }
};

EngineState capture(RealtimeEngine& engine) {
  EngineState s;
  s.clips = engine.clip_count();
  s.midi_clips = engine.midi_clip_count();
  s.markers = engine.marker_count();
  s.automation_lanes = engine.automation().lane_count();
#if defined(SONARE_WITH_MIXING)
  s.lane_ids.resize(sonare::engine::TrackMixerRuntime::kMaxTrackLanes);
  s.lane_ids.resize(engine.track_mixer().copy_lane_track_ids(s.lane_ids.data(), s.lane_ids.size()));
#endif
  s.bpm_start = engine.bpm_at_sample(0);
  s.bpm_late = engine.bpm_at_sample(static_cast<int64_t>(kSr) * 10);
  const auto sig = engine.time_signature_at_ppq(0.0);
  s.numerator = sig.numerator;
  s.denominator = sig.denominator;
  return s;
}

/// Applies @p bad over an engine holding timeline A and requires that nothing changed.
void require_refused_unchanged(const arr::CompiledTimeline& bad, const arr::ApplyOptions& options,
                               ErrorCode expected) {
  const arr::CompiledTimeline a = timeline_a();
  RealtimeEngine engine;
  RealtimeEngine reference;
  REQUIRE(arr::apply_to_engine(a, *prepared(engine), options).ok());
  REQUIRE(arr::apply_to_engine(a, *prepared(reference), options).ok());
  const EngineState before = capture(engine);
  REQUIRE(before.clips == 2);
  REQUIRE(before.midi_clips == 1);
  REQUIRE(before.markers == 2);
  REQUIRE(before.bpm_start == 100.0);

  const arr::ApplyResult result = arr::apply_to_engine(bad, engine, options);
  REQUIRE_FALSE(result.ok());
  REQUIRE(result.code == expected);
  REQUIRE(result.outcome == arr::ApplyOutcome::kUnchanged);
  REQUIRE(result.installed_automation.empty());
  REQUIRE(capture(engine) == before);
  REQUIRE(render(engine) == render(reference));
}

}  // namespace

TEST_CASE("timeline A and B render differently", "[arrangement][timeline-apply]") {
  // Guards the replacement cases below against two timelines that sound alike.
  RealtimeEngine a;
  RealtimeEngine b;
  REQUIRE(arr::apply_to_engine(timeline_a(), *prepared(a)).ok());
  REQUIRE(arr::apply_to_engine(timeline_b(), *prepared(b)).ok());
  REQUIRE(capture(a).bpm_start != capture(b).bpm_start);
  REQUIRE(render(a) != render(b));
}

TEST_CASE("apply refuses an invalid tempo without changing the engine",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  bad.tempo_segments = {{0.0, 0.0, 0.0}};
  require_refused_unchanged(bad, {}, ErrorCode::InvalidParameter);
}

TEST_CASE("apply refuses an invalid time signature without changing the engine",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  bad.time_signatures = {{0.0, {0, 4}}};
  require_refused_unchanged(bad, {}, ErrorCode::InvalidParameter);
}

TEST_CASE("apply refuses an unpreparable MIDI SysEx without changing the engine",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  sonare::midi::MidiClipSchedule broken = make_midi_clip(900, bad.track_lanes.front().track_id);
  sonare::midi::MidiEvent sysex;
  sysex.render_frame = 20;
  sysex.ump = sonare::midi::make_sysex_handle(0, 1);
  sysex.sysex_payload = nullptr;
  sysex.sysex_payload_size = 4;
  broken.events.push_back(sysex);
  bad.midi_clips.push_back(broken);
  require_refused_unchanged(bad, {}, ErrorCode::InvalidParameter);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("apply refuses 33 track lanes without changing the engine",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  bad.track_lanes.clear();
  for (uint32_t id = 1; id <= sonare::engine::TrackMixerRuntime::kMaxTrackLanes + 1; ++id) {
    bad.track_lanes.push_back({id});
  }
  REQUIRE(bad.track_lanes.size() == 33);
  require_refused_unchanged(bad, {}, ErrorCode::InvalidParameter);
}

TEST_CASE("apply refuses a scene strip shared by two tracks when binding strips",
          "[arrangement][timeline-apply]") {
  ProjectSpec spec;
  spec.track_count = 2;
  arr::CompiledTimeline bad = make_timeline(spec);
  sonare::mixing::api::Strip shared;
  shared.id = "shared";
  bad.mixer.scene.strips.push_back(shared);
  bad.mixer.bindings = {{bad.track_lanes[0].track_id, "shared"},
                        {bad.track_lanes[1].track_id, "shared"}};
  arr::ApplyOptions options;
  options.bind_strips = true;
  require_refused_unchanged(bad, options, ErrorCode::NotImplemented);
}

TEST_CASE("apply refuses a strip with an unacceptable EQ when binding strips",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  sonare::mixing::api::Strip strip;
  strip.id = "bad-eq";
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::TiltShelf;
  band.enabled = true;
  strip.eq.bands.push_back(band);
  bad.mixer.scene.strips.push_back(strip);
  bad.mixer.bindings = {{bad.track_lanes.front().track_id, "bad-eq"}};
  arr::ApplyOptions options;
  options.bind_strips = true;
  require_refused_unchanged(bad, options, ErrorCode::InvalidParameter);
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("re-applying B over A leaves nothing of A", "[arrangement][timeline-apply]") {
  arr::ApplyOptions options;
#if defined(SONARE_WITH_MIXING)
  options.bind_strips = true;
#endif
  RealtimeEngine engine;
  REQUIRE(arr::apply_to_engine(timeline_a(), *prepared(engine), options).ok());
  const arr::CompiledTimeline b = timeline_b();
  const arr::ApplyResult result = arr::apply_to_engine(b, engine, options);
  REQUIRE(result.ok());
  REQUIRE(result.outcome == arr::ApplyOutcome::kApplied);
  REQUIRE(result.installed_automation.empty());

  RealtimeEngine fresh;
  REQUIRE(arr::apply_to_engine(b, *prepared(fresh), options).ok());
  const EngineState state = capture(engine);
  REQUIRE(state == capture(fresh));
  REQUIRE(state.clips == 1);
  REQUIRE(state.midi_clips == 2);
  REQUIRE(state.markers == 1);
  REQUIRE(state.automation_lanes == 0);
  REQUIRE(render(engine) == render(fresh));
}

TEST_CASE("installed_automation is the lane vector the engine plays",
          "[arrangement][timeline-apply]") {
  const arr::CompiledTimeline a = timeline_a();
  RealtimeEngine engine;
  const arr::ApplyResult result = arr::apply_to_engine(a, *prepared(engine));
  REQUIRE(result.ok());
  REQUIRE(result.outcome == arr::ApplyOutcome::kApplied);
  REQUIRE(result.installed_automation.size() == engine.automation().lane_count());
  REQUIRE(result.installed_automation.size() == 1);
#if defined(SONARE_WITH_MIXING)
  // The typed lane carries the resolved lane-0 id, not its persistent edit id.
  REQUIRE(result.installed_automation.front().target_param_id() ==
          sonare::engine::make_track_lane_param_id(
              0, static_cast<uint32_t>(sonare::automation::AutomationTargetKind::kTrackFaderDb)));
#endif

  // Republishing the returned vector, as a binding wrapper does, must not change playback.
  RealtimeEngine reference;
  REQUIRE(arr::apply_to_engine(a, *prepared(reference)).ok());
  engine.automation().set_lanes(result.installed_automation);
  REQUIRE(engine.automation().lane_count() == reference.automation().lane_count());
  REQUIRE(render(engine) == render(reference));
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("bind_strips binds each 1:1 strip and unbinds lanes without a binding",
          "[arrangement][timeline-apply]") {
  ProjectSpec spec;
  spec.track_count = 3;
  spec.later_track_gain = 0.5f;
  arr::CompiledTimeline timeline = make_timeline(spec);
  REQUIRE(timeline.track_lanes.size() == 3);
  // The compiler bound tracks 2 and 3 to synthesized strips; drop track 3's binding so its lane
  // must end unbound even though the engine held a strip for it.
  REQUIRE(timeline.mixer.bindings.size() == 2);
  const uint32_t unbound_track = timeline.track_lanes[2].track_id;
  REQUIRE(timeline.mixer.bindings[1].track_id == unbound_track);
  timeline.mixer.bindings.pop_back();
  arr::ApplyOptions options;
  options.bind_strips = true;

  RealtimeEngine engine;
  prepared(engine);
  REQUIRE(engine.set_track_lanes({{unbound_track}}));
  sonare::mixing::api::Strip loud;
  loud.fader_db = 6.0f;
  REQUIRE(engine.set_track_strip(unbound_track, loud));
  const arr::ApplyResult result = arr::apply_to_engine(timeline, engine, options);
  REQUIRE(result.ok());
  REQUIRE(result.outcome == arr::ApplyOutcome::kApplied);

  // Reference: the same timeline without strip binding, then each binding wired by hand.
  RealtimeEngine manual;
  REQUIRE(arr::apply_to_engine(timeline, *prepared(manual)).ok());
  for (const arr::MixerStripBinding& binding : timeline.mixer.bindings) {
    for (const sonare::mixing::api::Strip& strip : timeline.mixer.scene.strips) {
      if (strip.id == binding.strip_id) REQUIRE(manual.set_track_strip(binding.track_id, strip));
    }
  }
  RealtimeEngine unbound;
  REQUIRE(arr::apply_to_engine(timeline, *prepared(unbound)).ok());

  const std::vector<float> bound_render = render(engine);
  REQUIRE(bound_render == render(manual));
  REQUIRE(bound_render != render(unbound));
}

TEST_CASE("a setter refusal after the first mutation clears every timeline domain",
          "[arrangement][timeline-apply]") {
  // The track mixer's owned-strip table holds 32 entries and is never pruned, so a 33rd distinct
  // bound track is refused by set_track_strip after the lanes were already published.
  ProjectSpec spec;
  spec.track_count = sonare::engine::TrackMixerRuntime::kMaxTrackLanes;
  spec.later_track_gain = 0.5f;
  spec.marker_count = 1;
  spec.fader_automation_db = -6.0f;
  spec.midi_clip_count = 1;
  const arr::CompiledTimeline first = make_timeline(spec);
  REQUIRE(first.track_lanes.size() == 32);
  REQUIRE(first.mixer.bindings.size() == 31);

  // 31 bound strips from the timeline; a hand-bound strip on the unbound first track fills the
  // table.
  arr::ApplyOptions options;
  options.bind_strips = true;
  RealtimeEngine engine;
  REQUIRE(arr::apply_to_engine(first, *prepared(engine), options).ok());
  REQUIRE(engine.set_track_strip(first.track_lanes.front().track_id, {}));

  // Second: one new track id bound to its own strip.
  constexpr uint32_t last_track = 1000;
  arr::CompiledTimeline second = first;
  second.track_lanes = {{last_track}};
  second.mixer.bindings = {{last_track, first.mixer.bindings.back().strip_id}};
  second.mixer.automation_bindings.clear();
  second.automation_lanes.clear();
  REQUIRE(engine.validate_track_lanes({{last_track}}));
  const arr::ApplyResult result = arr::apply_to_engine(second, engine, options);
  REQUIRE_FALSE(result.ok());
  REQUIRE(result.outcome == arr::ApplyOutcome::kCleared);
  REQUIRE(result.code == ErrorCode::InvalidState);
  REQUIRE(result.installed_automation.empty());

  const EngineState state = capture(engine);
  REQUIRE(state.clips == 0);
  REQUIRE(state.midi_clips == 0);
  REQUIRE(state.markers == 0);
  REQUIRE(state.automation_lanes == 0);
  REQUIRE(state.lane_ids.empty());
  REQUIRE(state.bpm_start == sonare::constants::kDefaultBpm);
  REQUIRE(state.numerator == 4);
  REQUIRE(state.denominator == 4);
  const std::vector<float> out = render(engine);
  REQUIRE(out == std::vector<float>(out.size(), 0.0f));
}
#else
TEST_CASE("bind_strips with strip bindings is refused without mixing",
          "[arrangement][timeline-apply]") {
  arr::CompiledTimeline bad = timeline_b();
  bad.mixer.bindings = {{bad.track_lanes.front().track_id, "strip"}};
  arr::ApplyOptions options;
  options.bind_strips = true;
  require_refused_unchanged(bad, options, ErrorCode::NotImplemented);
}
#endif  // SONARE_WITH_MIXING
