/// @file edit_model_test.cpp
/// @brief headless arrangement project model tests.

#include "arrangement/edit_model.h"

#include <catch2/catch_test_macros.hpp>
#include <set>

#include "arrangement/edit_source.h"

using namespace sonare::arrangement;

namespace {

// Adds an audio source + audio track and returns {track_id, source_id}.
std::pair<TrackId, SourceId> make_audio_track(Project& p, const std::string& name = "audio") {
  AudioSourceRef ref;
  ref.uri = "file://" + name + ".wav";
  ref.channel_count = 2;
  ref.sample_rate_hint = 48000.0;
  const SourceId sid = p.add_audio_source(ref);

  Track t;
  t.name = name;
  t.kind = Track::Kind::kAudio;
  const TrackId tid = p.add_track(t);
  return {tid, sid};
}

}  // namespace

TEST_CASE("Project constructs with deterministic defaults", "[arrangement]") {
  Project p;
  CHECK(p.sample_rate() == 22050.0);
  CHECK(p.project_version() == kProjectVersion);
  CHECK(p.overlap_policy() == OverlapPolicy::kDisallow);
  CHECK(p.tracks().empty());
  CHECK(p.clips().empty());
  CHECK(p.sources().empty());
  CHECK(p.markers().empty());
  CHECK(p.assist_sidecars().empty());
  // The mixer scene is held as pure data even with mixing runtime disabled.
  CHECK(p.scene().version == 1);
}

TEST_CASE("Ids are unique and monotonic across tracks, clips, sources", "[arrangement]") {
  Project p;

  AudioSourceRef a;
  const SourceId s1 = p.add_audio_source(a);
  MidiSourceRef m;
  const SourceId s2 = p.add_midi_source(m);
  const SourceId s3 = p.add_audio_source(a);
  CHECK(s1 != 0);
  CHECK(s2 == s1 + 1);
  CHECK(s3 == s2 + 1);

  const TrackId t1 = p.add_track(Track{});
  const TrackId t2 = p.add_track(Track{});
  CHECK(t1 != 0);
  CHECK(t2 == t1 + 1);

  // Source ids and track ids use independent counters.
  std::set<SourceId> source_ids{s1, s2, s3};
  CHECK(source_ids.size() == 3);

  // The allocated source id is reflected inside the variant.
  const ClipSource* found = p.find_source(s2);
  REQUIRE(found != nullptr);
  CHECK(source_kind(*found) == SourceKind::kMidi);
  CHECK(source_id(*found) == s2);
}

TEST_CASE("Clip add enforces referential integrity", "[arrangement]") {
  Project p;
  auto [tid, sid] = make_audio_track(p);

  SECTION("valid clip is accepted") {
    EditClip c;
    c.track_id = tid;
    c.source_id = sid;
    c.start_ppq = 0.0;
    c.length_ppq = 960.0;
    const ClipId cid = p.add_clip(c);
    CHECK(cid != 0);
    CHECK(p.has_clip(cid));
    REQUIRE(p.find_clip(cid) != nullptr);
    CHECK(p.find_clip(cid)->end_ppq() == 960.0);
  }

  SECTION("unknown track is rejected") {
    EditClip c;
    c.track_id = 9999;
    c.source_id = sid;
    c.length_ppq = 480.0;
    CHECK(p.add_clip(c) == 0);
  }

  SECTION("unknown source is rejected") {
    EditClip c;
    c.track_id = tid;
    c.source_id = 9999;
    c.length_ppq = 480.0;
    CHECK(p.add_clip(c) == 0);
  }
}

TEST_CASE("Clip PPQ range validation", "[arrangement]") {
  Project p;
  const auto track_source = make_audio_track(p);
  const TrackId tid = track_source.first;
  const SourceId sid = track_source.second;

  auto base = [&] {
    EditClip c;
    c.track_id = tid;
    c.source_id = sid;
    c.start_ppq = 0.0;
    c.length_ppq = 480.0;
    return c;
  };

  SECTION("zero length is rejected") {
    EditClip c = base();
    c.length_ppq = 0.0;
    CHECK(p.add_clip(c) == 0);
  }

  SECTION("negative length is rejected") {
    EditClip c = base();
    c.length_ppq = -10.0;
    CHECK(p.add_clip(c) == 0);
  }

  SECTION("negative start is rejected") {
    EditClip c = base();
    c.start_ppq = -1.0;
    CHECK(p.add_clip(c) == 0);
  }

  SECTION("negative source offset is rejected") {
    EditClip c = base();
    c.source_offset_ppq = -1.0;
    CHECK(p.add_clip(c) == 0);
  }

  SECTION("looping requires positive loop length") {
    EditClip c = base();
    c.loop_mode = LoopMode::kLoop;
    c.loop_length_ppq = 0.0;
    CHECK(p.add_clip(c) == 0);
    c.loop_length_ppq = 240.0;
    CHECK(p.add_clip(c) != 0);
  }
}

