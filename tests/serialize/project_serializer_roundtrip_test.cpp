/// @file project_serializer_roundtrip_test.cpp
/// @brief project serializer byte-equality, round-trip fidelity, unknown-field-ignore, and
///        AssistSidecar lossless tests for the project serializer.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer.h"
#include "transport/tempo_map.h"
#include "util/json.h"

using namespace sonare;
using namespace sonare::arrangement;
using sonare::serialize::project_from_json;
using sonare::serialize::project_to_json;

namespace {

// Builds a representative project exercising every serialized field group:
// an audio clip + a MIDI clip + an automation lane + markers + a rich
// HarmonicTimeline (extensions / slash bass / Roman) + a mixer Scene + two
// AssistSidecars (one with a binary payload).
struct Fixture {
  Project project;
  MidiContentStore midi;
};

Fixture make_fixture() {
  Fixture f;
  Project& p = f.project;
  p.set_sample_rate(48000.0);
  p.set_overlap_policy(OverlapPolicy::kAllow);

  // Tempo + time signature.
  transport::TempoSegment seg;
  seg.start_ppq = 0.0;
  seg.bpm = 128.5;
  seg.end_bpm = 140.25;
  p.set_tempo_segments({seg});
  transport::TimeSignatureSegment ts;
  ts.start_ppq = 0.0;
  ts.time_sig.numerator = 7;
  ts.time_sig.denominator = 8;
  p.set_time_signatures({ts});

  // Audio source + MIDI source.
  AudioSourceRef audio;
  audio.uri = "file:///host/local/path with spaces & \"quotes\".wav";
  audio.channel_count = 2;
  audio.sample_rate_hint = 44100.0;
  audio.storage_handle_id = 9;
  const SourceId audio_sid = p.add_audio_source(audio);

  MidiSourceRef midi_src;
  midi_src.name = "lead synth";
  midi_src.channel_hint = 3;
  const SourceId midi_sid = p.add_midi_source(midi_src);

  // Audio track with an automation lane.
  Track atrack;
  atrack.name = "Audio";
  atrack.kind = Track::Kind::kAudio;
  atrack.channel_strip_ref = "strip.audio";
  atrack.output_target = "bus.main";
  automation::AutomationLane lane(42);
  lane.set_points({{0.0, 0.1f, automation::CurveType::Linear},
                   {480.0, 0.9f, automation::CurveType::SCurve},
                   {960.0, 0.25f, automation::CurveType::Exponential}});
  atrack.automation_lanes.push_back(lane);
  const TrackId atid = p.add_track(atrack);

  Track mtrack;
  mtrack.name = "MIDI";
  mtrack.kind = Track::Kind::kMidi;
  mtrack.channel_strip_ref = "strip.midi";
  const TrackId mtid = p.add_track(mtrack);

  // Audio clip with fades + loop + warp ref.
  EditClip aclip;
  aclip.track_id = atid;
  aclip.source_id = audio_sid;
  aclip.start_ppq = 0.0;
  aclip.length_ppq = 1920.0;
  aclip.source_offset_ppq = 120.0;
  aclip.gain = 0.75f;
  aclip.fade_in = {64.0, FadeCurve::kEqualPower};
  aclip.fade_out = {128.0, FadeCurve::kLogarithmic};
  aclip.loop_mode = LoopMode::kLoop;
  aclip.loop_length_ppq = 960.0;
  aclip.loop_crossfade_ppq = 48.0;
  aclip.warp_ref_id = 5;
  aclip.warp_mode = WarpMode::kTempoSync;
  aclip.takes = {{1, 0, 120.0, "take A"}, {2, audio_sid, 360.0, "take B"}};
  aclip.active_take_id = 1;
  aclip.comp_segments = {{0.0, 480.0, 1}, {480.0, 960.0, 2}};
  const ClipId aclip_id = p.add_clip(aclip);

  WarpMapRef warp;
  warp.id = 5;
  warp.name = "audio warp";
  warp.anchors = {{0.0, 0.0}, {48000.0, 44100.0}, {96000.0, 90000.0}};
  REQUIRE(p.set_warp_map(warp));

  // MIDI clip.
  EditClip mclip;
  mclip.track_id = mtid;
  mclip.source_id = midi_sid;
  mclip.start_ppq = 0.0;
  mclip.length_ppq = 960.0;
  const ClipId mclip_id = p.add_clip(mclip);

  // MIDI content store for the MIDI clip.
  MidiClipEventList events;
  events.push_back({0.0, 0x90403C, 0x00000040});
  events.push_back({240.0, 0x80403C, 0x00000000});
  events.push_back({480.0, 0x90443C, 0x0000007F});
  f.midi.events[mclip_id] = events;
  (void)aclip_id;

  // Markers (owned names), including a structured key-signature marker so the
  // kind / key-signature fields are exercised by the round-trip.
  p.add_marker(0.0, "Intro");
  p.add_marker(1920.0, "Verse \"A\"");
  p.add_marker(3840.0, "Bb major", /*kind=*/4, /*key_fifths=*/-2, /*key_minor=*/false);
  p.add_marker(5760.0, "lyric", /*kind=*/2);

  // Rich harmonic timeline.
  KeySegment key;
  key.start_ppq = 0.0;
  key.end_ppq = 1920.0;
  key.tonic_pc = 9;  // A
  key.mode = KeyMode::kMinor;
  p.annotation().keys.push_back(key);

  ChordSymbol c1;
  c1.start_ppq = 0.0;
  c1.end_ppq = 480.0;
  c1.root_pc = 9;
  c1.quality = ChordQuality::kMinor;
  c1.extensions = {7, 9, 11};
  c1.slash_bass_pc = 4;  // /E
  c1.roman_numeral = "i9";
  c1.modulation_boundary = true;
  p.annotation().chords.push_back(c1);

  ChordSymbol c2;
  c2.start_ppq = 480.0;
  c2.end_ppq = 960.0;
  c2.root_pc = 4;
  c2.quality = ChordQuality::kDominant;
  c2.extensions = {7, 13};
  c2.roman_numeral = "V7/iv";
  p.annotation().chords.push_back(c2);

  p.annotation().tempo_confidence = 0.875f;
  p.annotation().sections.push_back({0.0, 960.0, "Verse"});
  p.annotation().onsets.push_back({12.5, 0.9f});

  // Mixer Scene.
  mixing::api::Scene& scene = p.scene();
  scene.version = 1;
  mixing::api::Strip s;
  s.id = "strip.audio";
  s.fader_db = -3.5f;
  s.pan = 0.25f;
  s.muted = false;
  // Surround + VCA persistence (regression guard: these once round-tripped to
  // their defaults because the project serializer's local scene walker dropped
  // them).
  s.vca_offset_db = -1.25f;
  s.source_layout = ChannelLayout::FivePointOne;
  s.surround_pan.azimuth = 0.5f;
  s.surround_pan.divergence = 0.3f;
  s.surround_pan.lfe = 0.2f;
  s.surround_pan.distance = 0.8f;
  mixing::api::Insert strip_insert;
  strip_insert.slot = mixing::api::InsertSlot::PreFader;
  strip_insert.processor_name = "sonare.eq";
  strip_insert.params_json = "{\"gainDb\":-1.5}";
  s.inserts.push_back(strip_insert);
  mixing::api::Send send;
  send.id = "send.reverb";
  send.destination_bus_id = "bus.fx";
  send.send_db = -6.0f;
  send.timing = mixing::api::SendTiming::PostFader;
  s.sends.push_back(send);
  scene.strips.push_back(s);
  mixing::api::Bus master("bus.main", "master");
  master.layout = ChannelLayout::FivePointOne;  // surround bus layout must persist
  // Bus output trim / width / polarity persistence (regression guard: these once
  // round-tripped to their defaults because the project serializer's bus walker
  // dropped them, so a reloaded project rendered differently from the saved one).
  master.input_trim_db = -2.5f;
  master.width = 0.75f;
  master.polarity_invert_left = true;
  mixing::api::Insert master_insert;
  master_insert.slot = mixing::api::InsertSlot::PostFader;
  master_insert.processor_name = "sonare.compressor";
  master_insert.params_json = "{\"thresholdDb\":-12}";
  master_insert.sidechain_key = "strip.audio";
  master.inserts.push_back(master_insert);
  scene.buses.push_back(master);
  mixing::api::Bus fx("bus.fx", "aux");
  fx.width = 1.5f;
  fx.polarity_invert_right = true;
  scene.buses.push_back(fx);

  // Two assist sidecars: one JSON-ish text payload, one true binary payload
  // (includes a NUL and high bytes that are not valid UTF-8).
  AssistSidecar text_sidecar;
  text_sidecar.module_id = "midi-sketch";
  text_sidecar.schema_version = 3;
  const std::string text = "{\"blueprint\":\"verse\",\"seed\":42}";
  text_sidecar.payload.assign(text.begin(), text.end());
  text_sidecar.target_track_id = mtid;
  text_sidecar.region_start_ppq = 0.0;
  text_sidecar.region_end_ppq = 960.0;
  p.add_assist_sidecar(text_sidecar);

  AssistSidecar binary_sidecar;
  binary_sidecar.module_id = "unregistered.module.xyz";
  binary_sidecar.schema_version = 99;  // unknown schema version
  binary_sidecar.payload = {0x00, 0x01, 0xFF, 0xFE, 0x80, 0x7F, 0x00, 0xAB, 0xCD};
  p.add_assist_sidecar(binary_sidecar);

  return f;
}

// ---- Deep field comparisons (the model types lack operator==) -------------

bool eq(const transport::TempoSegment& a, const transport::TempoSegment& b) {
  return a.start_ppq == b.start_ppq && a.bpm == b.bpm && a.start_sample == b.start_sample &&
         a.end_bpm == b.end_bpm;
}

bool eq(const automation::AutomationLane& a, const automation::AutomationLane& b) {
  if (a.target_param_id() != b.target_param_id()) return false;
  if (a.points().size() != b.points().size()) return false;
  for (size_t i = 0; i < a.points().size(); ++i) {
    if (a.points()[i].ppq != b.points()[i].ppq) return false;
    if (a.points()[i].value != b.points()[i].value) return false;
    if (a.points()[i].curve_to_next != b.points()[i].curve_to_next) return false;
  }
  return true;
}

bool eq(const Track& a, const Track& b) {
  if (a.id != b.id || a.name != b.name || a.kind != b.kind) return false;
  if (a.channel_strip_ref != b.channel_strip_ref || a.output_target != b.output_target)
    return false;
  if (a.midi_destination_id != b.midi_destination_id) return false;
  if (a.automation_lanes.size() != b.automation_lanes.size()) return false;
  for (size_t i = 0; i < a.automation_lanes.size(); ++i) {
    if (!eq(a.automation_lanes[i], b.automation_lanes[i])) return false;
  }
  return true;
}

bool eq(const EditClip& a, const EditClip& b) {
  return a.id == b.id && a.track_id == b.track_id && a.source_id == b.source_id &&
         a.start_ppq == b.start_ppq && a.length_ppq == b.length_ppq &&
         a.source_offset_ppq == b.source_offset_ppq && a.gain == b.gain &&
         a.fade_in.length_ppq == b.fade_in.length_ppq && a.fade_in.curve == b.fade_in.curve &&
         a.fade_out.length_ppq == b.fade_out.length_ppq && a.fade_out.curve == b.fade_out.curve &&
         a.loop_mode == b.loop_mode && a.loop_length_ppq == b.loop_length_ppq &&
         a.loop_crossfade_ppq == b.loop_crossfade_ppq && a.warp_ref_id == b.warp_ref_id &&
         a.warp_mode == b.warp_mode && a.takes == b.takes && a.active_take_id == b.active_take_id &&
         a.comp_segments == b.comp_segments;
}

bool eq(const ChordSymbol& a, const ChordSymbol& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.root_pc == b.root_pc &&
         a.quality == b.quality && a.extensions == b.extensions &&
         a.slash_bass_pc == b.slash_bass_pc && a.roman_numeral == b.roman_numeral &&
         a.modulation_boundary == b.modulation_boundary;
}

bool eq(const KeySegment& a, const KeySegment& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.tonic_pc == b.tonic_pc &&
         a.mode == b.mode;
}

bool eq(const AssistSidecar& a, const AssistSidecar& b) {
  return a.module_id == b.module_id && a.schema_version == b.schema_version &&
         a.payload == b.payload && a.target_track_id == b.target_track_id &&
         a.region_start_ppq == b.region_start_ppq && a.region_end_ppq == b.region_end_ppq;
}

bool eq(const WarpMapRef& a, const WarpMapRef& b) {
  return a.id == b.id && a.name == b.name && a.anchors == b.anchors;
}

bool eq(const mixing::api::Insert& a, const mixing::api::Insert& b) {
  return a.slot == b.slot && a.processor_name == b.processor_name &&
         a.params_json == b.params_json && a.sidechain_key == b.sidechain_key;
}

bool eq(const mixing::api::Send& a, const mixing::api::Send& b) {
  return a.id == b.id && a.destination_bus_id == b.destination_bus_id && a.send_db == b.send_db &&
         a.timing == b.timing;
}

bool eq(const mixing::api::Strip& a, const mixing::api::Strip& b) {
  if (a.id != b.id || a.input_trim_db != b.input_trim_db || a.fader_db != b.fader_db ||
      a.pan != b.pan || a.width != b.width || a.muted != b.muted || a.soloed != b.soloed ||
      a.solo_safe != b.solo_safe || a.pan_mode != b.pan_mode ||
      a.dual_pan_left != b.dual_pan_left || a.dual_pan_right != b.dual_pan_right ||
      a.polarity_invert_left != b.polarity_invert_left ||
      a.polarity_invert_right != b.polarity_invert_right || a.pan_law != b.pan_law ||
      a.channel_delay_samples != b.channel_delay_samples || a.vca_offset_db != b.vca_offset_db ||
      a.source_layout != b.source_layout || a.surround_pan.azimuth != b.surround_pan.azimuth ||
      a.surround_pan.elevation != b.surround_pan.elevation ||
      a.surround_pan.divergence != b.surround_pan.divergence ||
      a.surround_pan.lfe != b.surround_pan.lfe ||
      a.surround_pan.distance != b.surround_pan.distance || a.inserts.size() != b.inserts.size() ||
      a.sends.size() != b.sends.size()) {
    return false;
  }
  for (size_t i = 0; i < a.inserts.size(); ++i) {
    if (!eq(a.inserts[i], b.inserts[i])) return false;
  }
  for (size_t i = 0; i < a.sends.size(); ++i) {
    if (!eq(a.sends[i], b.sends[i])) return false;
  }
  return true;
}

bool eq(const mixing::api::Bus& a, const mixing::api::Bus& b) {
  if (a.id != b.id || a.role != b.role || a.layout != b.layout ||
      a.input_trim_db != b.input_trim_db || a.width != b.width ||
      a.polarity_invert_left != b.polarity_invert_left ||
      a.polarity_invert_right != b.polarity_invert_right || a.inserts.size() != b.inserts.size()) {
    return false;
  }
  for (size_t i = 0; i < a.inserts.size(); ++i) {
    if (!eq(a.inserts[i], b.inserts[i])) return false;
  }
  return true;
}

bool eq(const mixing::api::VcaGroup& a, const mixing::api::VcaGroup& b) {
  return a.id == b.id && a.gain_db == b.gain_db && a.members == b.members;
}

bool eq(const mixing::api::Connection& a, const mixing::api::Connection& b) {
  return a.source == b.source && a.destination == b.destination;
}

bool eq(const mixing::api::Scene& a, const mixing::api::Scene& b) {
  if (a.version != b.version || a.strips.size() != b.strips.size() ||
      a.buses.size() != b.buses.size() || a.vca_groups.size() != b.vca_groups.size() ||
      a.connections.size() != b.connections.size()) {
    return false;
  }
  for (size_t i = 0; i < a.strips.size(); ++i) {
    if (!eq(a.strips[i], b.strips[i])) return false;
  }
  for (size_t i = 0; i < a.buses.size(); ++i) {
    if (!eq(a.buses[i], b.buses[i])) return false;
  }
  for (size_t i = 0; i < a.vca_groups.size(); ++i) {
    if (!eq(a.vca_groups[i], b.vca_groups[i])) return false;
  }
  for (size_t i = 0; i < a.connections.size(); ++i) {
    if (!eq(a.connections[i], b.connections[i])) return false;
  }
  return true;
}

void check_project_equal(const Project& a, const Project& b) {
  CHECK(a.sample_rate() == b.sample_rate());
  CHECK(a.overlap_policy() == b.overlap_policy());
  CHECK(eq(a.scene(), b.scene()));

  REQUIRE(a.tempo_segments().size() == b.tempo_segments().size());
  for (size_t i = 0; i < a.tempo_segments().size(); ++i)
    CHECK(eq(a.tempo_segments()[i], b.tempo_segments()[i]));

  REQUIRE(a.time_signatures().size() == b.time_signatures().size());
  for (size_t i = 0; i < a.time_signatures().size(); ++i) {
    CHECK(a.time_signatures()[i].start_ppq == b.time_signatures()[i].start_ppq);
    CHECK(a.time_signatures()[i].time_sig.numerator == b.time_signatures()[i].time_sig.numerator);
    CHECK(a.time_signatures()[i].time_sig.denominator ==
          b.time_signatures()[i].time_sig.denominator);
  }

  REQUIRE(a.sources().size() == b.sources().size());
  for (size_t i = 0; i < a.sources().size(); ++i) {
    CHECK(source_kind(a.sources()[i]) == source_kind(b.sources()[i]));
    CHECK(source_id(a.sources()[i]) == source_id(b.sources()[i]));
  }

  REQUIRE(a.tracks().size() == b.tracks().size());
  for (size_t i = 0; i < a.tracks().size(); ++i) CHECK(eq(a.tracks()[i], b.tracks()[i]));

  REQUIRE(a.clips().size() == b.clips().size());
  for (size_t i = 0; i < a.clips().size(); ++i) CHECK(eq(a.clips()[i], b.clips()[i]));

  REQUIRE(a.warp_maps().size() == b.warp_maps().size());
  for (size_t i = 0; i < a.warp_maps().size(); ++i) CHECK(eq(a.warp_maps()[i], b.warp_maps()[i]));

  REQUIRE(a.markers().size() == b.markers().size());
  for (size_t i = 0; i < a.markers().size(); ++i) {
    CHECK(a.markers()[i].id == b.markers()[i].id);
    CHECK(a.markers()[i].ppq == b.markers()[i].ppq);
    CHECK(a.markers()[i].name == b.markers()[i].name);
    CHECK(a.markers()[i].kind == b.markers()[i].kind);
    CHECK(a.markers()[i].key_fifths == b.markers()[i].key_fifths);
    CHECK(a.markers()[i].key_minor == b.markers()[i].key_minor);
  }

  CHECK(a.annotation().tempo_confidence == b.annotation().tempo_confidence);
  REQUIRE(a.annotation().keys.size() == b.annotation().keys.size());
  for (size_t i = 0; i < a.annotation().keys.size(); ++i)
    CHECK(eq(a.annotation().keys[i], b.annotation().keys[i]));
  REQUIRE(a.annotation().chords.size() == b.annotation().chords.size());
  for (size_t i = 0; i < a.annotation().chords.size(); ++i)
    CHECK(eq(a.annotation().chords[i], b.annotation().chords[i]));
  REQUIRE(a.annotation().sections.size() == b.annotation().sections.size());
  REQUIRE(a.annotation().onsets.size() == b.annotation().onsets.size());

  REQUIRE(a.assist_sidecars().size() == b.assist_sidecars().size());
  for (size_t i = 0; i < a.assist_sidecars().size(); ++i)
    CHECK(eq(a.assist_sidecars()[i], b.assist_sidecars()[i]));
}

}  // namespace

