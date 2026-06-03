/// @file binding_project_parity_test.cpp
/// @brief headless project parity test: drives the headless-project C ABI
///        (sonare_c_project.h) end to end and pins serialize byte-stability,
///        deterministic bounce, undo/redo, the ABI version, and malformed-input
///        handling — all through the C surface (no direct C++ model access).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "midi/smf.h"
#include "sonare_c.h"
#include "sonare_c_project.h"
#include "util/constants.h"

namespace {

// Builds a small but non-trivial project through the C ABI: 48 kHz, an audio
// track + audio clip carrying decoded stereo samples, and a MIDI track + clip
// with a couple of events. Returns the handle (caller destroys) and the ids.
struct BuiltProject {
  SonareProject* project = nullptr;
  uint32_t audio_track = 0;
  uint32_t audio_clip = 0;
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
};

std::vector<float> make_stereo_sine(int frames) {
  std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    out[static_cast<size_t>(i) * 2] =
        0.25f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 220.0 * t));
    out[static_cast<size_t>(i) * 2 + 1] =
        0.18f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 330.0 * t));
  }
  return out;
}

void push_u32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
  v->push_back(static_cast<uint8_t>(x & 0xFFu));
}

void push_u16(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
  v->push_back(static_cast<uint8_t>(x & 0xFFu));
}

void push_tag(std::vector<uint8_t>* v, const char* tag) {
  for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>(tag[i]));
}

std::vector<uint8_t> make_project_sysex_smf() {
  const std::vector<uint8_t> payload = {0x7E, 0x7F, 0x09, 0x01, 0xF7};
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xF0);
  body.push_back(static_cast<uint8_t>(payload.size()));
  body.insert(body.end(), payload.begin(), payload.end());
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(0x3C);
  body.push_back(0x40);
  body.push_back(0x83);
  body.push_back(0x60);
  body.push_back(0x80);
  body.push_back(0x3C);
  body.push_back(0x00);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 0);
  push_u16(&smf, 1);
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(body.size()));
  smf.insert(smf.end(), body.begin(), body.end());
  return smf;
}

BuiltProject build_project(const std::vector<float>& audio) {
  BuiltProject built;
  REQUIRE(sonare_project_create(&built.project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(built.project, 48000.0) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  REQUIRE(sonare_project_add_track(built.project, &track_desc, &built.audio_track) == SONARE_OK);
  REQUIRE(built.audio_track != 0);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = built.audio_track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 2.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = static_cast<int64_t>(audio.size() / 2);
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  REQUIRE(sonare_project_add_clip(built.project, &clip_desc, &built.audio_clip) == SONARE_OK);
  REQUIRE(built.audio_clip != 0);

  REQUIRE(sonare_project_add_midi_clip(built.project, 0.0, 4.0, &built.midi_track,
                                       &built.midi_clip) == SONARE_OK);
  REQUIRE(built.midi_clip != 0);

  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // UMP word
  events[0].data1 = 0u;
  events[1].ppq = 1.0;
  events[1].data0 = 0x20803C00u;
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(built.project, built.midi_clip, events, 2) == SONARE_OK);

  return built;
}

std::string serialize(const SonareProject* project) {
  char* json = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_serialize(project, &json, &len) == SONARE_OK);
  REQUIRE(json != nullptr);
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

}  // namespace

