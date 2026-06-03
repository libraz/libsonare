/// @file project_serializer_roundtrip_test.cpp
/// @brief project serializer byte-equality, round-trip fidelity, unknown-field-ignore, and
///        AssistSidecar lossless tests for the project serializer.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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
  aclip.warp_ref_id = 5;
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

  // Markers (owned names).
  p.add_marker(0.0, "Intro");
  p.add_marker(1920.0, "Verse \"A\"");

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
  mixing::api::Send send;
  send.id = "send.reverb";
  send.destination_bus_id = "bus.fx";
  send.send_db = -6.0f;
  send.timing = mixing::api::SendTiming::PostFader;
  s.sends.push_back(send);
  scene.strips.push_back(s);
  mixing::api::Bus master("bus.main", "master");
  scene.buses.push_back(master);
  mixing::api::Bus fx("bus.fx", "aux");
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
         a.warp_ref_id == b.warp_ref_id;
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
      a.channel_delay_samples != b.channel_delay_samples || a.inserts.size() != b.inserts.size() ||
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
  if (a.id != b.id || a.role != b.role || a.inserts.size() != b.inserts.size()) return false;
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

  REQUIRE(strip["sends"].is_array());
  REQUIRE_FALSE(strip["sends"].as_array().empty());
  const auto& send = strip["sends"].as_array()[0];
  CHECK(send.contains("destinationBusId"));
  CHECK(send.contains("sendDb"));
  CHECK_FALSE(send.contains("destination_bus_id"));
  CHECK_FALSE(send.contains("send_db"));
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
      "\"inserts\":[],\"sends\":[{\"id\":\"send\",\"destination_bus_id\":\"bus\","
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