TEST_CASE("project serialize is deterministic and stable across calls", "[serialize]") {
  Fixture f = make_fixture();
  const std::string a = project_to_json(f.project, f.midi);
  const std::string b = project_to_json(f.project, f.midi);
  CHECK(a == b);  // Two independent serializations are byte-identical.

  // Mandatory top-level integer version field is present.
  const auto root = util::json::parse(a);
  REQUIRE(root.is_object());
  REQUIRE(root.contains("version"));
  CHECK(root["version"].as_int() ==
        static_cast<int>(sonare::serialize::SONARE_PROJECT_SCHEMA_VERSION));
}

TEST_CASE("project serialize/deserialize/serialize is byte-identical", "[serialize]") {
  Fixture f = make_fixture();
  const std::string s1 = project_to_json(f.project, f.midi);

  auto result = project_from_json(s1);
  REQUIRE(result.ok());
  CHECK_FALSE(result.has_error());

  const std::string s2 = project_to_json(*result.project, result.midi);
  CHECK(s1 == s2);  // Round-trip is byte-for-byte stable.
}

TEST_CASE("project round-trip preserves all fields", "[serialize]") {
  Fixture f = make_fixture();
  const std::string s1 = project_to_json(f.project, f.midi);
  auto result = project_from_json(s1);
  REQUIRE(result.ok());

  check_project_equal(f.project, *result.project);
  CHECK(result.midi == f.midi);  // MidiContentStore deep-equal.
}

