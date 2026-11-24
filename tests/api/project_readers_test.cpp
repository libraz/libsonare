#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "c_api/project_internal.h"
#include "midi/smf2.h"
#include "midi/ump.h"
#include "util/resource_limits.h"

namespace {

struct OwnedProject {
  SonareProject* handle = nullptr;

  OwnedProject() { REQUIRE(sonare_project_create(&handle) == SONARE_OK); }
  ~OwnedProject() { sonare_project_destroy(handle); }
  OwnedProject(const OwnedProject&) = delete;
  OwnedProject& operator=(const OwnedProject&) = delete;
};

struct OwnedProjectSegments {
  SonareProjectTempoSegmentList tempo{};
  SonareProjectTimeSignatureList signatures{};

  OwnedProjectSegments() = default;
  ~OwnedProjectSegments() {
    sonare_free_project_tempo_segment_list(&tempo);
    sonare_free_project_time_signature_list(&signatures);
  }
  OwnedProjectSegments(const OwnedProjectSegments&) = delete;
  OwnedProjectSegments& operator=(const OwnedProjectSegments&) = delete;
};

void check_refusal_subject(const char* expected) {
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  const char* subject = sonare_last_refusal_subject(0);
  REQUIRE(subject != nullptr);
  CHECK(std::string(subject) == expected);
}

}  // namespace

TEST_CASE("project track route reader returns owned route strings", "[c_api][project][readers]") {
  OwnedProject project;
  SonareProjectTrackDesc track{};
  track.kind = SONARE_TRACK_AUDIO;
  track.name = "routed";
  uint32_t routed_track_id = 0;
  REQUIRE(sonare_project_add_track(project.handle, &track, &routed_track_id) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project.handle, routed_track_id, "strip-main",
                                         "bus-master") == SONARE_OK);
  track.name = "default";
  uint32_t default_track_id = 0;
  REQUIRE(sonare_project_add_track(project.handle, &track, &default_track_id) == SONARE_OK);

  char* channel_strip_ref = nullptr;
  char* output_target = nullptr;
  REQUIRE(sonare_project_track_route_by_index(project.handle, 0, &channel_strip_ref,
                                              &output_target) == SONARE_OK);
  REQUIRE(channel_strip_ref != nullptr);
  REQUIRE(output_target != nullptr);
  CHECK(std::string(channel_strip_ref) == "strip-main");
  CHECK(std::string(output_target) == "bus-master");
  REQUIRE(sonare_project_set_track_route(project.handle, routed_track_id, channel_strip_ref,
                                         output_target) == SONARE_OK);
  sonare_free_string(channel_strip_ref);
  sonare_free_string(output_target);
  REQUIRE(sonare_project_track_route_by_index(project.handle, 0, &channel_strip_ref,
                                              &output_target) == SONARE_OK);
  REQUIRE(channel_strip_ref != nullptr);
  REQUIRE(output_target != nullptr);
  CHECK(std::string(channel_strip_ref) == "strip-main");
  CHECK(std::string(output_target) == "bus-master");
  sonare_free_string(channel_strip_ref);
  sonare_free_string(output_target);

  channel_strip_ref = nullptr;
  output_target = nullptr;
  REQUIRE(sonare_project_track_route_by_index(project.handle, 1, &channel_strip_ref,
                                              &output_target) == SONARE_OK);
  CHECK(channel_strip_ref == nullptr);
  CHECK(output_target == nullptr);

  channel_strip_ref = const_cast<char*>("sentinel");
  output_target = const_cast<char*>("sentinel");
  CHECK(sonare_project_track_route_by_index(project.handle, 2, &channel_strip_ref,
                                            &output_target) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(channel_strip_ref == nullptr);
  CHECK(output_target == nullptr);
  CHECK(std::string(sonare_last_error_message()).find("index") != std::string::npos);

  CHECK(sonare_project_track_route_by_index(project.handle, 0, &channel_strip_ref,
                                            &channel_strip_ref) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(channel_strip_ref == nullptr);
}

TEST_CASE("project mixer scene reader distinguishes absence from explicit default and clears",
          "[c_api][project][readers]") {
  OwnedProject project;
  char* scene_json = const_cast<char*>("sentinel");
  REQUIRE(sonare_project_mixer_scene_json(project.handle, &scene_json) == SONARE_OK);
  CHECK(scene_json == nullptr);

  constexpr const char* kDefaultScene = R"({"version":2,"strips":[],"buses":[],"connections":[]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, kDefaultScene) == SONARE_OK);
  REQUIRE(sonare_project_mixer_scene_json(project.handle, &scene_json) == SONARE_OK);
  REQUIRE(scene_json != nullptr);
  CHECK(std::string(scene_json).find("\"version\":2") != std::string::npos);
  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, scene_json) == SONARE_OK);
  const std::string first_scene(scene_json);
  sonare_free_string(scene_json);
  REQUIRE(sonare_project_mixer_scene_json(project.handle, &scene_json) == SONARE_OK);
  REQUIRE(scene_json != nullptr);
  CHECK(std::string(scene_json) == first_scene);
  sonare_free_string(scene_json);

  REQUIRE(sonare_project_set_mixer_scene_json(project.handle, nullptr) == SONARE_OK);
  scene_json = const_cast<char*>("sentinel");
  REQUIRE(sonare_project_mixer_scene_json(project.handle, &scene_json) == SONARE_OK);
  CHECK(scene_json == nullptr);
}

