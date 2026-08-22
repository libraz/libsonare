/// @file binding_project_c_surface_test.cpp
/// @brief Project C surface parity tests.

#include <algorithm>

#include "binding_project_parity_test_helpers.h"

TEST_CASE("project ABI version is positive and matches the macro", "[project]") {
  REQUIRE(sonare_project_abi_version() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(sonare_project_abi_version() > 0u);
}

TEST_CASE("a successful project load hands back the warnings it collected", "[project]") {
  // out_diag is owned by the caller on EVERY return code, not only on failure.
  // A document that loads with warnings answers SONARE_OK with a non-NULL
  // string, so a C embedder that frees it only on failure leaks one per warned
  // load, and one that reads "non-NULL means failure" reports a good load as a
  // bad one.
  const std::string warned = R"({"version":1,"midi_content":{"not-a-clip-id":[]}})";
  SonareProject* project = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(warned.data(), warned.size(), &project, &diagnostics) ==
          SONARE_OK);
  REQUIRE(project != nullptr);
  REQUIRE(diagnostics != nullptr);
  CHECK(std::string(diagnostics).find("invalid_midi_content_key") != std::string::npos);
  sonare_free_string(diagnostics);
  sonare_project_destroy(project);

  // A clean document leaves it NULL, so "loaded with warnings" stays
  // distinguishable from "loaded clean".
  const std::string clean = R"({"version":1})";
  project = nullptr;
  diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(clean.data(), clean.size(), &project, &diagnostics) ==
          SONARE_OK);
  REQUIRE(project != nullptr);
  CHECK(diagnostics == nullptr);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface exposes read-only project state without JSON", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  double sample_rate = 0.0;
  REQUIRE(sonare_project_get_sample_rate(project, &sample_rate) == SONARE_OK);
  CHECK(sample_rate == 48000.0);
  REQUIRE(sonare_project_set_sample_rate(project, 44100.0) == SONARE_OK);
  REQUIRE(sonare_project_get_sample_rate(project, &sample_rate) == SONARE_OK);
  CHECK(sample_rate == 44100.0);

  uint32_t overlap_policy = 99;
  REQUIRE(sonare_project_get_overlap_policy(project, &overlap_policy) == SONARE_OK);
  CHECK(overlap_policy == 0u);
  REQUIRE(sonare_project_set_overlap_policy(project, SONARE_PROJECT_OVERLAP_ALLOW) == SONARE_OK);
  REQUIRE(sonare_project_get_overlap_policy(project, &overlap_policy) == SONARE_OK);
  CHECK(overlap_policy == SONARE_PROJECT_OVERLAP_ALLOW);

  size_t count = 99;
  REQUIRE(sonare_project_track_count(project, &count) == SONARE_OK);
  CHECK(count == 0);
  REQUIRE(sonare_project_clip_count(project, &count) == SONARE_OK);
  CHECK(count == 0);
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  CHECK(count == 0);
  REQUIRE(sonare_project_marker_count(project, &count) == SONARE_OK);
  CHECK(count == 0);
  REQUIRE(sonare_project_tempo_segment_count(project, &count) == SONARE_OK);
  CHECK(count == 0);
  REQUIRE(sonare_project_time_signature_count(project, &count) == SONARE_OK);
  CHECK(count == 0);

  SonareProjectTempoSegment tempos[2]{};
  tempos[0].start_ppq = 0.0;
  tempos[0].bpm = 120.0;
  tempos[1].start_ppq = 960.0;
  tempos[1].bpm = 132.0;
  tempos[1].start_sample = -123.0;  // input is ignored; start samples are derived.
  tempos[1].end_bpm = 144.0;
  REQUIRE(sonare_project_set_tempo_segments(project, tempos, 2) == SONARE_OK);
  REQUIRE(sonare_project_tempo_segment_count(project, &count) == SONARE_OK);
  CHECK(count == 2);

  // The list is readable back, which is what makes the count usable for
  // anything: an index alone tells a caller nothing without the segment.
  SonareProjectTempoSegment read_tempo{};
  REQUIRE(sonare_project_tempo_segment_by_index(project, 0, &read_tempo) == SONARE_OK);
  CHECK(read_tempo.start_ppq == tempos[0].start_ppq);
  CHECK(read_tempo.bpm == tempos[0].bpm);
  CHECK(read_tempo.end_bpm == tempos[0].end_bpm);
  REQUIRE(sonare_project_tempo_segment_by_index(project, 1, &read_tempo) == SONARE_OK);
  CHECK(read_tempo.start_ppq == tempos[1].start_ppq);
  CHECK(read_tempo.bpm == tempos[1].bpm);
  CHECK(read_tempo.end_bpm == tempos[1].end_bpm);
  // The setter ignored the -123 it was handed, and the getter reports what the
  // project holds rather than echoing the input back.
  CHECK(read_tempo.start_sample == 0.0);
  CHECK(sonare_project_tempo_segment_by_index(project, 2, &read_tempo) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_tempo_segment_by_index(project, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_tempo_segment_by_index(nullptr, 0, &read_tempo) ==
        SONARE_ERROR_INVALID_PARAMETER);

  SonareProjectTimeSignatureSegment sig{};
  sig.start_ppq = 0.0;
  sig.numerator = 7;
  sig.denominator = 8;
  REQUIRE(sonare_project_set_time_signatures(project, &sig, 1) == SONARE_OK);
  REQUIRE(sonare_project_time_signature_count(project, &count) == SONARE_OK);
  CHECK(count == 1);

  SonareProjectTimeSignatureSegment read_sig{};
  REQUIRE(sonare_project_time_signature_by_index(project, 0, &read_sig) == SONARE_OK);
  CHECK(read_sig.start_ppq == sig.start_ppq);
  CHECK(read_sig.numerator == sig.numerator);
  CHECK(read_sig.denominator == sig.denominator);
  CHECK(sonare_project_time_signature_by_index(project, 1, &read_sig) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_time_signature_by_index(project, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  uint32_t marker_id = 0;
  REQUIRE(sonare_project_set_marker(project, 0, 12.0, "Verse", &marker_id) == SONARE_OK);
  REQUIRE(marker_id != 0);
  REQUIRE(sonare_project_marker_count(project, &count) == SONARE_OK);
  CHECK(count == 1);
  uint32_t replaced_id = 0;
  REQUIRE(sonare_project_set_marker(project, marker_id, 16.0, "Chorus", &replaced_id) == SONARE_OK);
  CHECK(replaced_id == marker_id);
  REQUIRE(sonare_project_marker_count(project, &count) == SONARE_OK);
  CHECK(count == 1);

  const char* scene_json =
      "{\"buses\":[{\"id\":\"master\",\"role\":\"master\"}],\"connections\":[],\"strips\":[{"
      "\"id\":\"lead\",\"faderDb\":-3.0}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);
  CHECK(sonare_project_set_mixer_scene_json(project, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_id, &clip_id) == SONARE_OK);
  REQUIRE(sonare_project_track_count(project, &count) == SONARE_OK);
  CHECK(count == 1);
  REQUIRE(sonare_project_clip_count(project, &count) == SONARE_OK);
  CHECK(count == 1);
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  CHECK(count == 1);

  CHECK(sonare_project_get_sample_rate(project, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_track_count(project, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_clip_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_overlap_policy(project, 99) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_tempo_segments(project, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  tempos[0].bpm = 0.0;
  CHECK(sonare_project_set_tempo_segments(project, tempos, 1) == SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTempoSegment non_monotonic[] = {{8.0, 120.0, 0.0, 0.0},
                                                     {4.0, 120.0, 0.0, 0.0}};
  CHECK(sonare_project_set_tempo_segments(project, non_monotonic, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_time_signatures(project, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  sig.denominator = 0;
  CHECK(sonare_project_set_time_signatures(project, &sig, 1) == SONARE_ERROR_INVALID_PARAMETER);
  // The project setters admit exactly what their engine counterparts do: a
  // tempo or a position past the public bound is rejected here too, because the
  // project's own MIR and loop helpers build a raw tempo map from what is
  // stored and would otherwise compute clamped, meaningless sample positions.
  const SonareProjectTempoSegment over_range_bpm[] = {{0.0, 1.0e9, 0.0, 0.0}};
  CHECK(sonare_project_set_tempo_segments(project, over_range_bpm, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTempoSegment over_range_ppq[] = {{1.0e30, 120.0, 0.0, 0.0}};
  CHECK(sonare_project_set_tempo_segments(project, over_range_ppq, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  // ... and the time-signature setter rejects the same malformed shapes its
  // sibling does, rather than storing an order that reads back verbatim.
  const SonareProjectTimeSignatureSegment unordered[] = {{8.0, 4, 4}, {4.0, 3, 4}};
  CHECK(sonare_project_set_time_signatures(project, unordered, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTimeSignatureSegment duplicate_start[] = {{4.0, 4, 4}, {4.0, 3, 4}};
  CHECK(sonare_project_set_time_signatures(project, duplicate_start, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTimeSignatureSegment over_range_sig[] = {{1.0e30, 4, 4}};
  CHECK(sonare_project_set_time_signatures(project, over_range_sig, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_marker(project, 0, -1.0, "bad", &marker_id) ==
        SONARE_ERROR_INVALID_PARAMETER);

  const std::string json = serialize(project);
  CHECK(json.find("\"overlap_policy\":1") != std::string::npos);
  CHECK(json.find("\"bpm\":132") != std::string::npos);
  CHECK(json.find("\"denominator\":8") != std::string::npos);
  CHECK(json.find("\"name\":\"Chorus\"") != std::string::npos);
  CHECK(json.find("\"id\":\"lead\"") != std::string::npos);
  CHECK(json.find("\"gain\":1") != std::string::npos);

  sonare_project_destroy(project);
}

TEST_CASE("marker setters reject the reserved entity id", "[project]") {
  // UINT32_MAX is reserved for every entity id, and the deserializer drops a
  // whole document over one. A marker setter that accepted it would build a
  // project that serializes but can never be loaded back -- unrecoverable
  // except by hand-editing the file.
  constexpr uint32_t kReserved = std::numeric_limits<uint32_t>::max();

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  uint32_t out_id = 99;
  CHECK(sonare_project_set_marker(project, kReserved, 1.0, "reserved", &out_id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_id == 0u);

  const std::string marker_name = "reserved";
  SonareProjectMarker marker{};
  marker.id = kReserved;
  marker.ppq = 1.0;
  std::copy(marker_name.begin(), marker_name.end(), marker.name);
  out_id = 99;
  CHECK(sonare_project_set_marker_ex(project, &marker, &out_id) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_id == 0u);
  out_id = 99;
  CHECK(sonare_project_set_marker_ex_name(project, &marker, "reserved", &out_id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_id == 0u);

  size_t count = 99;
  REQUIRE(sonare_project_marker_count(project, &count) == SONARE_OK);
  CHECK(count == 0u);

  // Every id the setters do accept survives a serialize / deserialize round
  // trip, including the largest usable one. Allocate before pinning the id
  // counter to its top: an explicit id of UINT32_MAX-1 exhausts the allocator.
  REQUIRE(sonare_project_set_marker(project, 0, 2.0, "allocated", &out_id) == SONARE_OK);
  CHECK(out_id != 0u);
  CHECK(out_id != kReserved);
  marker.id = kReserved - 1u;
  out_id = 0;
  REQUIRE(sonare_project_set_marker_ex(project, &marker, &out_id) == SONARE_OK);
  CHECK(out_id == kReserved - 1u);

  const std::string json = serialize(project);
  SonareProject* reloaded = nullptr;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(json.c_str(), json.size(), &reloaded, &diagnostics) ==
          SONARE_OK);
  REQUIRE(reloaded != nullptr);
  sonare_free_string(diagnostics);
  REQUIRE(sonare_project_marker_count(reloaded, &count) == SONARE_OK);
  CHECK(count == 2u);

  sonare_project_destroy(reloaded);
  sonare_project_destroy(project);
}

TEST_CASE("NativeSynth enum names are supplied by the C project ABI", "[project][synth]") {
  const auto split = [](const char* joined) {
    std::vector<std::string> out;
    REQUIRE(joined != nullptr);
    std::string names(joined);
    size_t start = 0;
    while (start <= names.size()) {
      const size_t end = names.find('\n', start);
      if (end == std::string::npos) {
        if (start < names.size()) out.push_back(names.substr(start));
        break;
      }
      out.push_back(names.substr(start, end - start));
      start = end + 1;
    }
    return out;
  };

  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_ENGINE_MODE)) ==
        std::vector<std::string>{"default", "subtractive", "fm", "karplus-strong", "modal",
                                 "additive", "percussion", "piano", "pipe-organ", "bowed-string",
                                 "reed", "brass", "flute", "plucked-string", "vocal", "free-reed",
                                 "harpsichord"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_OSC_WAVEFORM)) ==
        std::vector<std::string>{"default", "sine", "saw", "square", "triangle", "noise"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM)) ==
        std::vector<std::string>{"sine", "saw", "sawtooth", "square", "triangle"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_MODEL)) ==
        std::vector<std::string>{"default", "svf", "moog-ladder", "diode-ladder", "sallen-key"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_FILTER_OUTPUT)) ==
        std::vector<std::string>{"default", "lowpass", "bandpass", "highpass"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_BODY_TYPE)) ==
        std::vector<std::string>{"default", "none", "guitar", "violin", "wood-tube", "brass-bell",
                                 "vocal"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_SOURCE)) ==
        std::vector<std::string>{"none", "amp-env", "filter-env", "lfo1", "lfo2", "velocity",
                                 "key-track", "mod-wheel", "random"});
  CHECK(split(sonare_synth_enum_names(SONARE_SYNTH_ENUM_MOD_DESTINATION)) ==
        std::vector<std::string>{"none", "pitch-cents", "cutoff-cents", "amp-gain", "pan-units"});
  CHECK(sonare_synth_builtin_waveform_from_name("sine") == SONARE_SYNTH_WAVEFORM_SINE);
  CHECK(sonare_synth_builtin_waveform_from_name("saw") == SONARE_SYNTH_WAVEFORM_SAW);
  CHECK(sonare_synth_builtin_waveform_from_name("sawtooth") == SONARE_SYNTH_WAVEFORM_SAW);
  CHECK(sonare_synth_builtin_waveform_from_name("square") == SONARE_SYNTH_WAVEFORM_SQUARE);
  CHECK(sonare_synth_builtin_waveform_from_name("triangle") == SONARE_SYNTH_WAVEFORM_TRIANGLE);
  CHECK(sonare_synth_builtin_waveform_from_name("noise") == -1);
  CHECK(sonare_synth_builtin_waveform_from_name(nullptr) == -1);
  CHECK(std::string(sonare_synth_enum_names(999)) == "");
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

  // A warp ref must name a registered warp map; an unregistered id is a caller
  // mistake and is rejected before any project state is touched.
  CHECK(sonare_project_set_clip_warp_ref(project, clip_id, 123) == SONARE_ERROR_INVALID_PARAMETER);

  SonareProjectWarpAnchor anchors[] = {{0.0, 0.0}, {48000.0, 44100.0}};
  SonareProjectWarpMapDesc map{};
  map.id = 123;
  map.anchors = anchors;
  map.anchor_count = 2;
  REQUIRE(sonare_project_set_warp_map(project, &map) == SONARE_OK);

  REQUIRE(sonare_project_set_clip_warp_ref(project, clip_id, 123) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_warp_mode(project, clip_id, SONARE_PROJECT_WARP_MODE_REPITCH) ==
          SONARE_OK);
  const std::string warped = serialize(project);
  REQUIRE(warped.find("\"warp_ref_id\":123") != std::string::npos);
  REQUIRE(warped.find("\"warp_mode\":1") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_mode\":0") != std::string::npos);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_ref_id\":0") != std::string::npos);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == warped);

  REQUIRE(sonare_project_set_clip_warp_ref(project, clip_id, 0) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_ref_id\":0") != std::string::npos);
  REQUIRE(sonare_project_set_clip_warp_mode(project, clip_id,
                                            SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_mode\":2") != std::string::npos);
  CHECK(sonare_project_set_clip_warp_ref(project, 999999u, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_clip_warp_mode(project, 999999u, SONARE_PROJECT_WARP_MODE_REPITCH) ==
        SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface reads stored tracks clips and sources by index", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "Readback";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.start_ppq = 2.0;
  clip_desc.length_ppq = 4.0;
  clip_desc.source_uri = "memory://readback.wav";
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);

  SonareProjectTrack track{};
  REQUIRE(sonare_project_track_by_index(project, 0, &track) == SONARE_OK);
  CHECK(track.id == track_id);
  CHECK(std::string(track.name) == "Readback");
  SonareProjectClip clip{};
  REQUIRE(sonare_project_clip_by_index(project, 0, &clip) == SONARE_OK);
  CHECK(clip.id == clip_id);
  CHECK(clip.track_id == track_id);
  CHECK(clip.start_ppq == Catch::Approx(2.0));
  SonareProjectSource source{};
  REQUIRE(sonare_project_source_by_index(project, 0, &source) == SONARE_OK);
  CHECK(source.id == clip.source_id);
  CHECK(source.kind == 0);
  CHECK(std::string(source.name_or_uri) == "memory://readback.wav");
  CHECK(sonare_project_clip_by_index(project, 1, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_project_destroy(project);
}

TEST_CASE("project C surface owns and edits audio source metadata", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc audio_track_desc{};
  audio_track_desc.kind = SONARE_TRACK_AUDIO;
  uint32_t audio_track_id = 0;
  REQUIRE(sonare_project_add_track(project, &audio_track_desc, &audio_track_id) == SONARE_OK);

  const std::vector<float> samples = {0.1f, -0.1f, 0.2f, -0.2f};
  SonareProjectClipDesc audio_clip_desc{};
  audio_clip_desc.track_id = audio_track_id;
  audio_clip_desc.length_ppq = 4.0;
  audio_clip_desc.audio_interleaved = samples.data();
  audio_clip_desc.audio_frames = 2;
  audio_clip_desc.audio_channels = 2;
  audio_clip_desc.audio_sample_rate = 44100;
  audio_clip_desc.source_uri = "asset://metadata.wav";
  uint32_t audio_clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &audio_clip_desc, &audio_clip_id) == SONARE_OK);

  SonareProjectClip audio_clip{};
  REQUIRE(sonare_project_clip_by_index(project, 0, &audio_clip) == SONARE_OK);
  REQUIRE(audio_clip.source_id != 0);

  SonareProjectAudioSourceMetadata metadata{
      reinterpret_cast<char*>(static_cast<uintptr_t>(1)),
      reinterpret_cast<char*>(static_cast<uintptr_t>(2)),
  };
  CHECK(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(metadata.content_hash != nullptr);
  CHECK(sonare_project_get_audio_source_metadata(nullptr, audio_clip.source_id, &metadata) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(metadata.content_hash == nullptr);
  CHECK(metadata.external_stem_role == nullptr);
  CHECK(sonare_project_get_audio_source_metadata(project, 999999u, &metadata) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(metadata.content_hash == nullptr);
  CHECK(metadata.external_stem_role == nullptr);
  sonare_project_free_audio_source_metadata(nullptr);
  sonare_project_free_audio_source_metadata(&metadata);

  // A successful read owns both strings, including empty strings.
  REQUIRE(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  REQUIRE(metadata.content_hash != nullptr);
  REQUIRE(metadata.external_stem_role != nullptr);
  CHECK(std::string(metadata.content_hash).empty());
  CHECK(std::string(metadata.external_stem_role).empty());
  sonare_project_free_audio_source_metadata(&metadata);
  CHECK(metadata.content_hash == nullptr);
  CHECK(metadata.external_stem_role == nullptr);

  CHECK(sonare_project_set_audio_source_metadata(project, audio_clip.source_id, nullptr, "lead") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_audio_source_metadata(project, audio_clip.source_id, "sha256:bad",
                                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_set_audio_source_metadata(project, 999999u, "sha256:bad", "lead") ==
        SONARE_ERROR_INVALID_PARAMETER);

  SonareProjectTrackDesc midi_track_desc{};
  midi_track_desc.kind = SONARE_TRACK_MIDI;
  uint32_t midi_track_id = 0;
  REQUIRE(sonare_project_add_track(project, &midi_track_desc, &midi_track_id) == SONARE_OK);
  SonareProjectClipDesc midi_clip_desc{};
  midi_clip_desc.track_id = midi_track_id;
  midi_clip_desc.is_midi = 1;
  midi_clip_desc.length_ppq = 4.0;
  uint32_t midi_clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &midi_clip_desc, &midi_clip_id) == SONARE_OK);
  SonareProjectClip midi_clip{};
  REQUIRE(sonare_project_clip_by_index(project, 1, &midi_clip) == SONARE_OK);
  CHECK(sonare_project_get_audio_source_metadata(project, midi_clip.source_id, &metadata) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(metadata.content_hash == nullptr);
  CHECK(metadata.external_stem_role == nullptr);
  CHECK(sonare_project_set_audio_source_metadata(project, midi_clip.source_id, "sha256:bad",
                                                 "lead") == SONARE_ERROR_INVALID_PARAMETER);

  SonareProjectSource before{};
  REQUIRE(sonare_project_source_by_index(project, 0, &before) == SONARE_OK);
  REQUIRE(sonare_project_set_audio_source_metadata(project, audio_clip.source_id, "sha256:deadbeef",
                                                   "lead") == SONARE_OK);
  SonareProjectSource after{};
  REQUIRE(sonare_project_source_by_index(project, 0, &after) == SONARE_OK);
  CHECK(after.id == before.id);
  CHECK(after.kind == before.kind);
  CHECK(after.channel_count == before.channel_count);
  CHECK(after.storage_handle_id == before.storage_handle_id);
  CHECK(after.sample_rate_hint == before.sample_rate_hint);
  CHECK(std::string(after.name_or_uri) == std::string(before.name_or_uri));

  REQUIRE(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  REQUIRE(metadata.content_hash != nullptr);
  REQUIRE(metadata.external_stem_role != nullptr);
  CHECK(std::string(metadata.content_hash) == "sha256:deadbeef");
  CHECK(std::string(metadata.external_stem_role) == "lead");
  sonare_project_free_audio_source_metadata(&metadata);

  // Both fields are one history command: one undo restores both, and one redo
  // reapplies both. Empty strings clear the fields rather than becoming NULL.
  REQUIRE(sonare_project_set_audio_source_metadata(project, audio_clip.source_id, "", "") ==
          SONARE_OK);
  REQUIRE(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  CHECK(std::string(metadata.content_hash).empty());
  CHECK(std::string(metadata.external_stem_role).empty());
  sonare_project_free_audio_source_metadata(&metadata);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  CHECK(std::string(metadata.content_hash) == "sha256:deadbeef");
  CHECK(std::string(metadata.external_stem_role) == "lead");
  sonare_project_free_audio_source_metadata(&metadata);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_get_audio_source_metadata(project, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  CHECK(std::string(metadata.content_hash).empty());
  CHECK(std::string(metadata.external_stem_role).empty());
  sonare_project_free_audio_source_metadata(&metadata);

  REQUIRE(sonare_project_set_audio_source_metadata(project, audio_clip.source_id,
                                                   "sha256:roundtrip", "vocals") == SONARE_OK);
  const std::string json = serialize(project);
  SonareProject* restored = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &restored, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_get_audio_source_metadata(restored, audio_clip.source_id, &metadata) ==
          SONARE_OK);
  CHECK(std::string(metadata.content_hash) == "sha256:roundtrip");
  CHECK(std::string(metadata.external_stem_role) == "vocals");
  sonare_project_free_audio_source_metadata(&metadata);

  sonare_project_destroy(restored);
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

TEST_CASE("deserialized audio sources can be rebound before bouncing through the C surface",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.source_uri = "asset://lead.wav";
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);

  const std::string json = serialize(project);
  sonare_project_destroy(project);

  SonareProject* restored = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &restored, nullptr) == SONARE_OK);

  size_t unresolved = 0;
  REQUIRE(sonare_project_unresolved_audio_source_count(restored, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 1);
  uint32_t source_id = 0;
  REQUIRE(sonare_project_unresolved_audio_source_id_by_index(restored, 0, &source_id) == SONARE_OK);

  constexpr int kFrames = 480;
  const std::vector<float> samples(kFrames, 0.25f);
  REQUIRE(sonare_project_set_source_audio(restored, source_id, samples.data(), kFrames, 1, 48000) ==
          SONARE_OK);
  REQUIRE(sonare_project_unresolved_audio_source_count(restored, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 0);

  SonareProjectBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = 64;
  options.num_channels = 1;
  options.sample_rate = 48000;
  float* output = nullptr;
  size_t output_len = 0;
  REQUIRE(sonare_project_bounce(restored, &options, &output, &output_len) == SONARE_OK);
  REQUIRE(output != nullptr);
  REQUIRE(output_len == kFrames);
  CHECK(output[0] == Catch::Approx(0.25f));
  sonare_free_floats(output);
  sonare_project_destroy(restored);
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