TEST_CASE("project deserialize rejects an unsupported embedded scene version", "[serialize]") {
  // The embedded mixer scene carries its own version. A value the build does not
  // understand must be rejected (surfaced as a diagnostic, project reset) rather
  // than silently mis-read, mirroring the standalone scene_from_json guard.
  Fixture f = make_fixture();
  f.project.scene().version = 2;  // a future, unsupported scene schema version
  const std::string s = project_to_json(f.project, f.midi);

  auto result = project_from_json(s);
  CHECK_FALSE(result.ok());
  CHECK(result.has_error());
  // The guard surfaces the distinct invalid_format diagnostic, not the generic
  // deserialize_failed, staying symmetric with top-level schema-version handling.
  bool saw_invalid_format = false;
  bool saw_generic_failure = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "invalid_format") saw_invalid_format = true;
    if (d.code == "deserialize_failed") saw_generic_failure = true;
  }
  CHECK(saw_invalid_format);
  CHECK_FALSE(saw_generic_failure);

  // A version-1 scene still round-trips cleanly.
  f.project.scene().version = 1;
  auto ok_result = project_from_json(project_to_json(f.project, f.midi));
  REQUIRE(ok_result.ok());
}

TEST_CASE("project scene JSON uses stable camelCase keys", "[serialize]") {
  Fixture f = make_fixture();
  const auto root = util::json::parse(project_to_json(f.project, f.midi));
  REQUIRE(root.is_object());
  REQUIRE(root["scene"].is_object());
  const auto& scene = root["scene"];

  CHECK(scene.contains("vcaGroups"));
  CHECK_FALSE(scene.contains("vca_groups"));
  REQUIRE(scene["strips"].is_array());
  REQUIRE_FALSE(scene["strips"].as_array().empty());
  const auto& strip = scene["strips"].as_array()[0];
  REQUIRE(strip.is_object());
  CHECK(strip.contains("inputTrimDb"));
  CHECK(strip.contains("faderDb"));
  CHECK(strip.contains("soloSafe"));
  CHECK_FALSE(strip.contains("input_trim_db"));
  CHECK_FALSE(strip.contains("fader_db"));
  CHECK_FALSE(strip.contains("solo_safe"));

  REQUIRE(strip["inserts"].is_array());
  REQUIRE_FALSE(strip["inserts"].as_array().empty());
  const auto& insert = strip["inserts"].as_array()[0];
  REQUIRE(insert.is_object());
  CHECK(insert.contains("processor"));
  CHECK(insert.contains("params"));
  CHECK_FALSE(insert.contains("sidechainKey"));
  CHECK_FALSE(insert.contains("processor_name"));
  CHECK_FALSE(insert.contains("params_json"));
  CHECK_FALSE(insert.contains("sidechain_key"));

  REQUIRE(strip["sends"].is_array());
  REQUIRE_FALSE(strip["sends"].as_array().empty());
  const auto& send = strip["sends"].as_array()[0];
  CHECK(send.contains("destinationBusId"));
  CHECK(send.contains("sendDb"));
  CHECK_FALSE(send.contains("destination_bus_id"));
  CHECK_FALSE(send.contains("send_db"));

  REQUIRE(scene["buses"].is_array());
  REQUIRE_FALSE(scene["buses"].as_array().empty());
  const auto& bus = scene["buses"].as_array()[0];
  REQUIRE(bus.is_object());
  REQUIRE(bus["inserts"].is_array());
  REQUIRE_FALSE(bus["inserts"].as_array().empty());
  const auto& bus_insert = bus["inserts"].as_array()[0];
  REQUIRE(bus_insert.is_object());
  CHECK(bus_insert.contains("sidechainKey"));
  CHECK_FALSE(bus_insert.contains("sidechain_key"));
}

