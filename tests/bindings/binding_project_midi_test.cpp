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

  REQUIRE(std::strcmp(sonare_midi_gm_instrument_name(0), "Acoustic Grand Piano") == 0);
  REQUIRE(std::strcmp(sonare_midi_gm_instrument_name(40), "Violin") == 0);
  REQUIRE(sonare_midi_gm_instrument_name(128) == nullptr);
  REQUIRE(sonare_midi_gm_program_for_name("Violin") == 40);
  REQUIRE(sonare_midi_gm_program_for_name("No Such Instrument") == -1);
  REQUIRE(std::strcmp(sonare_midi_gm_family_name(4), "Bass") == 0);
  REQUIRE(sonare_midi_gm_family_first_program(4) == 32);
  REQUIRE(std::strcmp(sonare_midi_gm2_instrument_name(1, 24), "Ukulele") == 0);
  REQUIRE(std::strcmp(sonare_midi_gm_drum_name(38), "Acoustic Snare") == 0);
  REQUIRE(sonare_midi_gm_drum_note_for_name("Open Triangle") == 81);
  REQUIRE(std::strcmp(sonare_midi_gm2_drum_set_name(40), "Brush") == 0);
  REQUIRE(std::strcmp(sonare_midi_gm2_drum_name(40, 40), "Brush Swirl") == 0);
  REQUIRE(std::strcmp(sonare_midi_cc_name(74), "Brightness") == 0);
  REQUIRE(sonare_midi_cc_index_for_name("Pan (MSB)") == 10);
  REQUIRE(std::strcmp(sonare_midi_per_note_controller_name(11), "Expression") == 0);

  SonareMidiEventPod program_events[3]{};
  size_t program_event_count = 0;
  REQUIRE(sonare_midi_bank_program(0.0, 0, 3, 0x79, 1, 24, program_events, 3,
                                   &program_event_count) == SONARE_OK);
  REQUIRE(program_event_count == 3);
  sonare::midi::Ump program_umps[3]{};
  for (size_t i = 0; i < program_event_count; ++i) {
    program_umps[i].words[0] = program_events[i].data0;
    program_umps[i].words[1] = program_events[i].data1;
    program_umps[i].word_count = program_events[i].data1 != 0 ? 2 : 1;
  }
  REQUIRE(program_umps[0].status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));
  REQUIRE(program_umps[1].status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));
  REQUIRE(program_umps[2].status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange));
  REQUIRE(sonare_midi_bank_program(0.0, 0, 3, 0x79, 1, 24, program_events, 2,
                                   &program_event_count) == SONARE_ERROR_INVALID_PARAMETER);

  SonareMidiEventPod route_input[3]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 3, 60, 100, &route_input[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(0.5, 0, 3, 60, 0, &route_input[1]) == SONARE_OK);
  REQUIRE(sonare_midi_note_on(1.0, 0, 2, 61, 100, &route_input[2]) == SONARE_OK);
  SonareMidiRouteConfig route_config{-1, 3, 7, 1};
  SonareMidiEventPod routed[2]{};
  size_t routed_count = 0;
  int route_overflowed = 0;
  uint32_t route_overflow_count = 0;
  REQUIRE(sonare_midi_route_events(route_input, 3, &route_config, routed, 2, &routed_count,
                                   &route_overflowed, &route_overflow_count) == SONARE_OK);
  REQUIRE(routed_count == 2);
  REQUIRE(route_overflowed == 0);
  REQUIRE(route_overflow_count == 0);
  sonare::midi::Ump routed_umps[2]{};
  for (size_t i = 0; i < routed_count; ++i) {
    routed_umps[i].words[0] = routed[i].data0;
    routed_umps[i].words[1] = routed[i].data1;
    routed_umps[i].word_count = routed[i].data1 != 0 ? 2 : 1;
  }
  REQUIRE(routed[0].ppq == 0.0);
  REQUIRE(routed[1].ppq == 0.5);
  REQUIRE(routed_umps[0].channel() == 7);
  REQUIRE(routed_umps[1].channel() == 7);
  REQUIRE(routed_umps[0].note_number() == 60);
  REQUIRE(routed_umps[1].is_note_off());

  route_config.thru = 0;
  REQUIRE(sonare_midi_route_events(route_input, 3, &route_config, routed, 2, &routed_count,
                                   &route_overflowed, &route_overflow_count) == SONARE_OK);
  REQUIRE(routed_count == 0);
  REQUIRE(route_overflowed == 0);

  route_config = SonareMidiRouteConfig{-1, -1, -1, 1};
  REQUIRE(sonare_midi_route_events(route_input, 3, &route_config, routed, 1, &routed_count,
                                   &route_overflowed, &route_overflow_count) == SONARE_OK);
  REQUIRE(routed_count == 1);
  REQUIRE(route_overflowed == 1);
  REQUIRE(route_overflow_count == 2);

  SonareMidiEventPod learn_events[2]{};
  REQUIRE(sonare_midi_cc(0.0, 0, 2, 1, 64, &learn_events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_cc(0.1, 0, 2, 33, 12, &learn_events[1]) == SONARE_OK);
  SonareMidiCcBinding learned{};
  REQUIRE(sonare_midi_cc_learn(learn_events, 2, 77, -1.0f, 1.0f, 0, &learned) == SONARE_OK);
  REQUIRE(learned.kind == SONARE_MIDI_CC_CONTROL_CHANGE_14);
  REQUIRE(learned.cc_number == 1);
  REQUIRE(learned.cc_lsb_number == 33);
  REQUIRE(learned.channel == 2);
  REQUIRE(learned.param_id == 77);

  SonareMidiEventPod rpn_events[3]{};
  REQUIRE(sonare_midi_cc(0.0, 0, 3, 101, 0, &rpn_events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_cc(0.1, 0, 3, 100, 1, &rpn_events[1]) == SONARE_OK);
  REQUIRE(sonare_midi_cc(0.2, 0, 3, 6, 64, &rpn_events[2]) == SONARE_OK);
  REQUIRE(sonare_midi_cc_learn(rpn_events, 3, 78, 0.0f, 1.0f, 0, &learned) == SONARE_OK);
  REQUIRE(learned.kind == SONARE_MIDI_CC_RPN);
  REQUIRE(learned.selector_msb == 0);
  REQUIRE(learned.selector_lsb == 1);
  REQUIRE(learned.param_id == 78);

  SonareMidiCcBinding cc_binding{};
  cc_binding.cc_number = 74;
  cc_binding.channel = 4;
  cc_binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  cc_binding.param_id = 88;
  cc_binding.min_value = -60.0f;
  cc_binding.max_value = 0.0f;
  SonareMidiEventPod cc_event{};
  REQUIRE(sonare_midi_cc(2.0, 0, 4, 74, 127, &cc_event) == SONARE_OK);
  SonareAutomationPoint point{};
  REQUIRE(sonare_midi_cc_to_breakpoint(&cc_binding, 1, &cc_event, &point) == SONARE_OK);
  REQUIRE(point.ppq == 2.0);
  REQUIRE(point.value == Catch::Approx(0.0f));
  REQUIRE(point.curve_to_next == 0);
  SonareMidiEventPod back_cc{};
  REQUIRE(sonare_midi_param_to_cc(&cc_binding, 1, 88, -60.0f, 0, 3.0, &back_cc) == SONARE_OK);
  sonare::midi::Ump back_ump{};
  back_ump.words[0] = back_cc.data0;
  back_ump.words[1] = back_cc.data1;
  back_ump.word_count = back_cc.data1 != 0 ? 2 : 1;
  REQUIRE(back_cc.ppq == 3.0);
  REQUIRE(back_ump.channel() == 4);
  REQUIRE(back_ump.note_number() == 74);
  REQUIRE((back_ump.words[0] & 0x7Fu) == 0u);

  SonareMidiCcBinding rpn_binding{};
  rpn_binding.cc_number = 6;
  rpn_binding.channel = 3;
  rpn_binding.kind = SONARE_MIDI_CC_RPN;
  rpn_binding.selector_msb = 0;
  rpn_binding.selector_lsb = 1;
  rpn_binding.param_id = 78;
  rpn_binding.min_value = 0.0f;
  rpn_binding.max_value = 1.0f;
  REQUIRE(sonare_midi_param_to_cc(&rpn_binding, 1, 78, 0.5f, 0, 4.0, &back_cc) ==
          SONARE_ERROR_INVALID_STATE);

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

TEST_CASE("project C surface imports SMF text-class meta with structured kinds", "[project]") {
  // Format 0, 480 PPQN: text / lyric / cue / marker / key signature + a note.
  std::vector<uint8_t> body;
  auto meta_text = [&](uint8_t type, const std::string& s) {
    body.push_back(0x00);
    body.push_back(0xFF);
    body.push_back(type);
    body.push_back(static_cast<uint8_t>(s.size()));
    for (char c : s) body.push_back(static_cast<uint8_t>(c));
  };
  meta_text(0x01, "memo");   // Text.
  meta_text(0x05, "la");     // Lyric.
  meta_text(0x07, "cue1");   // Cue point.
  meta_text(0x06, "verse");  // Marker.
  // Key signature sf=1 mi=1 (E minor).
  for (uint8_t b : {0x00u, 0xFFu, 0x59u, 0x02u, 0x01u, 0x01u}) body.push_back(b);
  // Note-on / note-off so a clip exists.
  for (uint8_t b : {0x00u, 0x90u, 0x3Cu, 0x64u, 0x83u, 0x60u, 0x80u, 0x3Cu, 0x00u})
    body.push_back(b);
  for (uint8_t b : {0x00u, 0xFFu, 0x2Fu, 0x00u}) body.push_back(b);

  std::vector<uint8_t> smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0};
  for (char c : std::string("MTrk")) smf.push_back(static_cast<uint8_t>(c));
  const uint32_t len = static_cast<uint32_t>(body.size());
  for (int shift = 24; shift >= 0; shift -= 8) smf.push_back((len >> shift) & 0xFFu);
  smf.insert(smf.end(), body.begin(), body.end());

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_smf(project, smf.data(), smf.size(), &first_clip) == SONARE_OK);

  size_t count = 0;
  REQUIRE(sonare_project_marker_count(project, &count) == SONARE_OK);
  REQUIRE(count == 5);

  bool found_key = false, found_lyric = false, found_cue = false;
  for (size_t i = 0; i < count; ++i) {
    SonareProjectMarker m{};
    REQUIRE(sonare_project_marker_by_index(project, i, &m) == SONARE_OK);
    if (m.kind == SONARE_MARKER_KIND_KEY_SIGNATURE) {
      found_key = true;
      REQUIRE(m.key_fifths == 1);
      REQUIRE(m.key_minor == 1);
    } else if (m.kind == SONARE_MARKER_KIND_LYRIC) {
      found_lyric = true;
      REQUIRE(std::string(m.name) == "la");
    } else if (m.kind == SONARE_MARKER_KIND_CUE_POINT) {
      found_cue = true;
    }
  }
  REQUIRE(found_key);
  REQUIRE(found_lyric);
  REQUIRE(found_cue);

  // Out-of-range index fails cleanly without crashing.
  SonareProjectMarker oob{};
  REQUIRE(sonare_project_marker_by_index(project, count, &oob) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface set_marker_ex stores kind and key signature", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectMarker in{};
  in.id = 0;
  in.kind = SONARE_MARKER_KIND_KEY_SIGNATURE;
  in.key_fifths = -3;
  in.key_minor = 0;
  in.ppq = 4.0;
  const std::string key_name = "Eb major";
  std::copy(key_name.begin(), key_name.end(), in.name);
  in.name[key_name.size()] = '\0';

  uint32_t out_id = 0;
  REQUIRE(sonare_project_set_marker_ex(project, &in, &out_id) == SONARE_OK);
  REQUIRE(out_id != 0);

  SonareProjectMarker got{};
  REQUIRE(sonare_project_marker_by_index(project, 0, &got) == SONARE_OK);
  REQUIRE(got.id == out_id);
  REQUIRE(got.kind == SONARE_MARKER_KIND_KEY_SIGNATURE);
  REQUIRE(got.key_fifths == -3);
  REQUIRE(got.key_minor == 0);
  REQUIRE(got.ppq == 4.0);
  REQUIRE(std::string(got.name) == "Eb major");

  // Export to SMF and re-import: the key signature survives as a marker kind.
  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  REQUIRE(bytes != nullptr);
  std::vector<uint8_t> exported(bytes, bytes + len);
  sonare_free_bytes(bytes);

  SonareProject* round = nullptr;
  REQUIRE(sonare_project_create(&round) == SONARE_OK);
  REQUIRE(sonare_project_import_smf(round, exported.data(), exported.size(), nullptr) == SONARE_OK);
  SonareProjectMarker rmarker{};
  REQUIRE(sonare_project_marker_by_index(round, 0, &rmarker) == SONARE_OK);
  REQUIRE(rmarker.kind == SONARE_MARKER_KIND_KEY_SIGNATURE);
  REQUIRE(rmarker.key_fifths == -3);
  REQUIRE(rmarker.key_minor == 0);

  // A key signature with an out-of-range sf value (|fifths| > 7) is rejected so
  // it cannot serialize to a non-conformant SMF key signature.
  SonareProjectMarker bad = in;
  bad.id = 0;
  bad.key_fifths = 8;
  uint32_t bad_id = 0;
  REQUIRE(sonare_project_set_marker_ex(project, &bad, &bad_id) == SONARE_ERROR_INVALID_PARAMETER);
  bad.key_fifths = -8;
  REQUIRE(sonare_project_set_marker_ex(project, &bad, &bad_id) == SONARE_ERROR_INVALID_PARAMETER);
  // The same out-of-range fifths is accepted on a non-key-signature kind, where
  // the field is unused.
  bad.kind = SONARE_MARKER_KIND_MARKER;
  REQUIRE(sonare_project_set_marker_ex(project, &bad, &bad_id) == SONARE_OK);

  sonare_project_destroy(round);
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
  REQUIRE(sonare_project_bake_midi_fx(project, clip, config) == SONARE_OK);

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
  REQUIRE(sonare_project_bake_midi_fx(project, clip, "{bad json") == SONARE_ERROR_INVALID_FORMAT);

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
  REQUIRE(report.unmatched_note_offs == 0);

  // UMP group is part of note identity: same channel/note across groups must
  // not pair.
  SonareMidiEventPod cross_group[2]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 67, 100, &cross_group[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.0, 1, 0, 67, 0, &cross_group[1]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, cross_group, 2) == SONARE_OK);
  REQUIRE(sonare_project_validate_midi_notes(project, clip, &report) == SONARE_OK);
  REQUIRE(report.ok == 0);
  REQUIRE(report.unmatched_note_ons == 1);
  REQUIRE(report.unmatched_note_offs == 1);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface keeps an SMF note-off written on the end-of-track tick", "[project]") {
  // Single MIDI track at 480 PPQN: note-on @ tick 0, note-off @ tick 480, then
  // EndOfTrack at the SAME tick (delta 0). The closing note-off therefore lands
  // exactly on the end-of-track tick, which is also the imported clip length.
  // The compiler keeps clip events on the half-open window [0, length_ppq), so
  // without nudging the clip length just past the final event the note-off would
  // be discarded and the note left hanging when bounced through an instrument.
  const std::vector<uint8_t> smf = {'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00,
                                    0x00, 0x00, 0x01, 0x01, 0xE0, 'M',  'T',  'r',  'k',
                                    0x00, 0x00, 0x00, 0x0D, 0x00, 0x90, 0x3C, 0x40, 0x83,
                                    0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t first_clip = 0;
  REQUIRE(sonare_project_import_smf(project, smf.data(), smf.size(), &first_clip) == SONARE_OK);
  REQUIRE(first_clip != 0);

  // The clip length is nudged just past the boundary note-off (note-off ppq is
  // 1.0 quarter; length becomes the next representable double above it) so the
  // event survives compilation. A plain length == note-off ppq would drop it.
  const std::string json = serialize(project);
  REQUIRE(json.find("\"length_ppq\":1.0000000000000002") != std::string::npos);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline != 0);
  sonare_project_free_compile_result(&result);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface warns that a MIDI-only project bounces to silence", "[project]") {
  // A project with MIDI clips but no bound instrument compiles to a valid
  // timeline yet renders silence via the plain sonare_project_bounce path. The
  // compiler emits a best-effort warning (code 10 = kMidiClipNoInstrument) so a
  // caller is nudged toward bounce_with_builtin_instruments / a bound destination.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 60, 100, &events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(1.0, 0, 0, 60, 0, &events[1]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline != 0);  // warning is non-fatal

  bool warned = false;
  for (size_t i = 0; i < result.diagnostic_count; ++i) {
    if (result.diagnostics[i].code == 10u) {
      warned = true;
      REQUIRE(result.diagnostics[i].severity == 1u);  // kWarning
    }
  }
  REQUIRE(warned);
  REQUIRE(result.messages != nullptr);
  const std::string messages(result.messages);
  REQUIRE(messages.find("project contains MIDI clips") != std::string::npos);
  sonare_project_free_compile_result(&result);

  SonareProjectBounceOptions options{};
  options.sample_rate = 48000;
  options.block_size = 128;
  options.num_channels = 2;
  options.total_frames = 256;
  float* bounced = nullptr;
  size_t bounced_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &bounced, &bounced_len) == SONARE_OK);
  REQUIRE(bounced != nullptr);
  sonare_free_floats(bounced);

  SonareProjectCompileResult bounce_result{};
  REQUIRE(sonare_project_last_bounce_compile_result(project, &bounce_result) == SONARE_OK);
  bool bounce_warned = false;
  for (size_t i = 0; i < bounce_result.diagnostic_count; ++i) {
    if (bounce_result.diagnostics[i].code == 10u) {
      bounce_warned = true;
      REQUIRE(bounce_result.diagnostics[i].severity == 1u);
    }
  }
  REQUIRE(bounce_warned);
  REQUIRE(bounce_result.messages != nullptr);
  const std::string bounce_messages(bounce_result.messages);
  REQUIRE(bounce_messages.find("project contains MIDI clips") != std::string::npos);
  sonare_project_free_compile_result(&bounce_result);

  sonare_project_destroy(project);
}
