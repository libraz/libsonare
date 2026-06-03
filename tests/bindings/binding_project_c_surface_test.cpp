/// @file binding_project_c_surface_test.cpp
/// @brief Project C surface parity tests.

#include "binding_project_parity_test_helpers.h"

TEST_CASE("project ABI version is positive and matches the macro", "[project]") {
  REQUIRE(sonare_project_abi_version() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(sonare_project_abi_version() > 0u);
}

TEST_CASE("project C surface stores and retrieves AssistSidecar", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  const uint8_t payload[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
  REQUIRE(sonare_project_set_assist_sidecar(project, "midi-sketch", 3, 42, 10.0, 20.0, payload,
                                            sizeof(payload)) == SONARE_OK);
  REQUIRE(sonare_project_assist_sidecar_count(project) == 1);

  SonareProjectAssistSidecar sidecar{};
  REQUIRE(sonare_project_get_assist_sidecar(project, 0, &sidecar) == SONARE_OK);
  REQUIRE(sidecar.module_id != nullptr);
  CHECK(std::string(sidecar.module_id) == "midi-sketch");
  CHECK(sidecar.schema_version == 3);
  CHECK(sidecar.target_track_id == 42);
  CHECK(sidecar.region_start_ppq == 10.0);
  CHECK(sidecar.region_end_ppq == 20.0);
  REQUIRE(sidecar.payload != nullptr);
  REQUIRE(sidecar.payload_len == sizeof(payload));
  CHECK(std::memcmp(sidecar.payload, payload, sizeof(payload)) == 0);
  sonare_project_free_assist_sidecar(&sidecar);
  CHECK(sidecar.module_id == nullptr);
  CHECK(sidecar.payload == nullptr);

  const uint8_t replacement[] = {0xAA, 0xBB};
  REQUIRE(sonare_project_set_assist_sidecar(project, "midi-sketch", 4, 42, 10.0, 20.0, replacement,
                                            sizeof(replacement)) == SONARE_OK);
  REQUIRE(sonare_project_assist_sidecar_count(project) == 1);
  REQUIRE(sonare_project_get_assist_sidecar(project, 0, &sidecar) == SONARE_OK);
  CHECK(sidecar.schema_version == 4);
  REQUIRE(sidecar.payload_len == sizeof(replacement));
  CHECK(std::memcmp(sidecar.payload, replacement, sizeof(replacement)) == 0);
  sonare_project_free_assist_sidecar(&sidecar);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_get_assist_sidecar(project, 0, &sidecar) == SONARE_OK);
  CHECK(sidecar.schema_version == 3);
  REQUIRE(sidecar.payload_len == sizeof(payload));
  CHECK(std::memcmp(sidecar.payload, payload, sizeof(payload)) == 0);
  sonare_project_free_assist_sidecar(&sidecar);

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  const std::string json = serialize(project);
  SonareProject* restored = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &restored, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_assist_sidecar_count(restored) == 1);
  REQUIRE(sonare_project_get_assist_sidecar(restored, 0, &sidecar) == SONARE_OK);
  CHECK(sidecar.schema_version == 4);
  REQUIRE(sidecar.payload_len == sizeof(replacement));
  CHECK(std::memcmp(sidecar.payload, replacement, sizeof(replacement)) == 0);
  sonare_project_free_assist_sidecar(&sidecar);

  CHECK(sonare_project_get_assist_sidecar(restored, 1, &sidecar) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_assist_sidecar(project, "", 1, 0, 0.0, 0.0, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_assist_sidecar(project, "bad", 1, 0, 0.0, 0.0, nullptr, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(restored);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface authors MIR key and chord annotations", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectKeySegment keys[2]{};
  keys[0].start_ppq = 0.0;
  keys[0].end_ppq = 4.0;
  keys[0].tonic_pc = 0;
  keys[0].mode = 1;  // major
  keys[1].start_ppq = 4.0;
  keys[1].end_ppq = 8.0;
  keys[1].tonic_pc = 9;
  keys[1].mode = 2;  // minor
  REQUIRE(sonare_project_annotate_keys(project, keys, 2) == SONARE_OK);

  const uint8_t dom_ext[] = {7, 9};
  SonareProjectChordSymbol chords[2]{};
  chords[0].start_ppq = 0.0;
  chords[0].end_ppq = 4.0;
  chords[0].root_pc = 0;
  chords[0].quality = 1;  // major
  chords[0].slash_bass_pc = 255;
  chords[0].roman_numeral = "I";
  chords[1].start_ppq = 4.0;
  chords[1].end_ppq = 8.0;
  chords[1].root_pc = 7;
  chords[1].quality = 5;  // dominant
  chords[1].extensions = dom_ext;
  chords[1].extension_count = sizeof(dom_ext);
  chords[1].slash_bass_pc = 11;
  chords[1].roman_numeral = "V9/iii";
  chords[1].modulation_boundary = 1;
  REQUIRE(sonare_project_annotate_chords(project, chords, 2) == SONARE_OK);

  const std::string json = serialize(project);
  REQUIRE(json.find("\"keys\":[") != std::string::npos);
  REQUIRE(json.find("\"start_ppq\":0") != std::string::npos);
  REQUIRE(json.find("\"tonic_pc\":9") != std::string::npos);
  REQUIRE(json.find("\"roman_numeral\":\"V9/iii\"") != std::string::npos);
  REQUIRE(json.find("\"extensions\":[7,9]") != std::string::npos);
  REQUIRE(json.find("\"slash_bass_pc\":11") != std::string::npos);
  REQUIRE(json.find("\"modulation_boundary\":true") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  const std::string after_undo = serialize(project);
  REQUIRE(after_undo.find("\"chords\":[]") != std::string::npos);
  REQUIRE(after_undo.find("\"keys\":[") != std::string::npos);
  REQUIRE(after_undo.find("\"tonic_pc\":9") != std::string::npos);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == json);

  SonareProjectKeySegment bad_key = keys[0];
  bad_key.end_ppq = bad_key.start_ppq;
  CHECK(sonare_project_annotate_keys(project, &bad_key, 1) == SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectChordSymbol bad_chord = chords[0];
  bad_chord.root_pc = 12;
  CHECK(sonare_project_annotate_chords(project, &bad_chord, 1) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface sets a clip warp reference", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.length_ppq = 4.0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);

  REQUIRE(sonare_project_set_clip_warp_ref(project, clip_id, 123) == SONARE_OK);
  const std::string warped = serialize(project);
  REQUIRE(warped.find("\"warp_ref_id\":123") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_ref_id\":0") != std::string::npos);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == warped);

  REQUIRE(sonare_project_set_clip_warp_ref(project, clip_id, 0) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_ref_id\":0") != std::string::npos);
  CHECK(sonare_project_set_clip_warp_ref(project, 999999u, 1) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
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