TEST_CASE("project scene deserializer accepts legacy snake_case scene keys", "[serialize]") {
  Project project;
  MidiContentStore midi;
  auto root = util::json::parse(project_to_json(project, midi));
  root.as_object()["scene"] = util::json::parse(
      "{\"version\":1,\"strips\":[{\"id\":\"legacy\",\"input_trim_db\":1.5,"
      "\"fader_db\":-4,\"pan\":0.25,\"width\":1.1,\"muted\":false,\"soloed\":true,"
      "\"solo_safe\":true,\"pan_mode\":2,\"dual_pan_left\":-0.4,"
      "\"dual_pan_right\":0.6,\"polarity_invert_left\":true,"
      "\"polarity_invert_right\":false,\"pan_law\":3,\"channel_delay_samples\":21,"
      "\"inserts\":[{\"slot\":\"post\",\"processor_name\":\"legacy.eq\","
      "\"params_json\":\"{\\\"q\\\":1.25}\",\"sidechain_key\":\"legacy.sc\"}],"
      "\"sends\":[{\"id\":\"send\",\"destination_bus_id\":\"bus\","
      "\"send_db\":-8,\"timing\":\"pre\"}]}],\"buses\":[],"
      "\"vca_groups\":[{\"id\":\"vca\",\"gain_db\":-2,\"members\":[\"legacy\"]}],"
      "\"connections\":[{\"source\":\"legacy\",\"destination\":\"bus\"}]}");

  auto decoded = project_from_json(util::json::dump(root));
  REQUIRE(decoded.ok());
  const auto& scene = decoded.project->scene();
  REQUIRE(scene.strips.size() == 1);
  CHECK(scene.strips[0].id == "legacy");
  CHECK(scene.strips[0].input_trim_db == 1.5f);
  CHECK(scene.strips[0].fader_db == -4.0f);
  CHECK(scene.strips[0].solo_safe);
  CHECK(scene.strips[0].pan_mode == 2);
  CHECK(scene.strips[0].dual_pan_left == -0.4f);
  CHECK(scene.strips[0].dual_pan_right == 0.6f);
  CHECK(scene.strips[0].polarity_invert_left);
  CHECK(scene.strips[0].pan_law == 3);
  CHECK(scene.strips[0].channel_delay_samples == 21);
  REQUIRE(scene.strips[0].inserts.size() == 1);
  CHECK(scene.strips[0].inserts[0].slot == mixing::api::InsertSlot::PostFader);
  CHECK(scene.strips[0].inserts[0].processor_name == "legacy.eq");
  CHECK(scene.strips[0].inserts[0].params_json == "{\"q\":1.25}");
  CHECK(scene.strips[0].inserts[0].sidechain_key == "legacy.sc");
  REQUIRE(scene.strips[0].sends.size() == 1);
  CHECK(scene.strips[0].sends[0].destination_bus_id == "bus");
  CHECK(scene.strips[0].sends[0].send_db == -8.0f);
  CHECK(scene.strips[0].sends[0].timing == mixing::api::SendTiming::PreFader);
  REQUIRE(scene.vca_groups.size() == 1);
  CHECK(scene.vca_groups[0].id == "vca");
  CHECK(scene.vca_groups[0].gain_db == -2.0f);
  CHECK(scene.vca_groups[0].members == std::vector<std::string>{"legacy"});
  REQUIRE(scene.connections.size() == 1);
  CHECK(scene.connections[0].destination == "bus");

  const auto normalized = util::json::parse(project_to_json(*decoded.project, decoded.midi));
  CHECK(normalized["scene"].contains("vcaGroups"));
  CHECK_FALSE(normalized["scene"].contains("vca_groups"));
  const auto& normalized_strip = normalized["scene"]["strips"].as_array()[0];
  const auto& normalized_insert = normalized_strip["inserts"].as_array()[0];
  CHECK(normalized_insert.contains("processor"));
  CHECK(normalized_insert.contains("params"));
  CHECK(normalized_insert.contains("sidechainKey"));
  CHECK_FALSE(normalized_insert.contains("processor_name"));
  CHECK_FALSE(normalized_insert.contains("params_json"));
  CHECK_FALSE(normalized_insert.contains("sidechain_key"));
}

