/// @file edit_command_history_test.cpp
/// @brief edit command tests: apply/invert round-trips, deterministic replay,
/// and undo/redo stack behaviour for the arrangement subsystem.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"

using namespace sonare::arrangement;

namespace {

// ---------------------------------------------------------------------------
// Deep-equality helpers over the affected model fields. The project model structs do
// not define operator==, so the test compares the value-oriented fields it
// mutates. AutomationLane is compared via its accessors.
// ---------------------------------------------------------------------------

bool lane_equal(const sonare::automation::AutomationLane& a,
                const sonare::automation::AutomationLane& b) {
  if (a.target_param_id() != b.target_param_id()) return false;
  if (a.points().size() != b.points().size()) return false;
  for (size_t i = 0; i < a.points().size(); ++i) {
    const auto& pa = a.points()[i];
    const auto& pb = b.points()[i];
    if (pa.ppq != pb.ppq || pa.value != pb.value || pa.curve_to_next != pb.curve_to_next) {
      return false;
    }
  }
  return true;
}

bool track_equal(const Track& a, const Track& b) {
  if (a.id != b.id || a.name != b.name || a.kind != b.kind ||
      a.channel_strip_ref != b.channel_strip_ref || a.output_target != b.output_target) {
    return false;
  }
  if (a.automation_lanes.size() != b.automation_lanes.size()) return false;
  for (size_t i = 0; i < a.automation_lanes.size(); ++i) {
    if (!lane_equal(a.automation_lanes[i], b.automation_lanes[i])) return false;
  }
  return true;
}

bool fade_equal(const ClipFade& a, const ClipFade& b) {
  return a.length_ppq == b.length_ppq && a.curve == b.curve;
}

bool clip_equal(const EditClip& a, const EditClip& b) {
  return a.id == b.id && a.track_id == b.track_id && a.source_id == b.source_id &&
         a.start_ppq == b.start_ppq && a.length_ppq == b.length_ppq &&
         a.source_offset_ppq == b.source_offset_ppq && a.gain == b.gain &&
         fade_equal(a.fade_in, b.fade_in) && fade_equal(a.fade_out, b.fade_out) &&
         a.loop_mode == b.loop_mode && a.loop_length_ppq == b.loop_length_ppq &&
         a.warp_ref_id == b.warp_ref_id;
}

bool source_equal(const ClipSource& a, const ClipSource& b) {
  if (a.index() != b.index()) return false;
  if (const auto* aa = std::get_if<AudioSourceRef>(&a)) {
    const auto& bb = std::get<AudioSourceRef>(b);
    return aa->id == bb.id && aa->uri == bb.uri && aa->channel_count == bb.channel_count &&
           aa->sample_rate_hint == bb.sample_rate_hint &&
           aa->storage_handle_id == bb.storage_handle_id;
  }
  const auto& am = std::get<MidiSourceRef>(a);
  const auto& bm = std::get<MidiSourceRef>(b);
  return am.id == bm.id && am.name == bm.name && am.channel_hint == bm.channel_hint;
}

bool chord_equal(const ChordSymbol& a, const ChordSymbol& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.root_pc == b.root_pc &&
         a.quality == b.quality && a.extensions == b.extensions &&
         a.slash_bass_pc == b.slash_bass_pc && a.roman_numeral == b.roman_numeral &&
         a.modulation_boundary == b.modulation_boundary;
}

bool annotation_equal(const ProjectAnnotation& a, const ProjectAnnotation& b) {
  if (a.tempo_confidence != b.tempo_confidence) return false;
  if (a.chords.size() != b.chords.size()) return false;
  for (size_t i = 0; i < a.chords.size(); ++i) {
    if (!chord_equal(a.chords[i], b.chords[i])) return false;
  }
  // keys / sections / onsets compared coarsely by size for these tests.
  return a.keys.size() == b.keys.size() && a.sections.size() == b.sections.size() &&
         a.onsets.size() == b.onsets.size();
}

bool sidecar_equal(const AssistSidecar& a, const AssistSidecar& b) {
  return a.module_id == b.module_id && a.schema_version == b.schema_version &&
         a.payload == b.payload && a.target_track_id == b.target_track_id &&
         a.region_start_ppq == b.region_start_ppq && a.region_end_ppq == b.region_end_ppq;
}

bool warp_map_equal(const WarpMapRef& a, const WarpMapRef& b) {
  return a.id == b.id && a.name == b.name && a.anchors == b.anchors;
}

bool insert_equal(const sonare::mixing::api::Insert& a, const sonare::mixing::api::Insert& b) {
  return a.slot == b.slot && a.processor_name == b.processor_name &&
         a.params_json == b.params_json && a.sidechain_key == b.sidechain_key;
}

bool send_equal(const sonare::mixing::api::Send& a, const sonare::mixing::api::Send& b) {
  return a.id == b.id && a.destination_bus_id == b.destination_bus_id && a.send_db == b.send_db &&
         a.timing == b.timing;
}

bool strip_equal(const sonare::mixing::api::Strip& a, const sonare::mixing::api::Strip& b) {
  if (a.id != b.id || a.input_trim_db != b.input_trim_db || a.fader_db != b.fader_db ||
      a.pan != b.pan || a.width != b.width || a.muted != b.muted || a.soloed != b.soloed ||
      a.solo_safe != b.solo_safe || a.pan_mode != b.pan_mode ||
      a.dual_pan_left != b.dual_pan_left || a.dual_pan_right != b.dual_pan_right ||
      a.polarity_invert_left != b.polarity_invert_left ||
      a.polarity_invert_right != b.polarity_invert_right || a.pan_law != b.pan_law ||
      a.channel_delay_samples != b.channel_delay_samples) {
    return false;
  }
  if (a.inserts.size() != b.inserts.size() || a.sends.size() != b.sends.size()) return false;
  for (size_t i = 0; i < a.inserts.size(); ++i) {
    if (!insert_equal(a.inserts[i], b.inserts[i])) return false;
  }
  for (size_t i = 0; i < a.sends.size(); ++i) {
    if (!send_equal(a.sends[i], b.sends[i])) return false;
  }
  return true;
}

bool bus_equal(const sonare::mixing::api::Bus& a, const sonare::mixing::api::Bus& b) {
  if (a.id != b.id || a.role != b.role || a.inserts.size() != b.inserts.size()) return false;
  for (size_t i = 0; i < a.inserts.size(); ++i) {
    if (!insert_equal(a.inserts[i], b.inserts[i])) return false;
  }
  return true;
}

bool vca_equal(const sonare::mixing::api::VcaGroup& a, const sonare::mixing::api::VcaGroup& b) {
  return a.id == b.id && a.gain_db == b.gain_db && a.members == b.members;
}

bool connection_equal(const sonare::mixing::api::Connection& a,
                      const sonare::mixing::api::Connection& b) {
  return a.source == b.source && a.destination == b.destination;
}

bool scene_equal(const sonare::mixing::api::Scene& a, const sonare::mixing::api::Scene& b) {
  if (a.version != b.version || a.strips.size() != b.strips.size() ||
      a.buses.size() != b.buses.size() || a.vca_groups.size() != b.vca_groups.size() ||
      a.connections.size() != b.connections.size()) {
    return false;
  }
  for (size_t i = 0; i < a.strips.size(); ++i) {
    if (!strip_equal(a.strips[i], b.strips[i])) return false;
  }
  for (size_t i = 0; i < a.buses.size(); ++i) {
    if (!bus_equal(a.buses[i], b.buses[i])) return false;
  }
  for (size_t i = 0; i < a.vca_groups.size(); ++i) {
    if (!vca_equal(a.vca_groups[i], b.vca_groups[i])) return false;
  }
  for (size_t i = 0; i < a.connections.size(); ++i) {
    if (!connection_equal(a.connections[i], b.connections[i])) return false;
  }
  return true;
}

bool tempo_seg_equal(const sonare::transport::TempoSegment& a,
                     const sonare::transport::TempoSegment& b) {
  return a.start_ppq == b.start_ppq && a.bpm == b.bpm && a.start_sample == b.start_sample &&
         a.end_bpm == b.end_bpm && a.end_ppq == b.end_ppq;
}

bool time_sig_seg_equal(const sonare::transport::TimeSignatureSegment& a,
                        const sonare::transport::TimeSignatureSegment& b) {
  return a.start_ppq == b.start_ppq && a.time_sig.numerator == b.time_sig.numerator &&
         a.time_sig.denominator == b.time_sig.denominator;
}

// Full deep equality across all command-affected fields of a Project.
bool project_equal(const Project& a, const Project& b) {
  if (a.sample_rate() != b.sample_rate()) return false;
  if (a.overlap_policy() != b.overlap_policy()) return false;
  if (!scene_equal(a.scene(), b.scene())) return false;

  if (a.tracks().size() != b.tracks().size()) return false;
  for (size_t i = 0; i < a.tracks().size(); ++i) {
    if (!track_equal(a.tracks()[i], b.tracks()[i])) return false;
  }

  if (a.clips().size() != b.clips().size()) return false;
  for (size_t i = 0; i < a.clips().size(); ++i) {
    if (!clip_equal(a.clips()[i], b.clips()[i])) return false;
  }

  if (a.sources().size() != b.sources().size()) return false;
  for (size_t i = 0; i < a.sources().size(); ++i) {
    if (!source_equal(a.sources()[i], b.sources()[i])) return false;
  }

  if (a.markers().size() != b.markers().size()) return false;
  for (size_t i = 0; i < a.markers().size(); ++i) {
    if (a.markers()[i].id != b.markers()[i].id || a.markers()[i].ppq != b.markers()[i].ppq ||
        a.markers()[i].name != b.markers()[i].name) {
      return false;
    }
  }

  if (!annotation_equal(a.annotation(), b.annotation())) return false;

  if (a.tempo_segments().size() != b.tempo_segments().size()) return false;
  for (size_t i = 0; i < a.tempo_segments().size(); ++i) {
    if (!tempo_seg_equal(a.tempo_segments()[i], b.tempo_segments()[i])) return false;
  }

  if (a.time_signatures().size() != b.time_signatures().size()) return false;
  for (size_t i = 0; i < a.time_signatures().size(); ++i) {
    if (!time_sig_seg_equal(a.time_signatures()[i], b.time_signatures()[i])) return false;
  }

  if (a.assist_sidecars().size() != b.assist_sidecars().size()) return false;
  for (size_t i = 0; i < a.assist_sidecars().size(); ++i) {
    if (!sidecar_equal(a.assist_sidecars()[i], b.assist_sidecars()[i])) return false;
  }
  if (a.warp_maps().size() != b.warp_maps().size()) return false;
  for (size_t i = 0; i < a.warp_maps().size(); ++i) {
    if (!warp_map_equal(a.warp_maps()[i], b.warp_maps()[i])) return false;
  }
  return true;
}

// A fixture project with one audio track + source + clip, one MIDI track +
// source + clip, used as the common starting state for round-trip tests.
struct Fixture {
  Project project;
  TrackId audio_track = 0;
  TrackId midi_track = 0;
  SourceId audio_source = 0;
  SourceId midi_source = 0;
  ClipId audio_clip = 0;
  ClipId midi_clip = 0;