TEST_CASE("project ABI version is positive and matches the macro", "[project]") {
  REQUIRE(sonare_project_abi_version() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(sonare_project_abi_version() > 0u);
}

TEST_CASE("serialize round-trips byte-identically through the C surface", "[project]") {
  const std::vector<float> audio = make_stereo_sine(48000);
  BuiltProject built = build_project(audio);

  const std::string first = serialize(built.project);
  REQUIRE_FALSE(first.empty());

  // Deserialize into a SECOND project and re-serialize: byte-identical.
  SonareProject* second = nullptr;
  REQUIRE(sonare_project_deserialize(first.data(), first.size(), &second, nullptr) == SONARE_OK);
  REQUIRE(second != nullptr);
  const std::string round_tripped = serialize(second);
  REQUIRE(round_tripped == first);

  sonare_project_destroy(second);
  sonare_project_destroy(built.project);
}

TEST_CASE("bounce is bit-exact across two renders through the C surface", "[project]") {
  const std::vector<float> audio = make_stereo_sine(48000);
  BuiltProject built = build_project(audio);

  SonareProjectBounceOptions options{};
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* first = nullptr;
  size_t first_len = 0;
  REQUIRE(sonare_project_bounce(built.project, &options, &first, &first_len) == SONARE_OK);
  REQUIRE(first != nullptr);
  REQUIRE(first_len == static_cast<size_t>(options.total_frames) * 2);

  float* second = nullptr;
  size_t second_len = 0;
  REQUIRE(sonare_project_bounce(built.project, &options, &second, &second_len) == SONARE_OK);
  REQUIRE(second_len == first_len);

  REQUIRE(std::memcmp(first, second, first_len * sizeof(float)) == 0);

  sonare_free_floats(first);
  sonare_free_floats(second);
  sonare_project_destroy(built.project);
}

TEST_CASE("undo restores the serialized bytes through the C surface", "[project]") {
  const std::vector<float> audio = make_stereo_sine(4800);
  BuiltProject built = build_project(audio);

  const std::string before = serialize(built.project);

  // An edit that mutates the model, then undo it.
  uint32_t new_clip = 0;
  REQUIRE(sonare_project_split_clip(built.project, built.audio_clip, 1.0, &new_clip) == SONARE_OK);
  const std::string after = serialize(built.project);
  REQUIRE(after != before);

  REQUIRE(sonare_project_undo(built.project) == SONARE_OK);
  const std::string restored = serialize(built.project);
  REQUIRE(restored == before);

  // Redo re-applies.
  REQUIRE(sonare_project_redo(built.project) == SONARE_OK);
  REQUIRE(serialize(built.project) == after);

  sonare_project_destroy(built.project);
}

TEST_CASE("compile surfaces a renderable timeline for a valid project", "[project]") {
  const std::vector<float> audio = make_stereo_sine(4800);
  BuiltProject built = build_project(audio);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(built.project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline == 1);
  sonare_project_free_compile_result(&result);
  REQUIRE(result.diagnostics == nullptr);
  REQUIRE(result.messages == nullptr);

  sonare_project_destroy(built.project);
}

TEST_CASE("malformed deserialize returns an error without crashing", "[project]") {
  const std::string garbage = "{ this is not valid project json ]]";
  SonareProject* project = reinterpret_cast<SonareProject*>(0x1);  // sentinel
  char* diag = nullptr;
  const SonareError err =
      sonare_project_deserialize(garbage.data(), garbage.size(), &project, &diag);
  REQUIRE(err != SONARE_OK);
  REQUIRE(project == nullptr);
  if (diag) sonare_free_string(diag);

  // Truncated empty buffer is also handled.
  SonareProject* empty = reinterpret_cast<SonareProject*>(0x1);
  REQUIRE(sonare_project_deserialize("", 0, &empty, nullptr) != SONARE_OK);
  REQUIRE(empty == nullptr);
}

TEST_CASE("snap_to_grid snaps a near-beat coordinate to the beat line", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  double snapped = -1.0;
  REQUIRE(sonare_project_snap_to_grid(project, 1.02, 1.0, &snapped) == SONARE_OK);
  REQUIRE(snapped == 1.0);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface composite edits roll back on failure", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);

  SonareProjectClipDesc first{};
  first.track_id = track;
  first.start_ppq = 0.0;
  first.length_ppq = 4.0;
  first.source_uri = "asset://a";
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &first, &clip) == SONARE_OK);
  const std::string before_overlap = serialize(project);

  SonareProjectClipDesc overlapping{};
  overlapping.track_id = track;
  overlapping.start_ppq = 2.0;
  overlapping.length_ppq = 1.0;
  overlapping.source_uri = "asset://b";
  uint32_t failed_clip = 123;
  REQUIRE(sonare_project_add_clip(project, &overlapping, &failed_clip) != SONARE_OK);
  REQUIRE(failed_clip == 0);
  REQUIRE(serialize(project) == before_overlap);

  const std::string before_midi = serialize(project);
  uint32_t failed_track = 123;
  failed_clip = 123;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, -1.0, &failed_track, &failed_clip) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(failed_track == 0);
  REQUIRE(failed_clip == 0);
  REQUIRE(serialize(project) == before_midi);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface exports MIDI events to SMF", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);

  SonareMidiEventPod events[5]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 60, 100, &events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_poly_pressure(0.25, 0, 0, 60, 70, &events[1]) == SONARE_OK);
  REQUIRE(sonare_midi_channel_pressure(0.50, 0, 0, 88, &events[2]) == SONARE_OK);
  REQUIRE(sonare_midi_pitch_bend(0.75, 0, 0, 0x1234, &events[3]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.0, 0, 0, 60, 0, &events[4]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 5) == SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  REQUIRE(bytes != nullptr);
  REQUIRE(len > 0);

  const auto imported = sonare::midi::import_smf(bytes, len);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 5);
  REQUIRE(imported.clips[0].events()[0].ump.is_note_on());
  REQUIRE(imported.clips[0].events()[1].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kPolyPressure));
  REQUIRE(imported.clips[0].events()[2].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kChannelPressure));
  REQUIRE(imported.clips[0].events()[3].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kPitchBend));
  REQUIRE(imported.clips[0].events()[4].ump.is_note_off());

  sonare_free_bytes(bytes);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface validates MIDI event helpers and input", "[project]") {
  SonareMidiEventPod event{};
  REQUIRE(sonare_midi_cc(0.5, 0, 0, 74, 127, &event) == SONARE_OK);
  REQUIRE(event.ppq == 0.5);
  REQUIRE(event.data0 != 0);
  REQUIRE(sonare_midi_poly_pressure(0.5, 0, 0, 60, 127, &event) == SONARE_OK);
  REQUIRE(sonare_midi_channel_pressure(0.5, 0, 0, 127, &event) == SONARE_OK);
  REQUIRE(sonare_midi_pitch_bend(0.5, 0, 0, 8192, &event) == SONARE_OK);
  REQUIRE(sonare_midi_note_on(0.0, 16, 0, 60, 100, &event) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_midi_pitch_bend(0.0, 0, 0, 16384, &event) == SONARE_ERROR_INVALID_PARAMETER);

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);
  const std::string before = serialize(project);

  SonareMidiEventPod invalid{};
  invalid.ppq = 0.0;
  invalid.data0 = 0xFFFFFFFFu;  // Not a supported MIDI 1.0/2.0 channel-voice UMP.
  REQUIRE(sonare_project_set_midi_events(project, clip, &invalid, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == before);

  double snapped = 0.0;
  REQUIRE(sonare_project_snap_to_grid(project, 1.0, 1.5, &snapped) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface imports SMF time signatures and markers", "[project]") {
  const std::vector<uint8_t> smf = {
      'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x01, 0xE0, 'M',
      'T',  'r',  'k',  0x00, 0x00, 0x00, 0x1D, 0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00,
      0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08, 0x83, 0x60, 0xFF, 0x06, 0x05, 'v',  'e',  'r',
      's',  'e',  0x00, 0xFF, 0x2F, 0x00, 'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x0D, 0x00,
      0x90, 0x3C, 0x40, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_smf(project, smf.data(), smf.size(), &first_clip) == SONARE_OK);
  REQUIRE(first_clip != 0);

  const std::string json = serialize(project);
  REQUIRE(json.find("\"time_signatures\":[{\"denominator\":4,\"numerator\":3,\"start_ppq\":0}]") !=
          std::string::npos);
  REQUIRE(json.find("\"markers\":[{\"id\":1,\"name\":\"verse\",\"ppq\":1}]") != std::string::npos);
  REQUIRE(json.find("\"tempo_segments\":[{\"bpm\":120") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  const std::string after_undo = serialize(project);
  REQUIRE(after_undo.find("\"markers\":[]") != std::string::npos);
  REQUIRE(after_undo.find("\"clips\":[]") != std::string::npos);

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  const std::string after_redo = serialize(project);
  REQUIRE(after_redo.find("\"name\":\"verse\"") != std::string::npos);
  REQUIRE(after_redo.find("\"numerator\":3") != std::string::npos);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface preserves SMF SysEx through project serialization", "[project]") {
  const std::vector<uint8_t> smf = make_project_sysex_smf();
  const std::vector<uint8_t> payload = {0x7E, 0x7F, 0x09, 0x01, 0xF7};

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_smf(project, smf.data(), smf.size(), &first_clip) == SONARE_OK);
  REQUIRE(first_clip != 0);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  const std::string after_undo = serialize(project);
  REQUIRE(after_undo.find("__sysex_payloads") == std::string::npos);
  REQUIRE(after_undo.find("sysex_handle") == std::string::npos);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);

  const std::string json = serialize(project);
  REQUIRE(json.find("__sysex_payloads") != std::string::npos);
  REQUIRE(json.find("sysex_handle") != std::string::npos);

  SonareProject* restored = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &restored, nullptr) == SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(restored, &bytes, &len) == SONARE_OK);
  REQUIRE(bytes != nullptr);
  REQUIRE(len > 0);

  const auto imported = sonare::midi::import_smf(bytes, len);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);

  bool saw_sysex = false;
  bool saw_note = false;
  for (const auto& event : imported.clips[0].events()) {
    if (event.ump.sysex_handle != 0) {
      const std::vector<uint8_t>* round_payload =
          imported.sysex_store.lookup(event.ump.sysex_handle);
      REQUIRE(round_payload != nullptr);
      REQUIRE(*round_payload == payload);
      saw_sysex = true;
    }
    if (event.ump.is_note_on() && event.ump.note_number() == 60) {
      saw_note = true;
    }
  }
  REQUIRE(saw_sysex);
  REQUIRE(saw_note);

  sonare_free_bytes(bytes);
  sonare_project_destroy(restored);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface rejects saturated SysEx handle allocation", "[project]") {
  const std::vector<uint8_t> smf = make_project_sysex_smf();

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_smf(project, smf.data(), smf.size(), &first_clip) == SONARE_OK);
  REQUIRE(first_clip != 0);

  std::string json = serialize(project);
  const std::string max_handle = std::to_string(std::numeric_limits<uint32_t>::max());
  const std::string payload_prefix = "\"__sysex_payloads\":{\"1\":";
  const size_t payload_pos = json.find(payload_prefix);
  REQUIRE(payload_pos != std::string::npos);
  json.replace(payload_pos, payload_prefix.size(), "\"__sysex_payloads\":{\"" + max_handle + "\":");

  const std::string event_handle = "\"sysex_handle\":1";
  const size_t event_pos = json.find(event_handle);
  REQUIRE(event_pos != std::string::npos);
  json.replace(event_pos, event_handle.size(), "\"sysex_handle\":" + max_handle);

  SonareProject* saturated = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &saturated, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_import_smf(saturated, smf.data(), smf.size(), nullptr) ==
          SONARE_ERROR_INVALID_STATE);

  sonare_project_destroy(saturated);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface set_program emits bank select and program change", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod existing{};
  REQUIRE(sonare_midi_program(0.0, 0, 2, 11, &existing) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, &existing, 1) == SONARE_OK);

  REQUIRE(sonare_project_set_program_on_channel(project, clip, 0, 3, 24, 0x0123) == SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  const auto imported = sonare::midi::import_smf(bytes, len);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 4);

  bool preserved_other_channel = false;
  bool saw_bank_msb = false;
  bool saw_bank_lsb = false;
  bool saw_program = false;
  for (const auto& event : imported.clips[0].events()) {
    const uint8_t status = event.ump.status_nibble();
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange) &&
        event.ump.channel() == 2 && event.ump.note_number() == 11) {
      preserved_other_channel = true;
    }
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange) &&
        event.ump.channel() == 3 && event.ump.note_number() == 0) {
      saw_bank_msb = true;
    }
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange) &&
        event.ump.channel() == 3 && event.ump.note_number() == 32) {
      saw_bank_lsb = true;
    }
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange) &&
        event.ump.channel() == 3 && event.ump.note_number() == 24) {
      saw_program = true;
    }
  }
  REQUIRE(preserved_other_channel);
  REQUIRE(saw_bank_msb);
  REQUIRE(saw_bank_lsb);
  REQUIRE(saw_program);

  sonare_free_bytes(bytes);

  REQUIRE(sonare_project_set_program_on_channel(project, clip, 0, 3, 25, -1) == SONARE_OK);
  bytes = nullptr;
  len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  const auto replaced = sonare::midi::import_smf(bytes, len);
  REQUIRE(replaced.ok());

  int channel3_cc_count = 0;
  bool saw_replaced_program = false;
  preserved_other_channel = false;
  for (const auto& event : replaced.clips[0].events()) {
    const uint8_t status = event.ump.status_nibble();
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange) &&
        event.ump.channel() == 3) {
      ++channel3_cc_count;
    }
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange) &&
        event.ump.channel() == 3 && event.ump.note_number() == 25) {
      saw_replaced_program = true;
    }
    if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange) &&
        event.ump.channel() == 2 && event.ump.note_number() == 11) {
      preserved_other_channel = true;
    }
  }
  REQUIRE(channel3_cc_count == 0);
  REQUIRE(saw_replaced_program);
  REQUIRE(preserved_other_channel);

  sonare_free_bytes(bytes);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface set_midi_fx transforms stored MIDI events", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  SonareMidiEventPod events[2]{};
  REQUIRE(sonare_midi_note_on(0.10, 0, 0, 60, 100, &events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.10, 0, 0, 60, 0, &events[1]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);

  const char* config =
      "{\"transpose_semitones\":12,\"quantize_ppq\":0.25,\"quantize_strength\":1.0,"
      "\"velocity_scale\":0.5,\"chord_intervals\":[0,7]}";
  REQUIRE(sonare_project_set_midi_fx(project, clip, config) == SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  const auto imported = sonare::midi::import_smf(bytes, len);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 4);

  bool saw_c5 = false;
  bool saw_g5 = false;
  for (const auto& event : imported.clips[0].events()) {
    REQUIRE((event.ppq == 0.0 || event.ppq == 1.0));
    if (event.ump.is_note_on() && event.ump.note_number() == 72) {
      saw_c5 = true;
    }
    if (event.ump.is_note_on() && event.ump.note_number() == 79) {
      saw_g5 = true;
    }
  }
  REQUIRE(saw_c5);
  REQUIRE(saw_g5);

  REQUIRE(sonare_project_set_midi_fx(project, clip, "{bad json") == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_project_set_midi_fx(project, clip, "{\"quantize_ppq\":0}") ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_free_bytes(bytes);
  sonare_project_destroy(project);
}