TEST_CASE("project deserialize preserves next id counters without gaps", "[serialize]") {
  Fixture f = make_fixture();
  const std::string s = project_to_json(f.project, f.midi);
  auto decoded = project_from_json(s);
  REQUIRE(decoded.ok());
  Project& p = *decoded.project;

  SourceId max_source = 0;
  for (const auto& src : p.sources()) max_source = std::max(max_source, source_id(src));
  TrackId max_track = 0;
  for (const auto& track : p.tracks()) max_track = std::max(max_track, track.id);
  ClipId max_clip = 0;
  for (const auto& clip : p.clips()) max_clip = std::max(max_clip, clip.id);
  uint32_t max_marker = 0;
  for (const auto& marker : p.markers()) max_marker = std::max(max_marker, marker.id);

  AudioSourceRef audio;
  const SourceId new_source = p.add_audio_source(audio);
  CHECK(new_source == max_source + 1);

  Track track;
  track.kind = Track::Kind::kAudio;
  const TrackId new_track = p.add_track(track);
  CHECK(new_track == max_track + 1);

  p.set_overlap_policy(OverlapPolicy::kAllow);
  EditClip clip;
  clip.track_id = new_track;
  clip.source_id = new_source;
  clip.start_ppq = 0.0;
  clip.length_ppq = 1.0;
  const ClipId new_clip = p.add_clip(clip);
  CHECK(new_clip == max_clip + 1);

  const uint32_t new_marker = p.add_marker(0.0, "next");
  CHECK(new_marker == max_marker + 1);
}

TEST_CASE("unknown fields are ignored and dropped on re-serialize", "[serialize]") {
  Fixture f = make_fixture();
  const std::string s1 = project_to_json(f.project, f.midi);

  // Inject extra unknown keys at the top level and inside a clip object.
  auto root = util::json::parse(s1);
  root.as_object()["__unknown_top__"] = std::string("ignore me");
  root.as_object()["future_feature"] = 1234.5;
  REQUIRE(root["clips"].is_array());
  REQUIRE(!root["clips"].as_array().empty());
  root.as_object()["clips"].as_array()[0].as_object()["__unknown_clip__"] = true;
  const std::string injected = util::json::dump(root);

  auto result = project_from_json(injected);
  REQUIRE(result.ok());
  CHECK_FALSE(result.has_error());

  // Re-serialization drops the unknown fields => identical to the clean form.
  const std::string s2 = project_to_json(*result.project, result.midi);
  CHECK(s2 == s1);
}

TEST_CASE("AssistSidecar with unregistered module + binary payload round-trips losslessly",
          "[serialize]") {
  Project p;
  MidiContentStore midi;
  AssistSidecar sidecar;
  sidecar.module_id = "totally.unregistered.module";
  sidecar.schema_version = 0xDEADBEEF;  // unknown / future schema version
  // Every byte value 0..255 so we exercise NUL, high bytes, and non-UTF-8.
  for (int i = 0; i < 256; ++i) sidecar.payload.push_back(static_cast<uint8_t>(i));
  sidecar.target_track_id = 7;
  sidecar.region_start_ppq = 100.0;
  sidecar.region_end_ppq = 200.0;
  p.add_assist_sidecar(sidecar);

  const std::string s1 = project_to_json(p, midi);
  auto result = project_from_json(s1);
  REQUIRE(result.ok());
  REQUIRE(result.project->assist_sidecars().size() == 1);

  const AssistSidecar& restored = result.project->assist_sidecars()[0];
  CHECK(restored.module_id == sidecar.module_id);
  CHECK(restored.schema_version == sidecar.schema_version);
  CHECK(restored.payload == sidecar.payload);  // byte-for-byte payload survival
  CHECK(restored.target_track_id == sidecar.target_track_id);
  CHECK(restored.region_start_ppq == sidecar.region_start_ppq);
  CHECK(restored.region_end_ppq == sidecar.region_end_ppq);

  const std::string s2 = project_to_json(*result.project, result.midi);
  CHECK(s1 == s2);
}

TEST_CASE("AudioSourceRef content_hash round-trips when set", "[serialize]") {
  Project p;
  MidiContentStore midi;
  AudioSourceRef audio;
  audio.uri = "file://x.wav";
  audio.content_hash = "sha256:deadbeef";
  p.add_audio_source(audio);

  const std::string s1 = project_to_json(p, midi);
  // The key is present only because the hash is set.
  CHECK(s1.find("content_hash") != std::string::npos);

  auto result = project_from_json(s1);
  REQUIRE(result.ok());
  REQUIRE(result.project->sources().size() == 1);
  const auto* restored = std::get_if<AudioSourceRef>(&result.project->sources()[0]);
  REQUIRE(restored != nullptr);
  CHECK(restored->content_hash == "sha256:deadbeef");

  const std::string s2 = project_to_json(*result.project, result.midi);
  CHECK(s1 == s2);
}

TEST_CASE("AudioSourceRef without content_hash omits the key (byte-stable)", "[serialize]") {
  Project p;
  MidiContentStore midi;
  AudioSourceRef audio;
  audio.uri = "file://x.wav";  // content_hash left empty (default)
  p.add_audio_source(audio);

  const std::string s = project_to_json(p, midi);
  // Empty hash must NOT appear so existing projects stay byte-identical.
  CHECK(s.find("content_hash") == std::string::npos);

  // Absent hash loads back as empty.
  auto result = project_from_json(s);
  REQUIRE(result.ok());
  REQUIRE(result.project->sources().size() == 1);
  const auto* restored = std::get_if<AudioSourceRef>(&result.project->sources()[0]);
  REQUIRE(restored != nullptr);
  CHECK(restored->content_hash.empty());
}

TEST_CASE("tempo segments are normalized (sorted + deduped) on load", "[serialize]") {
  // Hand-build an out-of-order document with a duplicate start_ppq (last wins).
  util::json::Object root;
  root["version"] = static_cast<double>(1);
  util::json::Array tempo;
  auto seg = [](double start, double bpm) {
    util::json::Object o;
    o["start_ppq"] = start;
    o["bpm"] = bpm;
    o["start_sample"] = 0.0;
    o["end_bpm"] = 0.0;
    return util::json::Value(std::move(o));
  };
  tempo.push_back(seg(1920.0, 140.0));
  tempo.push_back(seg(0.0, 120.0));
  tempo.push_back(seg(960.0, 130.0));
  tempo.push_back(seg(960.0, 131.0));  // duplicate tick: this one wins
  root["tempo_segments"] = std::move(tempo);

  auto result = project_from_json(util::json::dump(util::json::Value(std::move(root))));
  REQUIRE(result.ok());
  const auto& segs = result.project->tempo_segments();
  REQUIRE(segs.size() == 3);
  CHECK(segs[0].start_ppq == 0.0);
  CHECK(segs[1].start_ppq == 960.0);
  CHECK(segs[1].bpm == 131.0);  // last writer at the duplicate tick
  CHECK(segs[2].start_ppq == 1920.0);
}

TEST_CASE("non-positive tempo bpm is rejected with a diagnostic", "[serialize]") {
  auto result = project_from_json("{\"version\": 1, \"tempo_segments\": [{\"bpm\": -5.0}]}");
  CHECK_FALSE(result.ok());
  REQUIRE(result.has_error());
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "invalid_tempo_bpm") found = true;
  }
  CHECK(found);
}

TEST_CASE("invalid project sample rate is rejected with a diagnostic", "[serialize]") {
  for (const char* json :
       {"{\"version\": 1, \"sample_rate\": 0}", "{\"version\": 1, \"sample_rate\": -48000}",
        "{\"version\": 1, \"sample_rate\": 500000}"}) {
    auto result = project_from_json(json);
    CHECK_FALSE(result.ok());
    REQUIRE(result.has_error());
    bool found = false;
    for (const auto& d : result.diagnostics) {
      if (d.code == "invalid_sample_rate") found = true;
    }
    CHECK(found);
  }
}