  Fixture() {
    AudioSourceRef a;
    a.uri = "file://a.wav";
    a.channel_count = 2;
    audio_source = project.add_audio_source(a);

    MidiSourceRef m;
    m.name = "lead";
    midi_source = project.add_midi_source(m);

    Track at;
    at.name = "audio";
    at.kind = Track::Kind::kAudio;
    audio_track = project.add_track(at);

    Track mt;
    mt.name = "midi";
    mt.kind = Track::Kind::kMidi;
    midi_track = project.add_track(mt);

    EditClip ac;
    ac.track_id = audio_track;
    ac.source_id = audio_source;
    ac.start_ppq = 0.0;
    ac.length_ppq = 1920.0;
    audio_clip = project.add_clip(ac);

    EditClip mc;
    mc.track_id = midi_track;
    mc.source_id = midi_source;
    mc.start_ppq = 0.0;
    mc.length_ppq = 1920.0;
    midi_clip = project.add_clip(mc);
  }
};

// Applies a command directly (not via history), capturing before-state, then
// applies the inverse and asserts the project + content store return to before.
void check_round_trip(Project& project, MidiContentStore& store, EditCommandPtr cmd) {
  const Project before = project;
  const MidiContentStore store_before = store;

  REQUIRE(cmd->apply(project, store));
  EditCommandPtr inverse = cmd->invert(before, store_before);
  REQUIRE(inverse != nullptr);
  REQUIRE(inverse->apply(project, store));

  CHECK(project_equal(project, before));
  CHECK(store.events == store_before.events);
}

// A tiny deterministic PRNG (xorshift32) — NO rand(), fully seeded.
struct Rng {
  uint32_t state;
  explicit Rng(uint32_t seed) : state(seed ? seed : 0x12345678u) {}
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  uint32_t below(uint32_t n) { return next() % n; }
  double unit() { return static_cast<double>(next() % 100000) / 100000.0; }
};

}  // namespace

