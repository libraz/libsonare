/// @file binding_project_midi_test.cpp
/// @brief Project MIDI parity tests.

#include "binding_project_parity_test_helpers.h"

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

TEST_CASE("project C surface round-trips a MIDI 2.0 Clip File losslessly", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);

  // A 16-bit velocity whose low 9 bits would be lost through MIDI 1.0 SMF.
  const uint16_t velocity16 = 0xBEEF;
  const sonare::midi::Ump note_on = sonare::midi::make_midi2_note_on(0, 0, 60, velocity16);
  const sonare::midi::Ump note_off = sonare::midi::make_midi2_note_off(0, 0, 60, 0);
  SonareMidiEventPod events[2]{};
  events[0].ppq = 0.0;
  events[0].data0 = note_on.words[0];
  events[0].data1 = note_on.words[1];
  events[1].ppq = 1.0;
  events[1].data0 = note_off.words[0];
  events[1].data1 = note_off.words[1];
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_clip_file(project, &bytes, &len) == SONARE_OK);
  REQUIRE(bytes != nullptr);
  REQUIRE(len >= 8);
  REQUIRE(std::string(bytes, bytes + 8) == "SMF2CLIP");

  // Re-import through the C surface into a fresh project.
  SonareProject* reimported = nullptr;
  REQUIRE(sonare_project_create(&reimported) == SONARE_OK);
  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_clip_file(reimported, bytes, len, &first_clip) == SONARE_OK);
  REQUIRE(first_clip != 0);

  // The MIDI 2.0 note survives bit-for-bit through the container.
  const auto roundtrip = sonare::midi::import_clip_file(bytes, len);
  REQUIRE(roundtrip.ok());
  REQUIRE(roundtrip.clips.size() == 1);
  const auto& imported_events = roundtrip.clips[0].events();
  REQUIRE(imported_events.size() == 2);
  const sonare::midi::Ump& on =
      imported_events[0].ump.is_note_on() ? imported_events[0].ump : imported_events[1].ump;
  REQUIRE(on.message_type() == sonare::midi::UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(static_cast<uint16_t>(on.words[1] >> 16) == velocity16);

  sonare_free_bytes(bytes);
  sonare_project_destroy(reimported);
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
      's',  'e',  0x00, 0xFF, 0x2F, 0x00, 'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x0E, 0x00,
      0x90, 0x3C, 0x40, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x83, 0x60, 0xFF, 0x2F, 0x00};

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
  REQUIRE(json.find("\"length_ppq\":2") != std::string::npos);

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

TEST_CASE("project C surface validates MIDI note pairing", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  SonareNotePairValidation report{};
  // Null / invalid arguments are rejected.
  REQUIRE(sonare_project_validate_midi_notes(nullptr, clip, &report) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_validate_midi_notes(project, clip, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_validate_midi_notes(project, 999999, &report) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A well-paired clip validates clean.
  SonareMidiEventPod paired[2]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 60, 100, &paired[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.0, 0, 0, 60, 0, &paired[1]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, paired, 2) == SONARE_OK);
  REQUIRE(sonare_project_validate_midi_notes(project, clip, &report) == SONARE_OK);
  REQUIRE(report.ok == 1);
  REQUIRE(report.unmatched_note_ons == 0);
  REQUIRE(report.unmatched_note_offs == 0);

  // A hanging note-on (no matching note-off) is reported.
  SonareMidiEventPod hanging[1]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 64, 100, &hanging[0]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, hanging, 1) == SONARE_OK);
  REQUIRE(sonare_project_validate_midi_notes(project, clip, &report) == SONARE_OK);
  REQUIRE(report.ok == 0);
  REQUIRE(report.unmatched_note_ons == 1);

  sonare_project_destroy(project);
}