TEST_CASE("malformed warp map is dropped with a diagnostic instead of silently", "[serialize]") {
  // The public edit API requires two or more strictly-increasing anchors.
  // Loader input follows that same contract and must retain a diagnostic.
  for (const char* json : {"{\"version\": 1, \"warp_maps\": [{\"id\": 7, \"anchors\": ["
                           "{\"warp_sample\": 0.0, \"source_sample\": 0.0}]}]}",
                           "{\"version\": 1, \"warp_maps\": [{\"id\": 7, \"anchors\": ["
                           "{\"warp_sample\": 100.0, \"source_sample\": 0.0}, "
                           "{\"warp_sample\": 50.0, \"source_sample\": 100.0}]}]}"}) {
    auto result = project_from_json(json);
    bool found = false;
    for (const auto& d : result.diagnostics) {
      if (d.code == "invalid_warp_map") found = true;
    }
    CHECK(found);
  }
}

TEST_CASE("clip referencing a missing warp map reports a dangling diagnostic", "[serialize]") {
  auto result = project_from_json(
      "{\"version\": 1, \"clips\": [{\"id\": 1, \"track_id\": 1, \"source_id\": 1, "
      "\"warp_ref_id\": 99}]}");
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "dangling_clip_warp") found = true;
  }
  CHECK(found);
}

TEST_CASE("invalid time signature is rejected with a diagnostic", "[serialize]") {
  auto result = project_from_json(
      "{\"version\": 1, \"time_signatures\": [{\"start_ppq\": 0.0, \"numerator\": 4, "
      "\"denominator\": 0}]}");
  CHECK_FALSE(result.ok());
  REQUIRE(result.has_error());
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "invalid_time_signature") found = true;
  }
  CHECK(found);
}

TEST_CASE("duplicate marker id is rejected with a diagnostic", "[serialize]") {
  // The loader deduplicates marker ids through a hash set (O(n) overall); a
  // repeated id must still be rejected rather than silently merged.
  auto result = project_from_json(
      "{\"version\": 1, \"markers\": [{\"id\": 5, \"ppq\": 0.0}, "
      "{\"id\": 5, \"ppq\": 100.0}]}");
  CHECK_FALSE(result.ok());
  REQUIRE(result.has_error());
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "duplicate_entity_id") found = true;
  }
  CHECK(found);
}

TEST_CASE("present invalid project integer and enum fields report invalid_format", "[serialize]") {
  const std::vector<std::string> documents = {
      // Finite-but-unrepresentable values must never reach a floating-to-int cast.
      R"({"version":1,"time_signatures":[{"numerator":1e100,"denominator":4}]})",
      R"({"version":1,"time_signatures":[{"numerator":2147483648,"denominator":4}]})",
      R"({"version":1,"time_signatures":[{"numerator":-2147483649,"denominator":4}]})",
      // Integer fields and enum ordinals are exact, not truncating conversions.
      R"({"version":1,"time_signatures":[{"numerator":4.5,"denominator":4}]})",
      R"({"version":1,"tracks":[{"id":1,"kind":0.5}]})",
      R"({"version":1,"tracks":[{"id":1,"kind":3}]})",
      R"({"version":1,"scene":{"version":1,"strips":[{"id":"s","panMode":1.5}]}})",
      R"({"version":1,"annotation":{"chords":[{"extensions":[7.5]}]}})",
      R"({"version":1,"midi_content":{"1":[{"data0":1.5}]}})",
      R"({"version":1,"overlap_policy":2})",
  };

  for (const auto& document : documents) {
    INFO(document);
    const auto result = project_from_json(document);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().code == "invalid_format");
  }

  // The representable edge itself remains valid; only INT_MAX + 1 is rejected.
  const auto edge = project_from_json(
      R"({"version":1,"time_signatures":[{"numerator":2147483647,"denominator":4}]})");
  REQUIRE(edge.ok());
  REQUIRE(edge.project->time_signatures().size() == 1);
  CHECK(edge.project->time_signatures()[0].time_sig.numerator == std::numeric_limits<int>::max());
}

TEST_CASE("project deserialize rejects float narrowing overflow with a field path", "[serialize]") {
  struct InvalidFloatField {
    const char* document;
    const char* field_path;
  };
  const std::vector<InvalidFloatField> documents = {
      {R"({"version":1,"tracks":[{"id":1,"gain":1e39}]})", "tracks[].gain"},
      {R"({"version":1,"tracks":[{"id":1,"pan":-1e39}]})", "tracks[].pan"},
      {R"({"version":1,"tracks":[{"id":1,"automation_lanes":[{"points":[{"value":1e39}]}]}]})",
       "tracks[].automation_lanes[].points[].value"},
      {R"({"version":1,"clips":[{"id":1,"gain":-1e39}]})", "clips[].gain"},
      {R"({"version":1,"annotation":{"tempo_confidence":1e39}})", "annotation.tempo_confidence"},
      {R"({"version":1,"annotation":{"onsets":[{"confidence":-1e39}]}})",
       "annotation.onsets[].confidence"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"inputTrimDb":1e39}]}})",
       "scene.strips[].inputTrimDb"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"faderDb":-1e39}]}})",
       "scene.strips[].faderDb"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"vcaOffsetDb":1e39}]}})",
       "scene.strips[].vcaOffsetDb"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"pan":-1e39}]}})", "scene.strips[].pan"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"width":1e39}]}})", "scene.strips[].width"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"dualPanLeft":-1e39}]}})",
       "scene.strips[].dualPanLeft"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"dualPanRight":1e39}]}})",
       "scene.strips[].dualPanRight"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"surroundPan":{"azimuth":1e39}}]}})",
       "scene.strips[].surroundPan.azimuth"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"surroundPan":{"elevation":-1e39}}]}})",
       "scene.strips[].surroundPan.elevation"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"surroundPan":{"divergence":1e39}}]}})",
       "scene.strips[].surroundPan.divergence"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"surroundPan":{"lfe":-1e39}}]}})",
       "scene.strips[].surroundPan.lfe"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"surroundPan":{"distance":1e39}}]}})",
       "scene.strips[].surroundPan.distance"},
      {R"({"version":1,"scene":{"version":1,"strips":[{"sends":[{"sendDb":-1e39}]}]}})",
       "scene.strips[].sends[].sendDb"},
      {R"({"version":1,"scene":{"version":1,"buses":[{"inputTrimDb":1e39}]}})",
       "scene.buses[].inputTrimDb"},
      {R"({"version":1,"scene":{"version":1,"buses":[{"width":-1e39}]}})", "scene.buses[].width"},
      {R"({"version":1,"scene":{"version":1,"vcaGroups":[{"gainDb":1e39}]}})",
       "scene.vcaGroups[].gainDb"},
  };

  for (const auto& test : documents) {
    INFO(test.document);
    const auto result = project_from_json(test.document);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().code == "invalid_format");
    CHECK(result.diagnostics.back().message.find(test.field_path) != std::string::npos);
  }
}