// ===========================================================================
// apply/invert round-trip per command category
// ===========================================================================

TEST_CASE("Track command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  SECTION("AddTrack / RemoveTrack") {
    Track t;
    t.name = "new";
    t.kind = Track::Kind::kAux;
    check_round_trip(f.project, store, std::make_unique<AddTrack>(t));

    check_round_trip(f.project, store, std::make_unique<RemoveTrack>(f.audio_track));
  }
  SECTION("RenameTrack") {
    check_round_trip(f.project, store, std::make_unique<RenameTrack>(f.audio_track, "renamed"));
  }
  SECTION("SetTrackRoute") {
    check_round_trip(f.project, store,
                     std::make_unique<SetTrackRoute>(f.audio_track, "strip-A", "bus-1"));
  }
  SECTION("SetTrackKind") {
    check_round_trip(f.project, store,
                     std::make_unique<SetTrackKind>(f.audio_track, Track::Kind::kAux));
  }
}

TEST_CASE("RemoveTrack removes owned clips and restores MIDI content on undo", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  EditClip second_midi = *f.project.find_clip(f.midi_clip);
  second_midi.id = 0;
  second_midi.start_ppq = 2400.0;
  const ClipId second_midi_clip = f.project.add_clip(second_midi);
  REQUIRE(second_midi_clip != 0);

  store.events[f.midi_clip] = {
      {120.0, 0x20903C64u, 0u},
      {360.0, 0x20803C00u, 0u},
  };
  store.events[second_midi_clip] = {
      {48.0, 0x20904064u, 0u},
  };

  const Project before = f.project;
  const MidiContentStore store_before = store;

  auto remove = std::make_unique<RemoveTrack>(f.midi_track);
  RemoveTrack* raw = remove.get();
  REQUIRE(remove->apply(f.project, store));

  CHECK_FALSE(f.project.has_track(f.midi_track));
  CHECK_FALSE(f.project.has_clip(f.midi_clip));
  CHECK_FALSE(f.project.has_clip(second_midi_clip));
  CHECK(f.project.has_track(f.audio_track));
  CHECK(f.project.has_clip(f.audio_clip));
  CHECK(store.events.find(f.midi_clip) == store.events.end());
  CHECK(store.events.find(second_midi_clip) == store.events.end());

  EditCommandPtr undo = raw->invert(before, store_before);
  REQUIRE(undo != nullptr);
  REQUIRE(undo->apply(f.project, store));

  CHECK(project_equal(f.project, before));
  CHECK(store.events == store_before.events);
}