TEST_CASE("project source readers reject a released project handle", "[c_api][project][readers]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  sonare_project_destroy(project);
  project = nullptr;

  SECTION("unresolved count") {
    size_t count = 1;
    CHECK(sonare_project_unresolved_audio_source_count(project, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(count == 0);
  }
  SECTION("unresolved source ID") {
    uint32_t id = 1;
    CHECK(sonare_project_unresolved_audio_source_id_by_index(project, 0, &id) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(id == 0);
  }
  SECTION("unresolved source record") {
    SonareProjectSource source{};
    source.id = 1;
    CHECK(sonare_project_unresolved_audio_source_by_index(project, 0, &source) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(source.id == 0);
  }
  SECTION("source name or URI") {
    char sentinel = '\0';
    char* name = &sentinel;
    CHECK(sonare_project_source_name_or_uri_by_index(project, 0, &name) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(name == nullptr);
  }
  CHECK(std::string(sonare_last_error_message()) == "project must not be NULL");
}

namespace {

struct OwnedMidiEvents {
  SonareMidiEventList value{};

  OwnedMidiEvents(const SonareProject* project, uint32_t clip_id) {
    REQUIRE(sonare_project_midi_events(project, clip_id, &value) == SONARE_OK);
  }
  ~OwnedMidiEvents() { sonare_free_midi_event_list(&value); }
  OwnedMidiEvents(const OwnedMidiEvents&) = delete;
  OwnedMidiEvents& operator=(const OwnedMidiEvents&) = delete;
};

using ExportMidi = SonareError (*)(const SonareProject*, uint8_t**, size_t*);

std::vector<uint8_t> export_midi(const SonareProject* project, ExportMidi export_format) {
  uint8_t* bytes = nullptr;
  size_t size = 0;
  REQUIRE(export_format(project, &bytes, &size) == SONARE_OK);
  std::unique_ptr<uint8_t, decltype(&sonare_free_bytes)> owned(bytes, sonare_free_bytes);
  REQUIRE(bytes != nullptr);
  REQUIRE(size > 0);
  return {bytes, bytes + size};
}

std::vector<uint8_t> payload(const SonareMidiEventPod& event) {
  if (event.sysex_size == 0) {
    CHECK(event.sysex == nullptr);
    return {};
  }
  REQUIRE(event.sysex != nullptr);
  return {event.sysex, event.sysex + event.sysex_size};
}

void check_midi_round_trip(SonareProject* project, uint32_t clip_id,
                           const SonareMidiEventList& before, ExportMidi export_format) {
  const auto bytes_before = export_midi(project, export_format);
  REQUIRE(sonare_project_set_midi_events(project, clip_id, before.items, before.count) ==
          SONARE_OK);
  const OwnedMidiEvents after(project, clip_id);
  REQUIRE(after.value.count == before.count);
  for (size_t i = 0; i < before.count; ++i) {
    CAPTURE(i);
    const auto& previous = before.items[i];
    const auto& current = after.value.items[i];
    CHECK(current.ppq == previous.ppq);
    CHECK(current.word0 == previous.word0);
    CHECK(current.word1 == previous.word1);
    CHECK(payload(current) == payload(previous));
    if (current.sysex_size != 0) CHECK(current.sysex != previous.sysex);
  }
  CHECK(export_midi(project, export_format) == bytes_before);
}

constexpr uint32_t kSysExWord = 0x30000000u;
constexpr uint32_t kGroupThreeSysExWord = 0x33000000u;
constexpr uint8_t kGsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
constexpr uint8_t kGmOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
constexpr uint8_t kEmptyFrame[] = {0xF0, 0xF7};

std::vector<uint8_t> group_three_clip_file() {
  namespace midi = sonare::midi;
  midi::SysExStore store;
  const auto reset = store.add(kGsReset, sizeof(kGsReset));
  const auto gm = store.add(kGmOn, sizeof(kGmOn));
  REQUIRE(reset != 0);
  REQUIRE(gm != 0);
  constexpr uint8_t group = 3;
  midi::MidiClip clip;
  const auto add = [&](double ppq, midi::Ump ump) {
    midi::MidiClipEvent event{};
    event.ppq = ppq;
    event.ump = ump;
    clip.add_event(event);
  };
  add(0.0, midi::make_midi1_note_on(group, 0, 60, 100));
  add(1.0, midi::make_sysex_handle(group, reset));
  add(1.0, midi::make_midi1_control_change(group, 0, 7, 100));
  add(2.0, midi::make_sysex_handle(group, gm));
  add(3.0, midi::make_midi1_note_off(group, 0, 60, 0));
  midi::Smf2ExportOptions options;
  options.sysex_store = &store;
  options.length_ppq = 4.0;
  auto clip_file = midi::export_clip_file(clip, {}, {}, options);
  REQUIRE(clip_file.ok());
  REQUIRE(clip_file.skipped_events == 0);
  return clip_file.bytes;
}

}  // namespace

TEST_CASE("project MIDI event reader preserves imported SMF SysEx",
          "[c_api][project][readers][sysex]") {
  constexpr uint8_t smf[] = {
      'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,   0,   1,    0,    96,   'M',  'T',
      'r',  'k',  0,    0,    0,    41,   0,    0x90, 60,   100, 1,   0xF0, 10,   0x41, 0x10, 0x42,
      0x12, 0x40, 0,    0x7F, 0,    0x41, 0xF7, 0,    0xB0, 7,   100, 1,    0xF0, 5,    0x7E, 0x7F,
      0x09, 0x01, 0xF7, 1,    0xF0, 1,    0xF7, 1,    0x80, 60,  0,   1,    0xFF, 0x2F, 0,
  };
  OwnedProject project;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_import_smf(project.handle, smf, sizeof(smf), &clip_id) == SONARE_OK);
  const OwnedMidiEvents before(project.handle, clip_id);
  REQUIRE(before.value.count == 6);
  const auto* rows = before.value.items;
  CHECK(rows[0].word0 == 0x20903C64u);
  CHECK(rows[2].word0 == 0x20B00764u);
  CHECK(rows[5].word0 == 0x20803C00u);
  CHECK(rows[1].ppq == rows[2].ppq);
  for (size_t i : {1u, 3u, 4u}) {
    CHECK(rows[i].word0 == kSysExWord);
    CHECK(rows[i].word1 == 0);
  }
  // The file's leading F0 is the SMF status. Its trailing F7 is stored.
  CHECK(payload(rows[1]) == std::vector<uint8_t>(kGsReset + 1, std::end(kGsReset)));
  CHECK(payload(rows[3]) == std::vector<uint8_t>(kGmOn + 1, std::end(kGmOn)));
  CHECK(payload(rows[4]) == std::vector<uint8_t>{0xF7});
  check_midi_round_trip(project.handle, clip_id, before.value, sonare_project_export_smf);
}

TEST_CASE("project MIDI event reader preserves clip-file SysEx groups",
          "[c_api][project][readers][sysex]") {
  const auto clip_file = group_three_clip_file();
  OwnedProject project;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_import_clip_file(project.handle, clip_file.data(), clip_file.size(),
                                          &clip_id) == SONARE_OK);
  const OwnedMidiEvents before(project.handle, clip_id);
  REQUIRE(before.value.count == 5);
  const auto* rows = before.value.items;
  CHECK(rows[0].word0 == 0x23903C64u);
  CHECK(rows[1].word0 == kGroupThreeSysExWord);
  CHECK(rows[2].word0 == 0x23B00764u);
  CHECK(rows[3].word0 == kGroupThreeSysExWord);
  CHECK(rows[4].word0 == 0x23803C00u);
  CHECK(payload(rows[1]) == std::vector<uint8_t>(kGsReset + 1, std::end(kGsReset) - 1));
  CHECK(payload(rows[3]) == std::vector<uint8_t>(kGmOn + 1, std::end(kGmOn) - 1));
  check_midi_round_trip(project.handle, clip_id, before.value, sonare_project_export_smf);
  check_midi_round_trip(project.handle, clip_id, before.value, sonare_project_export_clip_file);
}

TEST_CASE("project MIDI event writer names refused SysEx fields",
          "[c_api][project][readers][sysex]") {
  OwnedProject project;
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project.handle, 0.0, 4.0, &track_id, &clip_id) == SONARE_OK);
  const SonareMidiEventPod valid{0.0, kSysExWord, 0, kEmptyFrame, sizeof(kEmptyFrame)};
  REQUIRE(sonare_project_set_midi_events(project.handle, clip_id, &valid, 1) == SONARE_OK);
  const auto refuse = [&](const SonareMidiEventPod* rows, size_t count, const char* subject) {
    REQUIRE(sonare_project_set_midi_events(project.handle, clip_id, rows, count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_last_refusal_subject_count() == 1);
    REQUIRE(sonare_last_refusal_subject(0) != nullptr);
    CHECK(std::string(sonare_last_refusal_subject(0)) == subject);
    const OwnedMidiEvents unchanged(project.handle, clip_id);
    REQUIRE(unchanged.value.count == 1);
    CHECK(payload(unchanged.value.items[0]) == std::vector<uint8_t>{0xF0, 0xF7});
  };
  SonareMidiEventPod bad = valid;
  SECTION("SysEx requires a pointer") {
    bad.sysex = nullptr;
    refuse(&bad, 1, "events[0].sysex");
  }
  SECTION("SysEx requires bytes") {
    bad.sysex_size = 0;
    refuse(&bad, 1, "events[0].sysex");
  }
  SECTION("a channel-voice event cannot have a SysEx pointer") {
    bad.word0 = 0x20903C64u;
    bad.sysex_size = 0;
    refuse(&bad, 1, "events[0].sysex");
  }
  SECTION("a channel-voice event cannot have a SysEx byte count") {
    bad.word0 = 0x20903C64u;
    bad.sysex = nullptr;
    refuse(&bad, 1, "events[0].sysex");
  }
  SECTION("SysEx reserves word0 bits 16 through 23") {
    bad.word0 |= 0x00010000u;
    refuse(&bad, 1, "events[0].word0");
  }
  SECTION("SysEx reserves word1") {
    bad.word1 = 1;
    refuse(&bad, 1, "events[0].word1");
  }
  SECTION("the first event crossing the cumulative limit is refused") {
    const size_t limit = sonare::resource::kProjectMidiImportResourceLimits.max_sysex_bytes;
    const std::vector<uint8_t> bytes(limit, 0);
    const SonareMidiEventPod rows[] = {{0.0, kSysExWord, 0, bytes.data(), bytes.size()}, valid};
    refuse(rows, 2, "events[1].sysex_size");
  }
}

TEST_CASE("C MIDI router borrows SysEx bytes and ignores its channels",
          "[c_api][project][readers][sysex]") {
  const SonareMidiEventPod events[] = {{0.0, kGroupThreeSysExWord, 0, kGsReset, sizeof(kGsReset)},
                                       {1.0, kSysExWord, 0, kGmOn, sizeof(kGmOn)},
                                       {2.0, 0x23913C64u, 0}};
  SonareMidiRouteConfig config{};
  sonare_midi_route_config_init(&config);
  config.filter_group = 3;
  config.filter_channel = 2;
  config.remap_channel = 9;
  SonareMidiEventPod out[3]{};
  size_t count = 0;
  REQUIRE(sonare_midi_route_events(events, 3, &config, out, 3, &count, nullptr, nullptr) ==
          SONARE_OK);
  REQUIRE(count == 1);
  CHECK(out[0].word0 == kGroupThreeSysExWord);
  CHECK(out[0].sysex == events[0].sysex);
  CHECK(out[0].sysex_size == events[0].sysex_size);

  SonareMidiCcBinding binding{};
  CHECK(sonare_midi_cc_learn(events, 1, 1, 0.0f, 1.0f, 0, &binding) == SONARE_ERROR_INVALID_STATE);
  SonareAutomationPoint point{};
  CHECK(sonare_midi_cc_to_breakpoint(nullptr, 0, &events[0], &point) == SONARE_ERROR_INVALID_STATE);
}

TEST_CASE("project MIDI CC conversions record their refused state",
          "[c_api][project][midi-error]") {
  const SonareMidiEventPod sysex{1.0, kGroupThreeSysExWord, 0, kGsReset, sizeof(kGsReset)};
  const SonareMidiEventPod cc{2.0, 0x23B00764u, 0};

  SECTION("learning requires a binding to emerge") {
    SonareMidiCcBinding binding{};
    binding.cc_number = 7;
    binding.param_id = 9;
    binding.max_value = 1;
    const SonareError status = sonare_midi_cc_learn(&sysex, 1, 1, 0, 1, 0, &binding);
    CHECK(status == SONARE_ERROR_INVALID_STATE);
    CHECK(sonare_last_error_code() == status);
    CHECK(std::string(sonare_last_error_message()) ==
          "no MIDI CC binding could be learned from events");
    CHECK(binding.cc_number == 0);
    CHECK(binding.param_id == 0);
    CHECK(binding.max_value == 0);
  }

  SECTION("SysEx is not converted to a breakpoint") {
    SonareAutomationPoint point{1.0, 1.0f, 1};
    const SonareError status = sonare_midi_cc_to_breakpoint(nullptr, 0, &sysex, &point);
    CHECK(status == SONARE_ERROR_INVALID_STATE);
    CHECK(sonare_last_error_code() == status);
    CHECK(std::string(sonare_last_error_message()) ==
          "MIDI CC conversion does not support SysEx events");
    CHECK(point.ppq == 0);
    CHECK(point.value == 0);
    CHECK(point.curve_to_next == 0);
  }

  SECTION("an unmatched controller yields no breakpoint") {
    SonareAutomationPoint point{1.0, 1.0f, 1};
    const SonareError status = sonare_midi_cc_to_breakpoint(nullptr, 0, &cc, &point);
    CHECK(status == SONARE_ERROR_INVALID_STATE);
    CHECK(sonare_last_error_code() == status);
    CHECK(std::string(sonare_last_error_message()) ==
          "no automation breakpoint could be built from event and bindings");
    CHECK(point.ppq == 0);
    CHECK(point.value == 0);
    CHECK(point.curve_to_next == 0);
  }

  SECTION("an unmatched parameter yields no event") {
    SonareMidiEventPod event = sysex;
    const SonareError status = sonare_midi_param_to_cc(nullptr, 0, 1, 0, 0, 0, &event);
    CHECK(status == SONARE_ERROR_INVALID_STATE);
    CHECK(sonare_last_error_code() == status);
    CHECK(std::string(sonare_last_error_message()) == "param_id has no MIDI CC binding");
    CHECK(event.ppq == 0);
    CHECK(event.word0 == 0);
    CHECK(event.word1 == 0);
    CHECK(event.sysex == nullptr);
    CHECK(event.sysex_size == 0);
  }
}

TEST_CASE("project MIDI Clip File import records the refusal cause",
          "[c_api][project][midi-error]") {
  OwnedProject project;
  const auto check_refusal = [&](const uint8_t* bytes, size_t len, const std::string& message) {
    uint32_t first_clip_id = 7;
    const SonareError status =
        sonare_project_import_clip_file(project.handle, bytes, len, &first_clip_id);
    CHECK(status == SONARE_ERROR_INVALID_FORMAT);
    CHECK(sonare_last_error_code() == status);
    CHECK(std::string(sonare_last_error_message()) == message);
    CHECK(first_clip_id == 0);
    CHECK(project.handle->history.project().clips().empty());
    CHECK_FALSE(project.handle->history.can_undo());
  };

  SECTION("file size limit") {
    const size_t limit = sonare::resource::kProjectMidiImportResourceLimits.max_file_bytes;
    const std::vector<uint8_t> bytes(limit + 1);
    check_refusal(
        bytes.data(), bytes.size(),
        "bytes exceeds the MIDI import file limit of " + std::to_string(limit) + " bytes");
  }

  SECTION("missing file header") {
    constexpr uint8_t bytes[8]{};
    check_refusal(bytes, sizeof(bytes), "missing SMF2CLIP file header");
  }
}

TEST_CASE("project MIDI export records the encoder refusal", "[c_api][project][midi-error]") {
  OwnedProject project;
  uint8_t byte = 0;
  uint8_t* bytes = &byte;
  size_t len = 7;
  SonareError status = SONARE_OK;
  const char* expected = nullptr;

  SECTION("MIDI Clip File clockstamp span") {
    uint32_t track_id = 0;
    uint32_t clip_id = 0;
    REQUIRE(sonare_project_add_midi_clip(project.handle, 0.0, sonare::transport::kMaxPublicPpq,
                                         &track_id, &clip_id) == SONARE_OK);
    status = sonare_project_export_clip_file(project.handle, &bytes, &len);
    expected = "clip spans more Delta Clockstamp ticks than the export limit";
  }

  SECTION("SMF data track count") {
    auto& history = project.handle->history;
    auto& model = history.project();
    arr::Track track{};
    track.kind = arr::Track::Kind::kMidi;
    arr::EditClip clip{};
    clip.track_id = model.add_track(std::move(track));
    clip.source_id = model.add_midi_source({});
    clip.length_ppq = 1;
    auto& events = history.midi_content().events;
    const uint32_t first_refused_count = std::numeric_limits<uint16_t>::max();
    for (uint32_t id = 1; id <= first_refused_count; ++id) {
      clip.id = id;
      if (!model.insert_clip_raw(clip, arr::Project::kAppend, true)) FAIL("could not seed clip");
      arr::MidiClipEvent note{};
      note.word0 = 0x20903C64u;
      events.emplace(id, arr::MidiClipEventList{note});
    }
    status = sonare_project_export_smf(project.handle, &bytes, &len);
    expected = "SMF format 1 supports at most 65534 data tracks";
  }

  if (status == SONARE_OK && bytes != &byte) sonare_free_bytes(bytes);
  CHECK(status == SONARE_ERROR_INVALID_STATE);
  CHECK(sonare_last_error_code() == status);
  CHECK(std::string(sonare_last_error_message()) == expected);
  CHECK(bytes == nullptr);
  CHECK(len == 0);
}

TEST_CASE("project stored segment readers return owned snapshots in stored order",
          "[c_api][project][readers][owned_list]") {
  OwnedProject project;
  OwnedProjectSegments lists;

  REQUIRE(sonare_project_tempo_segments(project.handle, &lists.tempo) == SONARE_OK);
  REQUIRE(sonare_project_time_signatures(project.handle, &lists.signatures) == SONARE_OK);
  CHECK(lists.tempo.items == nullptr);
  CHECK(lists.tempo.count == 0);
  CHECK(lists.signatures.items == nullptr);
  CHECK(lists.signatures.count == 0);

  SonareProjectPosition effective{};
  REQUIRE(sonare_project_position_at_ppq(project.handle, 0.0, &effective) == SONARE_OK);
  CHECK(effective.bpm == 120.0);
  CHECK(effective.time_signature.numerator == 4);
  CHECK(effective.time_signature.denominator == 4);

  const SonareProjectTempoSegment tempo_input[] = {{0.25, 91.0, 0.0}, {8.75, 137.0, 0.0}};
  const SonareProjectTimeSignatureSegment signature_input[] = {{0.25, 3, 4}, {8.75, 5, 8}};
  REQUIRE(sonare_project_set_tempo_segments(project.handle, tempo_input, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(project.handle, signature_input, 2) == SONARE_OK);
  REQUIRE(sonare_project_tempo_segments(project.handle, &lists.tempo) == SONARE_OK);
  REQUIRE(sonare_project_time_signatures(project.handle, &lists.signatures) == SONARE_OK);
  REQUIRE(lists.tempo.count == 2);
  REQUIRE(lists.signatures.count == 2);
  CHECK(lists.tempo.items[0].start_ppq == 0.25);
  CHECK(lists.tempo.items[0].bpm == 91.0);
  CHECK(lists.tempo.items[1].start_ppq == 8.75);
  CHECK(lists.tempo.items[1].bpm == 137.0);
  CHECK(lists.signatures.items[0].start_ppq == 0.25);
  CHECK(lists.signatures.items[0].numerator == 3);
  CHECK(lists.signatures.items[1].start_ppq == 8.75);
  CHECK(lists.signatures.items[1].numerator == 5);
  CHECK(lists.signatures.items[1].denominator == 8);

  const SonareProjectTempoSegment replacement_tempo[] = {{1.5, 120.0, 0.0}};
  const SonareProjectTimeSignatureSegment replacement_signature[] = {{1.5, 4, 4}};
  REQUIRE(sonare_project_set_tempo_segments(project.handle, replacement_tempo, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(project.handle, replacement_signature, 1) ==
          SONARE_OK);
  CHECK(lists.tempo.count == 2);
  CHECK(lists.tempo.items[0].start_ppq == 0.25);
  CHECK(lists.tempo.items[1].bpm == 137.0);
  CHECK(lists.signatures.count == 2);
  CHECK(lists.signatures.items[0].numerator == 3);
  CHECK(lists.signatures.items[1].start_ppq == 8.75);

  sonare_free_project_tempo_segment_list(&lists.tempo);
  sonare_free_project_time_signature_list(&lists.signatures);
  CHECK(lists.tempo.items == nullptr);
  CHECK(lists.tempo.count == 0);
  CHECK(lists.signatures.items == nullptr);
  CHECK(lists.signatures.count == 0);
  sonare_free_project_tempo_segment_list(&lists.tempo);
  sonare_free_project_time_signature_list(&lists.signatures);
  sonare_free_project_tempo_segment_list(nullptr);
  sonare_free_project_time_signature_list(nullptr);
}

TEST_CASE("project stored segment readers refuse null arguments by name",
          "[c_api][project][readers][refusal]") {
  OwnedProject project;

  SonareProjectTempoSegment tempo_sentinel{};
  SonareProjectTempoSegmentList tempo{&tempo_sentinel, 1};
  CHECK(sonare_project_tempo_segments(nullptr, &tempo) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(tempo.items == nullptr);
  CHECK(tempo.count == 0);
  check_refusal_subject("project");
  CHECK(sonare_project_tempo_segments(project.handle, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  check_refusal_subject("out");

  SonareProjectTimeSignatureSegment signature_sentinel{};
  SonareProjectTimeSignatureList signatures{&signature_sentinel, 1};
  CHECK(sonare_project_time_signatures(nullptr, &signatures) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(signatures.items == nullptr);
  CHECK(signatures.count == 0);
  check_refusal_subject("project");
  CHECK(sonare_project_time_signatures(project.handle, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  check_refusal_subject("out");
}

namespace {

template <typename List, void (*Release)(List*)>
struct OwnedAnnotations {
  List list{};

  OwnedAnnotations() = default;
  ~OwnedAnnotations() { Release(&list); }
  OwnedAnnotations(const OwnedAnnotations&) = delete;
  OwnedAnnotations& operator=(const OwnedAnnotations&) = delete;
};

template <typename List, typename Item>
void check_annotation_empty_and_refusals(const SonareProject* project, Item& sentinel,
                                         SonareError (*read)(const SonareProject*, List*),
                                         void (*release)(List*)) {
  List list{&sentinel, 1};
  CHECK(read(nullptr, &list) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(list.items == nullptr);
  CHECK(list.count == 0);
  check_refusal_subject("project");
  CHECK(read(project, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  check_refusal_subject("out");

  list = {&sentinel, 1};
  REQUIRE(read(project, &list) == SONARE_OK);
  CHECK(list.items == nullptr);
  CHECK(list.count == 0);
  release(&list);
  release(nullptr);
}

}  // namespace

TEST_CASE("project annotation readers return empty snapshots and name null arguments",
          "[c_api][project][readers][annotations][refusal]") {
  OwnedProject project;
  SonareProjectKeySegment key{};
  SonareProjectChordSymbol chord{};
  SonareProjectSection section{};
  check_annotation_empty_and_refusals(project.handle, key, sonare_project_key_annotations,
                                      sonare_free_project_key_segment_list);
  check_annotation_empty_and_refusals(project.handle, chord, sonare_project_chord_annotations,
                                      sonare_free_project_chord_symbol_list);
  check_annotation_empty_and_refusals(project.handle, section, sonare_project_section_annotations,
                                      sonare_free_project_section_list);
}

TEST_CASE("project key annotations round-trip in stored order",
          "[c_api][project][readers][annotations]") {
  OwnedAnnotations<SonareProjectKeySegmentList, &sonare_free_project_key_segment_list> snapshot;
  const SonareProjectKeySegment keys[] = {{8.25, 12, 4, 3}, {0, 2, 255, 0}, {0, 5, 11, 7}};
  {
    OwnedProject project;
    OwnedAnnotations<SonareProjectKeySegmentList, &sonare_free_project_key_segment_list> first;
    REQUIRE(sonare_project_annotate_keys(project.handle, keys, std::size(keys)) == SONARE_OK);
    REQUIRE(sonare_project_key_annotations(project.handle, &first.list) == SONARE_OK);
    REQUIRE(first.list.count == std::size(keys));
    REQUIRE(sonare_project_annotate_keys(project.handle, first.list.items, first.list.count) ==
            SONARE_OK);
    REQUIRE(sonare_project_key_annotations(project.handle, &snapshot.list) == SONARE_OK);
    CHECK(snapshot.list.items != first.list.items);
    REQUIRE(sonare_project_key_annotations(project.handle, &snapshot.list) == SONARE_OK);
    REQUIRE(sonare_project_annotate_keys(project.handle, nullptr, 0) == SONARE_OK);
    SonareProjectKeySegmentList cleared{};
    REQUIRE(sonare_project_key_annotations(project.handle, &cleared) == SONARE_OK);
    CHECK(cleared.items == nullptr);
    CHECK(cleared.count == 0);
    sonare_free_project_key_segment_list(&cleared);
  }
  REQUIRE(snapshot.list.count == std::size(keys));
  REQUIRE(snapshot.list.items != nullptr);
  for (size_t i = 0; i < std::size(keys); ++i) {
    const auto& item = snapshot.list.items[i];
    CHECK(item.start_ppq == keys[i].start_ppq);
    CHECK(item.end_ppq == keys[i].end_ppq);
    CHECK(item.tonic_pc == keys[i].tonic_pc);
    CHECK(item.mode == keys[i].mode);
  }
  sonare_free_project_key_segment_list(&snapshot.list);
  CHECK(snapshot.list.items == nullptr);
  CHECK(snapshot.list.count == 0);
}

TEST_CASE("project chord annotation snapshots own their extension and numeral data",
          "[c_api][project][readers][annotations]") {
  OwnedAnnotations<SonareProjectChordSymbolList, &sonare_free_project_chord_symbol_list> snapshot;
  const uint8_t extensions[] = {13, 7, 9};
  const SonareProjectChordSymbol chords[] = {
      {6.5, 9, 7, 5, extensions, std::size(extensions), 11, "V7/ii", 1},
      {0, 8, 255, 0, nullptr, 0, 255, nullptr, 0},
  };
  {
    OwnedProject project;
    OwnedAnnotations<SonareProjectChordSymbolList, &sonare_free_project_chord_symbol_list> first;
    REQUIRE(sonare_project_annotate_chords(project.handle, chords, std::size(chords)) == SONARE_OK);
    REQUIRE(sonare_project_chord_annotations(project.handle, &first.list) == SONARE_OK);
    REQUIRE(first.list.count == std::size(chords));
    REQUIRE(sonare_project_annotate_chords(project.handle, first.list.items, first.list.count) ==
            SONARE_OK);
    REQUIRE(sonare_project_chord_annotations(project.handle, &snapshot.list) == SONARE_OK);
    REQUIRE(snapshot.list.count == std::size(chords));
    REQUIRE(first.list.items != nullptr);
    REQUIRE(snapshot.list.items != nullptr);
    REQUIRE(snapshot.list.items != first.list.items);
    CHECK(snapshot.list.items[0].extensions != first.list.items[0].extensions);
    CHECK(snapshot.list.items[0].roman_numeral != first.list.items[0].roman_numeral);
    REQUIRE(sonare_project_annotate_chords(project.handle, nullptr, 0) == SONARE_OK);
    SonareProjectChordSymbolList cleared{};
    REQUIRE(sonare_project_chord_annotations(project.handle, &cleared) == SONARE_OK);
    CHECK(cleared.items == nullptr);
    CHECK(cleared.count == 0);
    sonare_free_project_chord_symbol_list(&cleared);
  }
  REQUIRE(snapshot.list.count == std::size(chords));
  REQUIRE(snapshot.list.items != nullptr);
  for (size_t i = 0; i < std::size(chords); ++i) {
    const auto& item = snapshot.list.items[i];
    CHECK(item.start_ppq == chords[i].start_ppq);
    CHECK(item.end_ppq == chords[i].end_ppq);
    CHECK(item.root_pc == chords[i].root_pc);
    CHECK(item.quality == chords[i].quality);
    CHECK(item.slash_bass_pc == chords[i].slash_bass_pc);
    CHECK(item.modulation_boundary == chords[i].modulation_boundary);
    REQUIRE(item.extension_count == chords[i].extension_count);
  }
  const auto& first = snapshot.list.items[0];
  REQUIRE(first.extensions != nullptr);
  CHECK(std::equal(std::begin(extensions), std::end(extensions), first.extensions));
  REQUIRE(first.roman_numeral != nullptr);
  CHECK(std::string(first.roman_numeral) == "V7/ii");
  CHECK(snapshot.list.items[1].extensions == nullptr);
  CHECK(snapshot.list.items[1].roman_numeral == nullptr);
  sonare_free_project_chord_symbol_list(&snapshot.list);
  CHECK(snapshot.list.items == nullptr);
  CHECK(snapshot.list.count == 0);
}

TEST_CASE("project section annotations preserve overlaps and own their labels",
          "[c_api][project][readers][annotations]") {
  OwnedAnnotations<SonareProjectSectionList, &sonare_free_project_section_list> snapshot;
  const SonareProjectSection sections[] = {{4, 10, "chorus"}, {0.5, 8, "verse"}, {0.5, 2, "intro"}};
  {
    OwnedProject project;
    OwnedAnnotations<SonareProjectSectionList, &sonare_free_project_section_list> first;
    REQUIRE(sonare_project_annotate_sections(project.handle, sections, std::size(sections)) ==
            SONARE_OK);
    REQUIRE(sonare_project_section_annotations(project.handle, &first.list) == SONARE_OK);
    REQUIRE(first.list.count == std::size(sections));
    REQUIRE(sonare_project_annotate_sections(project.handle, first.list.items, first.list.count) ==
            SONARE_OK);
    REQUIRE(sonare_project_section_annotations(project.handle, &snapshot.list) == SONARE_OK);
    REQUIRE(snapshot.list.count == std::size(sections));
    REQUIRE(first.list.items != nullptr);
    REQUIRE(snapshot.list.items != nullptr);
    REQUIRE(snapshot.list.items != first.list.items);
    CHECK(snapshot.list.items[0].label != first.list.items[0].label);
    REQUIRE(sonare_project_annotate_sections(project.handle, nullptr, 0) == SONARE_OK);
    SonareProjectSectionList cleared{};
    REQUIRE(sonare_project_section_annotations(project.handle, &cleared) == SONARE_OK);
    CHECK(cleared.items == nullptr);
    CHECK(cleared.count == 0);
    sonare_free_project_section_list(&cleared);
  }
  REQUIRE(snapshot.list.count == std::size(sections));
  REQUIRE(snapshot.list.items != nullptr);
  for (size_t i = 0; i < std::size(sections); ++i) {
    const auto& item = snapshot.list.items[i];
    CHECK(item.start_ppq == sections[i].start_ppq);
    CHECK(item.end_ppq == sections[i].end_ppq);
    REQUIRE(item.label != nullptr);
    CHECK(std::string(item.label) == sections[i].label);
  }
  sonare_free_project_section_list(&snapshot.list);
  CHECK(snapshot.list.items == nullptr);
  CHECK(snapshot.list.count == 0);
}