TEST_CASE("project deserialize accepts finite float boundaries and ordinary values",
          "[serialize]") {
  constexpr const char* kFloatMax = "3.4028234663852886e38";
  const std::string document =
      std::string(
          R"({"version":1,"sources":[{"id":1,"kind":0}],"tracks":[{"id":1,"gain":0.75,"pan":-0.25,"automation_lanes":[{"points":[{"value":)") +
      kFloatMax + R"(}]}]}],"clips":[{"id":1,"track_id":1,"source_id":1,"gain":-)" + kFloatMax +
      R"(}],"annotation":{"tempo_confidence":0.8,"onsets":[{"confidence":0.6}]},"scene":{"version":1,"strips":[{"id":"s","inputTrimDb":-3.5,"faderDb":2.25,"vcaOffsetDb":0.5,"pan":0.1,"width":1.2,"dualPanLeft":-0.8,"dualPanRight":0.7,"surroundPan":{"azimuth":45,"elevation":-12,"divergence":0.4,"lfe":0.2,"distance":2},"sends":[{"sendDb":-6}]}],"buses":[{"id":"b","inputTrimDb":1.5,"width":0.9}],"vcaGroups":[{"id":"v","gainDb":-2}]}})";

  const auto result = project_from_json(document);
  REQUIRE(result.ok());
  REQUIRE(result.project->tracks().size() == 1);
  REQUIRE(result.project->clips().size() == 1);
  REQUIRE(result.project->tracks()[0].automation_lanes.size() == 1);
  REQUIRE(result.project->tracks()[0].automation_lanes[0].points().size() == 1);
  CHECK(result.project->tracks()[0].gain == 0.75f);
  CHECK(result.project->tracks()[0].pan == -0.25f);
  CHECK(result.project->tracks()[0].automation_lanes[0].points()[0].value ==
        std::numeric_limits<float>::max());
  CHECK(result.project->clips()[0].gain == 0.0f);
  REQUIRE_FALSE(result.diagnostics.empty());
  CHECK(result.diagnostics[0].code == "clip_gain_clamped");
  CHECK(result.project->annotation().tempo_confidence == 0.8f);
  CHECK(result.project->annotation().onsets[0].confidence == 0.6f);
  REQUIRE(result.project->scene().strips.size() == 1);
  REQUIRE(result.project->scene().buses.size() == 1);
  REQUIRE(result.project->scene().vca_groups.size() == 1);
  CHECK(result.project->scene().strips[0].fader_db == 2.25f);
  CHECK(result.project->scene().strips[0].surround_pan.distance == 2.0f);
  CHECK(result.project->scene().strips[0].sends[0].send_db == -6.0f);
  CHECK(result.project->scene().buses[0].input_trim_db == 1.5f);
  CHECK(result.project->scene().vca_groups[0].gain_db == -2.0f);

  const auto serialized = util::json::parse(project_to_json(*result.project, result.midi));
  CHECK(serialized["clips"].as_array()[0]["gain"].is_number());
  CHECK(serialized["tracks"]
            .as_array()[0]["automation_lanes"]
            .as_array()[0]["points"]
            .as_array()[0]["value"]
            .is_number());
}

TEST_CASE("project deserialize warns for raw clip PPQ outside the edit contract", "[serialize]") {
  const auto result = project_from_json(
      R"({"version":1,"sources":[{"id":1,"kind":0}],"tracks":[{"id":1}],"clips":[{"id":1,"track_id":1,"source_id":1,"start_ppq":-1,"length_ppq":0,"source_offset_ppq":-2}]})");
  REQUIRE(result.ok());
  REQUIRE(result.project->clips().size() == 1);
  REQUIRE_FALSE(result.diagnostics.empty());
  CHECK(result.diagnostics[0].code == "invalid_clip_ppq");
}

TEST_CASE("project deserialize bounds dangling-reference diagnostics", "[serialize]") {
  std::string document = R"({"version":1,"clips":[)";
  for (uint32_t id = 1; id <= 130; ++id) {
    if (id > 1) document += ',';
    document +=
        "{\"id\":" + std::to_string(id) + ",\"track_id\":999,\"source_id\":999,\"length_ppq\":1}";
  }
  document += "]}";
  const auto result = project_from_json(document);
  REQUIRE(result.ok());
  // 128 retained details plus one count-preserving summary, rather than two
  // growing strings per malformed clip.
  REQUIRE(result.diagnostics.size() == 129);
  CHECK(result.diagnostics.back().code == "referential_diagnostics_truncated");
  CHECK(result.diagnostics.back().message.find("132") != std::string::npos);
}

TEST_CASE("wrong-typed scene enum fields fall back instead of aborting the load", "[serialize]") {
  // int_or_any now matches num_or_any/str_or_any/bool_or_any: a present-but-
  // wrong-typed (non-numeric) panMode/panLaw/channelDelaySamples falls back to the
  // default rather than rejecting an otherwise-valid project. A numeric-but-
  // fractional value is still a genuine error (covered above), so the guard only
  // relaxes the type-mismatch case.
  const std::vector<std::string> documents = {
      R"({"version":1,"scene":{"version":1,"strips":[{"id":"s","panMode":"stereo"}]}})",
      R"({"version":1,"scene":{"version":1,"strips":[{"id":"s","panLaw":true}]}})",
      R"({"version":1,"scene":{"version":1,"strips":[{"id":"s","channelDelaySamples":"none"}]}})",
  };
  for (const auto& document : documents) {
    INFO(document);
    const auto result = project_from_json(document);
    REQUIRE(result.ok());
    REQUIRE(result.project->scene().strips.size() == 1);
    // The wrong-typed field took its default (0), and the strip id still loaded.
    CHECK(result.project->scene().strips[0].id == "s");
    CHECK(result.project->scene().strips[0].pan_mode == 0);
    CHECK(result.project->scene().strips[0].pan_law == 0);
    CHECK(result.project->scene().strips[0].channel_delay_samples == 0);
  }
}

TEST_CASE("out-of-range MIDI data word is clamped with a warning, not silently zeroed",
          "[serialize]") {
  // data0 below zero clamps to 0; data1 above uint32 max clamps to 0xFFFFFFFF.
  const std::string in =
      "{\"version\": 1, \"midi_content\": {\"3\": [{\"ppq\": 0.0, \"data0\": -1.0, "
      "\"data1\": 5000000000.0}]}}";
  auto result = project_from_json(in);
  REQUIRE(result.ok());
  REQUIRE(result.midi.events.count(3) == 1);
  const auto& events = result.midi.events.at(3);
  REQUIRE(events.size() == 1);
  CHECK(events[0].data0 == 0u);
  CHECK(events[0].data1 == 0xFFFFFFFFu);
  int warnings = 0;
  for (const auto& d : result.diagnostics) {
    if (d.code == "midi_word_out_of_range") ++warnings;
  }
  CHECK(warnings == 2);
}

TEST_CASE("out-of-range MIDI content keys are ignored instead of truncating to uint32",
          "[serialize]") {
  const std::string in =
      "{\"version\": 1, \"midi_content\": {"
      "\"4294967296\": [{\"ppq\": 0.0, \"data0\": 1.0, \"data1\": 2.0}],"
      "\"__sysex_payloads\": {\"4294967296\": \"AQI=\"}}}";
  auto result = project_from_json(in);
  REQUIRE(result.ok());
  CHECK(result.midi.events.empty());
  CHECK(result.midi.sysex_payloads.empty());

  bool found_clip_key = false;
  bool found_sysex_key = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "invalid_midi_content_key") found_clip_key = true;
    if (d.code == "invalid_sysex_handle") found_sysex_key = true;
  }
  CHECK(found_clip_key);
  CHECK(found_sysex_key);
}