TEST_CASE("Clip command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  SECTION("AddClip / RemoveClip") {
    EditClip c;
    c.track_id = f.audio_track;
    c.source_id = f.audio_source;
    c.start_ppq = 2000.0;
    c.length_ppq = 480.0;
    check_round_trip(f.project, store, std::make_unique<AddClip>(c));

    check_round_trip(f.project, store, std::make_unique<RemoveClip>(f.audio_clip));
  }
  SECTION("SplitClip") {
    check_round_trip(f.project, store, std::make_unique<SplitClip>(f.audio_clip, 960.0));
  }
  SECTION("TrimClip") {
    check_round_trip(f.project, store, std::make_unique<TrimClip>(f.audio_clip, 240.0, 960.0));
  }
  SECTION("MoveClip same track") {
    check_round_trip(f.project, store, std::make_unique<MoveClip>(f.midi_clip, 5000.0));
  }
  SECTION("DuplicateClip") {
    check_round_trip(f.project, store, std::make_unique<DuplicateClip>(f.audio_clip, 4000.0));
  }
  SECTION("SetClipGain") {
    check_round_trip(f.project, store, std::make_unique<SetClipGain>(f.audio_clip, 0.5f));
  }
  SECTION("SetClipFade") {
    check_round_trip(
        f.project, store,
        std::make_unique<SetClipFade>(f.audio_clip, ClipFade{120.0, FadeCurve::kLinear},
                                      ClipFade{240.0, FadeCurve::kEqualPower}));
  }
  SECTION("SetClipLoop") {
    check_round_trip(f.project, store,
                     std::make_unique<SetClipLoop>(f.audio_clip, LoopMode::kLoop, 480.0));
  }
  SECTION("SetClipWarpRef") {
    check_round_trip(f.project, store, std::make_unique<SetClipWarpRef>(f.audio_clip, 77));
  }
  SECTION("SetWarpMap adds a project warp map") {
    WarpMapRef map;
    map.id = 77;
    map.name = "manual warp";
    map.anchors = {{0.0, 0.0}, {48000.0, 44100.0}};
    check_round_trip(f.project, store, std::make_unique<SetWarpMap>(map));
  }
  SECTION("SetWarpMap replaces an existing project warp map") {
    WarpMapRef map;
    map.id = 77;
    map.name = "manual warp";
    map.anchors = {{0.0, 0.0}, {48000.0, 44100.0}};
    REQUIRE(f.project.set_warp_map(map));
    map.name = "edited warp";
    map.anchors.push_back({96000.0, 88200.0});
    check_round_trip(f.project, store, std::make_unique<SetWarpMap>(map));
  }
  SECTION("RemoveWarpMap") {
    WarpMapRef map;
    map.id = 77;
    map.name = "manual warp";
    map.anchors = {{0.0, 0.0}, {48000.0, 44100.0}};
    REQUIRE(f.project.set_warp_map(map));
    check_round_trip(f.project, store, std::make_unique<RemoveWarpMap>(77));
  }
  SECTION("SetClipSource") {
    AudioSourceRef replacement;
    replacement.uri = "file://replacement.wav";
    replacement.channel_count = 2;
    const SourceId replacement_id = f.project.add_audio_source(replacement);
    check_round_trip(f.project, store,
                     std::make_unique<SetClipSource>(f.audio_clip, replacement_id));
  }
}