TEST_CASE("Clip overlap policy on the same track", "[arrangement]") {
  Project p;
  auto [tid, sid] = make_audio_track(p);

  EditClip first;
  first.track_id = tid;
  first.source_id = sid;
  first.start_ppq = 0.0;
  first.length_ppq = 960.0;
  const ClipId c1 = p.add_clip(first);
  REQUIRE(c1 != 0);

  SECTION("overlapping clip rejected under default disallow policy") {
    EditClip overlap;
    overlap.track_id = tid;
    overlap.source_id = sid;
    overlap.start_ppq = 480.0;  // overlaps [0, 960)
    overlap.length_ppq = 480.0;
    CHECK(p.add_clip(overlap) == 0);
  }

  SECTION("adjacent (touching) clip is allowed") {
    EditClip adjacent;
    adjacent.track_id = tid;
    adjacent.source_id = sid;
    adjacent.start_ppq = 960.0;  // touches end of first; not an overlap
    adjacent.length_ppq = 480.0;
    CHECK(p.add_clip(adjacent) != 0);
  }

  SECTION("overlap allowed when policy is kAllow") {
    p.set_overlap_policy(OverlapPolicy::kAllow);
    EditClip overlap;
    overlap.track_id = tid;
    overlap.source_id = sid;
    overlap.start_ppq = 480.0;
    overlap.length_ppq = 480.0;
    CHECK(p.add_clip(overlap) != 0);
  }

  SECTION("same span on a different track is not an overlap") {
    auto [tid2, sid2] = make_audio_track(p, "audio2");
    EditClip other;
    other.track_id = tid2;
    other.source_id = sid2;
    other.start_ppq = 0.0;
    other.length_ppq = 960.0;
    CHECK(p.add_clip(other) != 0);
  }

  SECTION("clip_overlaps query respects ignore id and span") {
    CHECK_FALSE(p.clip_overlaps(tid, 0.0, 960.0, c1));  // ignoring itself
    CHECK(p.clip_overlaps(tid, 100.0, 100.0));          // inside first
  }
}

TEST_CASE("Audio and MIDI sources use the same EditClip API", "[arrangement]") {
  Project p;

  // Audio source + track.
  AudioSourceRef audio;
  audio.uri = "file://drums.wav";
  const SourceId audio_sid = p.add_audio_source(audio);
  Track at;
  at.kind = Track::Kind::kAudio;
  const TrackId audio_tid = p.add_track(at);

  // MIDI source + track.
  MidiSourceRef midi;
  midi.name = "lead";
  const SourceId midi_sid = p.add_midi_source(midi);
  Track mt;
  mt.kind = Track::Kind::kMidi;
  const TrackId midi_tid = p.add_track(mt);

  // The identical EditClip struct/path places both.
  EditClip audio_clip;
  audio_clip.track_id = audio_tid;
  audio_clip.source_id = audio_sid;
  audio_clip.length_ppq = 1920.0;
  const ClipId audio_cid = p.add_clip(audio_clip);

  EditClip midi_clip;
  midi_clip.track_id = midi_tid;
  midi_clip.source_id = midi_sid;
  midi_clip.length_ppq = 1920.0;
  const ClipId midi_cid = p.add_clip(midi_clip);

  REQUIRE(audio_cid != 0);
  REQUIRE(midi_cid != 0);

  // Kind is decided by the referenced source, not by a separate clip type.
  CHECK(source_kind(*p.find_source(p.find_clip(audio_cid)->source_id)) == SourceKind::kAudio);
  CHECK(source_kind(*p.find_source(p.find_clip(midi_cid)->source_id)) == SourceKind::kMidi);
}

TEST_CASE("Markers own their name storage", "[arrangement]") {
  Project p;
  const uint32_t m1 = p.add_marker(0.0, "intro");
  const uint32_t m2 = p.add_marker(1920.0, "verse");
  CHECK(m1 != 0);
  CHECK(m2 == m1 + 1);
  REQUIRE(p.markers().size() == 2);
  CHECK(p.markers()[0].name == "intro");
  CHECK(p.markers()[1].name == "verse");
}

TEST_CASE("Annotation carries chord-symbol granularity", "[arrangement]") {
  Project p;
  ChordSymbol c;
  c.start_ppq = 0.0;
  c.end_ppq = 1920.0;
  c.root_pc = 7;  // G
  c.quality = ChordQuality::kDominant;
  c.extensions = {7, 9};
  c.slash_bass_pc = 11;  // /B
  c.roman_numeral = "V7/ii";
  c.modulation_boundary = true;
  p.annotation().chords.push_back(c);
  p.annotation().tempo_confidence = 0.83f;

  REQUIRE(p.annotation().chords.size() == 1);
  const ChordSymbol& got = p.annotation().chords[0];
  CHECK(got.root_pc == 7);
  CHECK(got.quality == ChordQuality::kDominant);
  CHECK(got.extensions == std::vector<uint8_t>{7, 9});
  CHECK(got.slash_bass_pc == 11);
  CHECK(got.roman_numeral == "V7/ii");
  CHECK(got.modulation_boundary);
}

TEST_CASE("Assist sidecar stored opaquely by id", "[arrangement]") {
  Project p;
  AssistSidecar s;
  s.module_id = "midi-sketch";
  s.schema_version = 3;
  s.payload = {0x01, 0x02, 0x03};
  s.target_track_id = 5;
  p.add_assist_sidecar(s);

  REQUIRE(p.assist_sidecars().size() == 1);
  CHECK(p.assist_sidecars()[0].module_id == "midi-sketch");
  CHECK(p.assist_sidecars()[0].schema_version == 3);
  CHECK(p.assist_sidecars()[0].payload == std::vector<uint8_t>{0x01, 0x02, 0x03});
}

TEST_CASE("Track channel strip ref links to a mixing Scene strip id", "[arrangement]") {
  Project p;
  // Pure-data scene mutation works without the mixing runtime.
  sonare::mixing::api::Strip strip;
  strip.id = "strip-A";
  p.scene().strips.push_back(strip);

  Track t;
  t.channel_strip_ref = "strip-A";
  const TrackId tid = p.add_track(t);
  REQUIRE(p.find_track(tid) != nullptr);
  CHECK(p.find_track(tid)->channel_strip_ref == "strip-A");
  CHECK(p.scene().strips.front().id == p.find_track(tid)->channel_strip_ref);
}