TEST_CASE("out-of-range entity ids are rejected instead of wrapping", "[serialize]") {
  const std::string in =
      "{\"version\": 1, "
      "\"sources\": [{\"kind\": 0, \"id\": 5000000000.0}], "
      "\"tracks\": [{\"id\": 5000000000.0, \"kind\": 0}], "
      "\"clips\": [{\"id\": 5000000000.0, \"track_id\": 5000000000.0, "
      "\"source_id\": 5000000000.0, \"length_ppq\": 1.0}]}";
  auto result = project_from_json(in);
  CHECK_FALSE(result.ok());
  REQUIRE_FALSE(result.diagnostics.empty());
  CHECK(result.diagnostics.back().code == "invalid_format");
}

TEST_CASE("maximum usable entity ids saturate allocators without wrapping", "[serialize]") {
  constexpr uint32_t kLastId = std::numeric_limits<uint32_t>::max() - 1;
  const std::string id = std::to_string(kLastId);
  const std::string in =
      "{\"version\":1,"
      "\"sources\":[{\"kind\":0,\"id\":" +
      id + "}],\"tracks\":[{\"id\":" + id + ",\"kind\":0}],\"clips\":[{\"id\":" + id +
      ",\"track_id\":" + id + ",\"source_id\":" + id +
      ",\"length_ppq\":1.0}],\"markers\":[{\"id\":" + id + ",\"ppq\":0.0,\"name\":\"last\"}]}";

  auto result = project_from_json(in);
  REQUIRE(result.ok());
  Project& project = *result.project;
  CHECK(project.next_source_id() == std::numeric_limits<uint32_t>::max());
  CHECK(project.next_track_id() == std::numeric_limits<uint32_t>::max());
  CHECK(project.next_clip_id() == std::numeric_limits<uint32_t>::max());
  CHECK(project.next_marker_id() == std::numeric_limits<uint32_t>::max());

  CHECK(project.add_audio_source(AudioSourceRef{}) == 0);
  CHECK(project.add_track(Track{}) == 0);
  EditClip clip;
  clip.track_id = kLastId;
  clip.source_id = kLastId;
  clip.length_ppq = 1.0;
  CHECK(project.add_clip(clip) == 0);
  CHECK(project.add_marker(0.0, "exhausted") == 0);

  const auto roundtrip = project_from_json(project_to_json(project, result.midi));
  REQUIRE(roundtrip.ok());
  CHECK(roundtrip.project->sources().front().index() == project.sources().front().index());
  CHECK(source_id(roundtrip.project->sources().front()) == kLastId);
  CHECK(roundtrip.project->tracks().front().id == kLastId);
  CHECK(roundtrip.project->clips().front().id == kLastId);
  CHECK(roundtrip.project->markers().front().id == kLastId);
}

TEST_CASE("reserved and duplicate entity ids are rejected", "[serialize]") {
  const std::string max = std::to_string(std::numeric_limits<uint32_t>::max());
  const std::vector<std::pair<std::string, std::string>> fixtures = {
      {"{\"version\":1,\"sources\":[{\"kind\":0,\"id\":0}]}", "invalid_entity_id"},
      {"{\"version\":1,\"tracks\":[{\"id\":" + max + ",\"kind\":0}]}", "invalid_entity_id"},
      {"{\"version\":1,\"clips\":[{\"id\":0,\"length_ppq\":1}]}", "invalid_entity_id"},
      {"{\"version\":1,\"markers\":[{\"id\":" + max + "}]}", "invalid_entity_id"},
      {"{\"version\":1,\"sources\":[{\"kind\":0,\"id\":1},{\"kind\":0,\"id\":1}]}",
       "duplicate_entity_id"},
      {"{\"version\":1,\"tracks\":[{\"id\":1,\"kind\":0},{\"id\":1,\"kind\":0}]}",
       "duplicate_entity_id"},
      {"{\"version\":1,\"clips\":[{\"id\":1,\"length_ppq\":1},{\"id\":1,\"length_ppq\":1}]}",
       "duplicate_entity_id"},
      {"{\"version\":1,\"markers\":[{\"id\":1},{\"id\":1}]}", "duplicate_entity_id"},
  };

  for (const auto& [json, expected_code] : fixtures) {
    const auto result = project_from_json(json);
    CHECK_FALSE(result.ok());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().code == expected_code);
  }
}

TEST_CASE("dangling clip references and source-kind mismatch emit diagnostics", "[serialize]") {
  // A clip referencing a non-existent source and track.
  const std::string dangling =
      "{\"version\": 1, \"clips\": [{\"id\": 1, \"track_id\": 99, \"source_id\": 99, "
      "\"length_ppq\": 1.0}]}";
  auto r1 = project_from_json(dangling);
  REQUIRE(r1.ok());  // Verbatim load: warnings, not errors.
  bool dangling_src = false;
  bool dangling_trk = false;
  for (const auto& d : r1.diagnostics) {
    if (d.code == "dangling_clip_source") dangling_src = true;
    if (d.code == "dangling_clip_track") dangling_trk = true;
  }
  CHECK(dangling_src);
  CHECK(dangling_trk);

  // A MIDI source on an audio track => kind mismatch.
  const std::string mismatch =
      "{\"version\": 1, "
      "\"sources\": [{\"kind\": 1, \"id\": 1}], "  // MIDI source
      "\"tracks\": [{\"id\": 1, \"kind\": 0}], "   // audio track
      "\"clips\": [{\"id\": 1, \"track_id\": 1, \"source_id\": 1, \"length_ppq\": 1.0}]}";
  auto r2 = project_from_json(mismatch);
  REQUIRE(r2.ok());
  bool kind_mismatch = false;
  for (const auto& d : r2.diagnostics) {
    if (d.code == "clip_source_kind_mismatch") kind_mismatch = true;
  }
  CHECK(kind_mismatch);
}

TEST_CASE("loader derives the id counters from the live max-id scan", "[serialize]") {
  // The counters are deliberately not serialized (serialization is a pure
  // function of the visible arrangement so edit+undo restores exact bytes);
  // the loader re-derives them so a later add never collides with a LIVE id.
  const std::string doc =
      "{\"version\": 1, "
      "\"sources\": [{\"kind\": 0, \"id\": 7}], "
      "\"tracks\": [{\"id\": 3, \"kind\": 0}], "
      "\"clips\": [{\"id\": 5, \"track_id\": 3, \"source_id\": 7, \"length_ppq\": 1.0}]}";
  auto r = project_from_json(doc);
  REQUIRE(r.ok());
  CHECK(r.project->next_source_id() == 8);
  CHECK(r.project->next_track_id() == 4);
  CHECK(r.project->next_clip_id() == 6);
}

TEST_CASE("serialization is invariant under counter-only state (edit+undo byte equality)",
          "[serialize]") {
  Fixture f = make_fixture();
  Project& p = f.project;
  const auto before = project_to_json(p, f.midi);

  // Allocate and delete a clip: only the monotonic counters change. The
  // serialized bytes must be identical — this is the contract that keeps the
  // cross-binding "undo restores serialized bytes" tests true.
  AudioSourceRef extra_src;
  extra_src.uri = "file:///extra.wav";
  const SourceId sid = p.add_audio_source(extra_src);
  EditClip extra;
  extra.track_id = p.tracks().front().id;
  extra.source_id = sid;
  extra.start_ppq = 10000.0;
  extra.length_ppq = 1.0;
  const ClipId clip_id = p.add_clip(extra);
  REQUIRE(clip_id != 0);
  REQUIRE(p.remove_clip(clip_id).second);
  REQUIRE(p.remove_source(sid).second);

  CHECK(project_to_json(p, f.midi) == before);
}