TEST_CASE("SetClipSource retargets one clip without replacing the shared source", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  EditClip second = *f.project.find_clip(f.audio_clip);
  second.id = 0;
  second.start_ppq = 2400.0;
  const ClipId second_clip = f.project.add_clip(second);
  REQUIRE(second_clip != 0);

  AudioSourceRef replacement;
  replacement.uri = "file://replacement.wav";
  replacement.channel_count = 2;
  const SourceId replacement_id = f.project.add_audio_source(replacement);

  REQUIRE(SetClipSource(f.audio_clip, replacement_id).apply(f.project, store));
  REQUIRE(f.project.find_clip(f.audio_clip)->source_id == replacement_id);
  REQUIRE(f.project.find_clip(second_clip)->source_id == f.audio_source);
  REQUIRE(std::get<AudioSourceRef>(*f.project.find_source(f.audio_source)).uri == "file://a.wav");
}

TEST_CASE("SetClipSource rejects missing or wrong-kind sources", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  REQUIRE_FALSE(SetClipSource(f.audio_clip, 999999u).apply(f.project, store));
  REQUIRE_FALSE(SetClipSource(f.audio_clip, f.midi_source).apply(f.project, store));
  REQUIRE(f.project.find_clip(f.audio_clip)->source_id == f.audio_source);
}

TEST_CASE("SplitClip partitions MIDI content instead of duplicating it", "[arrangement]") {
  Fixture f;
  MidiContentStore store;
  store.events[f.midi_clip] = {
      {100.0, 0x20903C64u, 0u},
      {1000.0, 0x20803C00u, 0u},
  };
  const Project before = f.project;
  const MidiContentStore store_before = store;

  auto split = std::make_unique<SplitClip>(f.midi_clip, 960.0);
  SplitClip* raw = split.get();
  REQUIRE(split->apply(f.project, store));
  const ClipId right_id = raw->new_clip_id();
  REQUIRE(right_id != 0);

  REQUIRE(store.events[f.midi_clip].size() == 1);
  REQUIRE(store.events[f.midi_clip][0].ppq == 100.0);
  REQUIRE(store.events[right_id].size() == 1);
  REQUIRE(store.events[right_id][0].ppq == 1000.0);

  EditCommandPtr undo = raw->invert(before, store_before);
  REQUIRE(undo != nullptr);
  REQUIRE(undo->apply(f.project, store));
  REQUIRE(project_equal(f.project, before));
  REQUIRE(store.events == store_before.events);
}

TEST_CASE("Clip placement commands preserve overlap invariants and fail atomically",
          "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  auto add_audio_clip = [&](double start, double length) {
    EditClip c;
    c.track_id = f.audio_track;
    c.source_id = f.audio_source;
    c.start_ppq = start;
    c.length_ppq = length;
    return f.project.add_clip(c);
  };

  SECTION("TrimClip rejects an overlap without mutating") {
    REQUIRE(add_audio_clip(2000.0, 480.0) != 0);
    const Project before = f.project;
    TrimClip trim(f.audio_clip, 1000.0, 1200.0);  // would overlap [2000, 2480)
    REQUIRE_FALSE(trim.apply(f.project, store));
    CHECK(project_equal(f.project, before));
  }

  SECTION("MoveClip rejects an overlap without mutating") {
    REQUIRE(add_audio_clip(3000.0, 480.0) != 0);
    const Project before = f.project;
    MoveClip move(f.audio_clip, 3200.0);  // would overlap [3000, 3480)
    REQUIRE_FALSE(move.apply(f.project, store));
    CHECK(project_equal(f.project, before));
  }

  SECTION("SplitClip checks the right-hand placement before shortening the left") {
    f.project.set_overlap_policy(OverlapPolicy::kAllow);
    REQUIRE(add_audio_clip(1000.0, 200.0) != 0);
    f.project.set_overlap_policy(OverlapPolicy::kDisallow);
    const Project before = f.project;
    SplitClip split(f.audio_clip, 960.0);  // right side [960, 1920) overlaps blocker.
    REQUIRE_FALSE(split.apply(f.project, store));
    CHECK(project_equal(f.project, before));
  }
}

TEST_CASE("Source command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  SECTION("AttachAudioSource") {
    AudioSourceRef a;
    a.uri = "file://b.wav";
    check_round_trip(f.project, store, std::make_unique<AttachAudioSource>(a));
  }
  SECTION("AttachMidiSource") {
    MidiSourceRef m;
    m.name = "bass";
    check_round_trip(f.project, store, std::make_unique<AttachMidiSource>(m));
  }
  SECTION("ReplaceSource") {
    AudioSourceRef repl;
    repl.uri = "file://replaced.wav";
    repl.channel_count = 1;
    check_round_trip(f.project, store,
                     std::make_unique<ReplaceSource>(f.audio_source, ClipSource{repl}));
  }
}

TEST_CASE("Timeline command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  SECTION("SetSampleRate") {
    check_round_trip(f.project, store, std::make_unique<SetSampleRate>(48000.0));
  }
  SECTION("SetOverlapPolicy") {
    check_round_trip(f.project, store, std::make_unique<SetOverlapPolicy>(OverlapPolicy::kAllow));
  }
  SECTION("SetScene") {
    sonare::mixing::api::Scene scene;
    scene.version = 1;
    sonare::mixing::api::Strip strip;
    strip.id = "lead";
    strip.input_trim_db = -1.0f;
    strip.fader_db = -4.5f;
    strip.pan = 0.25f;
    strip.width = 1.2f;
    strip.muted = true;
    strip.solo_safe = true;
    strip.channel_delay_samples = 17;
    strip.inserts.push_back(
        {sonare::mixing::api::InsertSlot::PreFader, "eq.parametric", "{\"gain\":2}"});
    sonare::mixing::api::Send send;
    send.id = "verb-send";
    send.destination_bus_id = "verb";
    send.send_db = -9.0f;
    send.timing = sonare::mixing::api::SendTiming::PreFader;
    strip.sends.push_back(send);
    scene.strips.push_back(strip);
    sonare::mixing::api::Bus bus("verb", "aux");
    bus.inserts.push_back(
        {sonare::mixing::api::InsertSlot::PostFader, "effects.reverb.plate", "{}"});
    scene.buses.push_back(bus);
    sonare::mixing::api::VcaGroup vca;
    vca.id = "vox";
    vca.gain_db = -2.0f;
    vca.members = {"lead"};
    scene.vca_groups.push_back(vca);
    scene.connections.push_back({"lead", "verb"});
    check_round_trip(f.project, store, std::make_unique<SetScene>(scene));
  }
  SECTION("SetMarker new") {
    check_round_trip(f.project, store, std::make_unique<SetMarker>(0, 480.0, "intro"));
  }
  SECTION("SetMarker existing") {
    const uint32_t mid = f.project.add_marker(0.0, "old");
    check_round_trip(f.project, store, std::make_unique<SetMarker>(mid, 960.0, "new"));
  }
  SECTION("SetAnnotation") {
    ProjectAnnotation ann;
    ann.tempo_confidence = 0.9f;
    ChordSymbol c;
    c.root_pc = 0;
    c.quality = ChordQuality::kMajor;
    ann.chords.push_back(c);
    check_round_trip(f.project, store, std::make_unique<SetAnnotation>(ann));
  }
  SECTION("SetTempoSegment") {
    std::vector<sonare::transport::TempoSegment> segs;
    sonare::transport::TempoSegment s;
    s.start_ppq = 0.0;
    s.bpm = 140.0;
    segs.push_back(s);
    check_round_trip(f.project, store, std::make_unique<SetTempoSegment>(segs));
  }
  SECTION("SetTimeSignatureSegment") {
    std::vector<sonare::transport::TimeSignatureSegment> segs;
    sonare::transport::TimeSignatureSegment s;
    s.start_ppq = 0.0;
    s.time_sig.numerator = 3;
    s.time_sig.denominator = 4;
    segs.push_back(s);
    check_round_trip(f.project, store, std::make_unique<SetTimeSignatureSegment>(segs));
  }
  SECTION("SetHarmonySegment") {
    std::vector<ChordSymbol> chords;
    ChordSymbol c;
    c.root_pc = 7;
    c.quality = ChordQuality::kDominant;
    chords.push_back(c);
    check_round_trip(f.project, store, std::make_unique<SetHarmonySegment>(chords));
  }
}

TEST_CASE("SetSampleRate rejects invalid values atomically", "[arrangement]") {
  Fixture f;
  MidiContentStore store;
  const Project before = f.project;

  CHECK_FALSE(SetSampleRate(0.0).apply(f.project, store));
  CHECK(project_equal(f.project, before));

  CHECK_FALSE(SetSampleRate(-1.0).apply(f.project, store));
  CHECK(project_equal(f.project, before));
}

TEST_CASE("Automation command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  auto make_lane = [](uint32_t param) {
    sonare::automation::AutomationLane lane(param);
    lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                     {960.0, 1.0f, sonare::automation::CurveType::Linear}});
    return lane;
  };

  SECTION("AddAutomationLane / RemoveAutomationLane") {
    check_round_trip(f.project, store,
                     std::make_unique<AddAutomationLane>(f.audio_track, make_lane(10)));

    // Seed a lane then remove it.
    f.project.find_track_mutable(f.audio_track)->automation_lanes.push_back(make_lane(11));
    check_round_trip(f.project, store, std::make_unique<RemoveAutomationLane>(f.audio_track, 0));
  }
  SECTION("EditAutomationLane") {
    f.project.find_track_mutable(f.audio_track)->automation_lanes.push_back(make_lane(20));
    check_round_trip(f.project, store,
                     std::make_unique<EditAutomationLane>(f.audio_track, 0, make_lane(21)));
  }
}

TEST_CASE("MIDI content command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;
  store.events[f.midi_clip] = {{0.0, 0x90, 60}, {480.0, 0x80, 60}};

  SECTION("ReplaceMidiClipEvents") {
    MidiClipEventList ev = {{0.0, 0x90, 64}, {240.0, 0x80, 64}, {960.0, 0x90, 67}};
    check_round_trip(f.project, store, std::make_unique<ReplaceMidiClipEvents>(f.midi_clip, ev));
  }
  SECTION("PatchMidiClip add and remove") {
    MidiClipPatch patch;
    patch.clip_id = f.midi_clip;
    patch.add = {{960.0, 0x90, 72}};
    patch.remove = {{0.0, 0x90, 60}};
    check_round_trip(f.project, store, std::make_unique<PatchMidiClip>(patch));
  }
}

TEST_CASE("Assist sidecar command round-trips", "[arrangement]") {
  Fixture f;
  MidiContentStore store;

  SECTION("SetAssistSidecar new") {
    AssistSidecar s;
    s.module_id = "midi-sketch";
    s.schema_version = 2;
    s.payload = {1, 2, 3};
    s.target_track_id = f.midi_track;
    check_round_trip(f.project, store, std::make_unique<SetAssistSidecar>(s));
  }
  SECTION("SetAssistSidecar update existing") {
    AssistSidecar s;
    s.module_id = "midi-sketch";
    s.schema_version = 1;
    s.payload = {1};
    s.target_track_id = f.midi_track;
    f.project.add_assist_sidecar(s);

    AssistSidecar updated = s;
    updated.schema_version = 5;
    updated.payload = {9, 9, 9};
    check_round_trip(f.project, store, std::make_unique<SetAssistSidecar>(updated));
  }
}

// ===========================================================================
// Undo / redo stack behaviour
// ===========================================================================

TEST_CASE("History undo/redo restores state and stack depths", "[arrangement]") {
  EditHistory h{Fixture{}.project};
  const Project initial = h.project();

  CHECK_FALSE(h.can_undo());
  CHECK_FALSE(h.can_redo());

  const TrackId track = h.project().tracks().front().id;
  REQUIRE(h.apply(std::make_unique<RenameTrack>(track, "step1")));
  REQUIRE(h.apply(std::make_unique<SetClipGain>(h.project().clips().front().id, 0.25f)));

  CHECK(h.undo_depth() == 2);
  CHECK(h.can_undo());
  CHECK_FALSE(h.can_redo());

  REQUIRE(h.undo());  // undo gain
  REQUIRE(h.undo());  // undo rename
  CHECK(project_equal(h.project(), initial));
  CHECK(h.redo_depth() == 2);

  REQUIRE(h.redo());  // redo rename
  CHECK(h.project().tracks().front().name == "step1");
  REQUIRE(h.redo());  // redo gain
  CHECK(h.project().clips().front().gain == 0.25f);

  CHECK_FALSE(h.can_redo());

  // A new apply clears the redo stack.
  REQUIRE(h.undo());
  CHECK(h.can_redo());
  REQUIRE(h.apply(std::make_unique<RenameTrack>(track, "fork")));
  CHECK_FALSE(h.can_redo());
}

TEST_CASE("History undo/redo keeps ids stable for Add commands", "[arrangement]") {
  EditHistory h;
  AudioSourceRef a;
  a.uri = "file://x.wav";
  const SourceId sid = h.project().add_audio_source(a);

  Track t;
  t.kind = Track::Kind::kAudio;
  const TrackId tid = h.project().add_track(t);

  EditClip c;
  c.track_id = tid;
  c.source_id = sid;
  c.length_ppq = 480.0;
  c.start_ppq = 0.0;

  REQUIRE(h.apply(std::make_unique<AddClip>(c)));
  const ClipId first_id = h.project().clips().back().id;

  REQUIRE(h.undo());
  CHECK(h.project().clips().empty());

  REQUIRE(h.redo());
  REQUIRE(h.project().clips().size() == 1);
  // Redo re-allocates the SAME stable id.
  CHECK(h.project().clips().back().id == first_id);
}

// ===========================================================================
// Deterministic randomized replay
// ===========================================================================

namespace {

// Builds a deterministic command sequence from a seed. Operates only on the two
// fixture tracks/clips so commands stay valid.
std::vector<EditCommandPtr> build_sequence(const Fixture& f, uint32_t seed, int count) {
  Rng rng(seed);
  std::vector<EditCommandPtr> cmds;
  cmds.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    switch (rng.below(6)) {
      case 0:
        cmds.push_back(
            std::make_unique<RenameTrack>(f.audio_track, "n" + std::to_string(rng.below(1000))));
        break;
      case 1:
        cmds.push_back(std::make_unique<SetClipGain>(f.audio_clip, static_cast<float>(rng.unit())));
        break;
      case 2:
        cmds.push_back(
            std::make_unique<MoveClip>(f.midi_clip, static_cast<double>(rng.below(8000))));
        break;
      case 3:
        cmds.push_back(std::make_unique<SetMarker>(0, static_cast<double>(rng.below(4000)),
                                                   "m" + std::to_string(rng.below(1000))));
        break;
      case 4: {
        ProjectAnnotation ann;
        ann.tempo_confidence = static_cast<float>(rng.unit());
        cmds.push_back(std::make_unique<SetAnnotation>(ann));
        break;
      }
      case 5:
        cmds.push_back(std::make_unique<SetClipFade>(
            f.audio_clip, ClipFade{static_cast<double>(rng.below(200)), FadeCurve::kLinear},
            ClipFade{static_cast<double>(rng.below(200)), FadeCurve::kExponential}));
        break;
    }
  }
  return cmds;
}

}  // namespace

TEST_CASE("Deterministic replay of a seeded command sequence", "[arrangement]") {
  const uint32_t seed = 0xC0FFEEu;
  const int count = 64;

  auto run = [&]() {
    Fixture f;
    EditHistory h{f.project};
    // The fixture allocates ids deterministically, so a fresh fixture yields the
    // same ids each run; the sequence is keyed to those ids.
    auto cmds = build_sequence(f, seed, count);
    for (auto& c : cmds) {
      REQUIRE(h.apply(std::move(c)));
    }
    return h.project();
  };

  const Project a = run();
  const Project b = run();
  CHECK(project_equal(a, b));
}

TEST_CASE("Full undo of a seeded sequence returns to the initial state", "[arrangement]") {
  Fixture f;
  EditHistory h{f.project};
  const Project initial = h.project();

  auto cmds = build_sequence(f, 0x1234u, 40);
  int applied = 0;
  for (auto& c : cmds) {
    if (h.apply(std::move(c))) ++applied;
  }
  REQUIRE(applied > 0);

  for (int i = 0; i < applied; ++i) {
    REQUIRE(h.undo());
  }
  CHECK(project_equal(h.project(), initial));

  // Redo everything and confirm we land on the post-sequence state again.
  const Project after_undo_then_redo = [&]() {
    for (int i = 0; i < applied; ++i) {
      REQUIRE(h.redo());
    }
    return h.project();
  }();
  // Re-running the same sequence on a fresh history must match the redo result.
  Fixture f2;
  EditHistory h2{f2.project};
  auto cmds2 = build_sequence(f2, 0x1234u, 40);
  for (auto& c : cmds2) {
    h2.apply(std::move(c));
  }
  CHECK(project_equal(after_undo_then_redo, h2.project()));
}
