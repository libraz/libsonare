/// @file binding_project_editing_test.cpp
/// @brief Project editing and compile parity tests.

#include "binding_project_parity_test_helpers.h"
#include "util/resource_limits.h"

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
  sonare_free_project_compile_result(&result);
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

TEST_CASE("project C deserialize rejects oversized JSON before copying caller bytes", "[project]") {
  const char prefix = '{';
  SonareProject* project = reinterpret_cast<SonareProject*>(0x1);
  char* diag = reinterpret_cast<char*>(0x1);
  const size_t oversized =
      sonare::resource::kDefaultProjectImportResourceLimits.max_json_bytes + 1u;
  REQUIRE(sonare_project_deserialize(&prefix, oversized, &project, &diag) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(project == nullptr);
  REQUIRE(diag == nullptr);
}

TEST_CASE("snap_to_grid snaps a near-beat coordinate to the beat line", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  double snapped = -1.0;
  REQUIRE(sonare_project_snap_to_grid(project, 1.02, 1.0, &snapped) == SONARE_OK);
  REQUIRE(snapped == 1.0);

  REQUIRE(sonare_project_snap_to_grid_ex(project, 0.27, 1.0, 4, &snapped) == SONARE_OK);
  REQUIRE(snapped == 0.25);
  REQUIRE(sonare_project_snap_to_grid_ex(project, 4.02, 1.0, 0, &snapped) == SONARE_OK);
  REQUIRE(snapped == 4.0);
  REQUIRE(sonare_project_snap_to_grid_ex(project, 1.0, 1.0, -1, &snapped) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("project tempo analysis exposes ranked candidates and applies detected meter",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  constexpr int kSampleRate = 48000;
  std::vector<float> audio(kSampleRate * 2, 0.0f);
  for (size_t beat = 0; beat < audio.size(); beat += static_cast<size_t>(kSampleRate / 2)) {
    audio[beat] = 1.0f;
  }

  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  REQUIRE(sonare_project_analyze_tempo(project, audio.data(), audio.size(), kSampleRate, candidates,
                                       std::size(candidates), &count) == SONARE_OK);
  REQUIRE(count > 0);
  REQUIRE(candidates[0].bpm > 0.0f);
  REQUIRE(candidates[0].confidence >= 0.0f);
  REQUIRE(candidates[0].first_time_signature.numerator > 0);
  REQUIRE(candidates[0].first_time_signature.denominator > 0);

  float applied_bpm = 0.0f;
  REQUIRE(sonare_project_auto_tempo_ex(project, audio.data(), audio.size(), kSampleRate, 0, 1,
                                       &applied_bpm) == SONARE_OK);
  REQUIRE(applied_bpm == candidates[0].bpm);
  sonare_project_destroy(project);
}

TEST_CASE("project tempo options reach the bridge and default to the fixed-signature calls",
          "[project]") {
  // A tempo that moves across the take. The fixed-signature entry points fit one
  // tempo to the whole thing and emit a map that says so; the options are how a
  // caller asks the tracker to follow it.
  constexpr int kSampleRate = 22050;
  constexpr double kStartBpm = 100.0;
  constexpr double kEndBpm = 160.0;
  constexpr double kSeconds = 24.0;
  std::vector<float> audio(static_cast<size_t>(kSampleRate * kSeconds), 0.0f);
  for (double t = 0.0; t < kSeconds;) {
    const auto at = static_cast<size_t>(t * kSampleRate);
    for (int i = 0; i < 64 && at + static_cast<size_t>(i) < audio.size(); ++i) {
      audio[at + static_cast<size_t>(i)] =
          (i % 2 == 0 ? 1.0f : -1.0f) * std::exp(-static_cast<float>(i) / 12.0f);
    }
    t += 60.0 / (kStartBpm + (kEndBpm - kStartBpm) * (t / kSeconds));
  }

  SonareProjectTempoOptions defaults;
  sonare_project_tempo_options_init(&defaults);
  CHECK(defaults.struct_size == sizeof(SonareProjectTempoOptions));
  CHECK(defaults.adaptive_tempo == 0);
  CHECK(defaults.tempo_update_interval_beats > 0);
  CHECK(defaults.ramp_threshold > 0.0f);

  const auto segment_count = [&audio](const SonareProjectTempoOptions* options) {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    float bpm = 0.0f;
    REQUIRE(sonare_project_auto_tempo_with_options(project, audio.data(), audio.size(), kSampleRate,
                                                   options, 0, 0, &bpm) == SONARE_OK);
    size_t count = 0;
    REQUIRE(sonare_project_tempo_segment_count(project, &count) == SONARE_OK);
    sonare_project_destroy(project);
    return count;
  };

  // Passing the defaults, and passing nothing, must both reproduce the
  // fixed-signature behaviour rather than quietly enabling anything.
  SonareProject* legacy = nullptr;
  REQUIRE(sonare_project_create(&legacy) == SONARE_OK);
  float legacy_bpm = 0.0f;
  REQUIRE(sonare_project_auto_tempo_ex(legacy, audio.data(), audio.size(), kSampleRate, 0, 0,
                                       &legacy_bpm) == SONARE_OK);
  size_t legacy_count = 0;
  REQUIRE(sonare_project_tempo_segment_count(legacy, &legacy_count) == SONARE_OK);
  sonare_project_destroy(legacy);

  CHECK(segment_count(&defaults) == legacy_count);
  CHECK(segment_count(nullptr) == legacy_count);

  // Following the tempo has to change the map, otherwise the option reached
  // nothing. This is the whole point of the entry point.
  SonareProjectTempoOptions adaptive = defaults;
  adaptive.adaptive_tempo = 1;
  const size_t adaptive_count = segment_count(&adaptive);
  CHECK(adaptive_count > legacy_count);

  // A coarser ramp threshold merges more of the take into constant stretches,
  // which is the direction the field is documented to move.
  SonareProjectTempoOptions coarse = adaptive;
  coarse.ramp_threshold = 0.20f;
  CHECK(segment_count(&coarse) < adaptive_count);

  // A zeroed struct is rejected rather than read as "use the defaults": it was not started
  // from the init function, so its struct_size is unset.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectTempoOptions zeroed{};
  float bpm = 0.0f;
  CHECK(sonare_project_auto_tempo_with_options(project, audio.data(), audio.size(), kSampleRate,
                                               &zeroed, 0, 0,
                                               &bpm) == SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  CHECK(sonare_project_analyze_tempo_with_options(project, audio.data(), audio.size(), kSampleRate,
                                                  &zeroed, candidates, std::size(candidates),
                                                  &count) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_analyze_tempo_with_options(project, audio.data(), audio.size(),
                                                    kSampleRate, &adaptive, candidates,
                                                    std::size(candidates), &count) == SONARE_OK);
  CHECK(count > 0);
  sonare_project_destroy(project);
}

TEST_CASE("project C unresolved source descriptors carry the full URI", "[project]") {
  SonareProject* source_project = nullptr;
  REQUIRE(sonare_project_create(&source_project) == SONARE_OK);
  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(source_project, &track_desc, &track) == SONARE_OK);

  const std::string long_uri = "asset://" + std::string(300, 'a') + ".wav";
  const std::string short_uri = "asset://stem.wav";
  const std::string* uris[] = {&long_uri, &short_uri};
  for (size_t i = 0; i < 2; ++i) {
    SonareProjectClipDesc desc{};
    desc.track_id = track;
    desc.start_ppq = 4.0 * static_cast<double>(i);
    desc.length_ppq = 1.0;
    desc.source_uri = uris[i]->c_str();
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_clip(source_project, &desc, &clip) == SONARE_OK);
  }
  uint32_t second_id = 0;
  REQUIRE(sonare_project_unresolved_audio_source_id_by_index(source_project, 1, &second_id) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_audio_source_metadata(source_project, second_id, "sha256:abc",
                                                   "vocals") == SONARE_OK);
  const std::string json = serialize(source_project);
  sonare_project_destroy(source_project);

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &project, nullptr) == SONARE_OK);

  size_t count = 0;
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &count) == SONARE_OK);
  REQUIRE(count == 2);
  std::vector<uint32_t> ids;
  for (size_t i = 0; i < count; ++i) {
    uint32_t id = 0;
    REQUIRE(sonare_project_unresolved_audio_source_id_by_index(project, i, &id) == SONARE_OK);
    SonareProjectSource source{};
    REQUIRE(sonare_project_unresolved_audio_source_by_index(project, i, &source) == SONARE_OK);
    CHECK(source.id == id);
    CHECK(source.kind == 0);
    CHECK(std::string(source.name_or_uri) == uris[i]->substr(0, 127));
    char* uri = nullptr;
    REQUIRE(sonare_project_get_audio_source_uri(project, id, &uri) == SONARE_OK);
    REQUIRE(uri != nullptr);
    CHECK(std::string(uri) == *uris[i]);
    sonare_free_string(uri);
    ids.push_back(id);
  }
  SonareProjectAudioSourceMetadata metadata{};
  REQUIRE(sonare_project_get_audio_source_metadata(project, ids[1], &metadata) == SONARE_OK);
  CHECK(std::string(metadata.content_hash) == "sha256:abc");
  CHECK(std::string(metadata.external_stem_role) == "vocals");
  sonare_free_project_audio_source_metadata(&metadata);

  SonareProjectSource out{};
  CHECK(sonare_project_unresolved_audio_source_by_index(project, 2, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_unresolved_audio_source_by_index(project, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  char* none = nullptr;
  CHECK(sonare_project_get_audio_source_uri(project, 0, &none) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(none == nullptr);

  const std::vector<float> samples(16, 0.25f);
  REQUIRE(sonare_project_set_source_audio(project, ids[0], samples.data(), samples.size(), 1,
                                          48000) == SONARE_OK);
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &count) == SONARE_OK);
  REQUIRE(count == 1);
  REQUIRE(sonare_project_unresolved_audio_source_by_index(project, 0, &out) == SONARE_OK);
  CHECK(out.id == ids[1]);
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

  size_t unresolved = 0;
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 1);
  uint32_t source_id = 0;
  REQUIRE(sonare_project_unresolved_audio_source_id_by_index(project, 0, &source_id) == SONARE_OK);
  const std::vector<float> samples(16, 0.25f);
  REQUIRE(sonare_project_set_source_audio(project, source_id, samples.data(), samples.size(), 1,
                                          48000) == SONARE_OK);
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 0);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 1);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_unresolved_audio_source_count(project, &unresolved) == SONARE_OK);
  REQUIRE(unresolved == 0);

  sonare_project_destroy(project);
}

TEST_CASE("project C surface MIR refusals name the parameter", "[project]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  const auto message = [] { return std::string(sonare_last_error_message()); };
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  constexpr int kRate = 22050;
  std::vector<float> audio(static_cast<std::size_t>(kRate) * 5u);
  for (std::size_t i = 0; i < audio.size(); ++i) {
    audio[i] = 0.25f * std::sin(0.05f * static_cast<float>(i));
  }

  // Two NULL pointers: the first one in header order is named.
  REQUIRE(sonare_project_snap_to_grid_ex(nullptr, 0.0, 0.5, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "project must not be NULL");

  double snapped = 0.0;
  REQUIRE(sonare_project_snap_to_grid_ex(project, -1.0, 0.5, 1, &snapped) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "ppq must be finite and at least 0");
  REQUIRE(sonare_project_snap_to_grid_ex(project, 1.0, 2.0, 1, &snapped) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "strength must be finite and in [0, 1]");
  REQUIRE(sonare_project_snap_to_grid_ex(project, 1.0, 0.5, 2000, &snapped) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "division must be an integer in [0, 1024]");
  REQUIRE(sonare_project_snap_to_grid_ex(project, 1.0, 0.5, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "out_ppq must not be NULL");

  size_t count = 0;
  REQUIRE(sonare_project_analyze_tempo(project, audio.data(), audio.size(), kRate, nullptr, 2,
                                       &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "candidates must not be NULL when capacity is not 0");

  SonareProjectTempoOptions options;
  sonare_project_tempo_options_init(&options);
  options.tempo_update_interval_beats = 0;
  SonareProjectTempoCandidate candidates[4];
  REQUIRE(sonare_project_analyze_tempo_with_options(project, audio.data(), audio.size(), kRate,
                                                    &options, candidates, 4,
                                                    &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "options.tempo_update_interval_beats must be an integer in [1, 2147483647]");
  sonare_project_tempo_options_init(&options);
  options.ramp_threshold = -0.5f;
  REQUIRE(sonare_project_analyze_tempo_with_options(project, audio.data(), audio.size(), kRate,
                                                    &options, candidates, 4,
                                                    &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "options.ramp_threshold must be finite and at least 0");
  sonare_project_tempo_options_init(&options);
  options.struct_size = sizeof(options) - 1;
  REQUIRE(sonare_project_analyze_tempo_with_options(project, audio.data(), audio.size(), kRate,
                                                    &options, candidates, 4,
                                                    &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "options.struct_size must be set by sonare_project_tempo_options_init");

  SonareProjectKeySegment key{0.0, 4.0, 12u, 1u};
  REQUIRE(sonare_project_annotate_keys(project, &key, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "keys[0].tonic_pc must be an integer in [0, 11] or 255");
  key = {4.0, 4.0, 0u, 1u};
  REQUIRE(sonare_project_annotate_keys(project, &key, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "keys[0].end_ppq must be greater than keys[0].start_ppq");
  key = {0.0, 4.0, 0u, 8u};
  REQUIRE(sonare_project_annotate_keys(project, &key, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "keys[0].mode must name a declared value");
  REQUIRE(sonare_project_annotate_keys(project, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "keys must not be NULL when count is not 0");

  const uint8_t degrees[1] = {7};
  SonareProjectChordSymbol chord{};
  chord.end_ppq = 4.0;
  chord.root_pc = 255u;
  chord.quality = 1u;
  chord.extensions = degrees;
  chord.extension_count = 33;
  chord.slash_bass_pc = 255u;
  REQUIRE(sonare_project_annotate_chords(project, &chord, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "chords[0].extension_count must be an integer in [0, 32]");

  uint32_t root_pc = 0;
  uint32_t quality = 0;
  uint32_t bass_pc = 0;
  size_t extension_count = 0;
  REQUIRE(sonare_chord_symbol_from_analysis(
              static_cast<SonarePitchClass>(12), SONARE_CHORD_MAJOR, SONARE_PITCH_C, &root_pc,
              &quality, nullptr, 0, &extension_count, &bass_pc) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "root must be an integer in [0, 11]");
  REQUIRE(sonare_chord_symbol_from_analysis(
              SONARE_PITCH_C, SONARE_CHORD_DOMINANT7_SHARP9, SONARE_PITCH_C, &root_pc, &quality,
              nullptr, 0, &extension_count, &bass_pc) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message().rfind("extension_capacity must be an integer of at least ", 0) == 0);

  uint32_t mode = 0;
  REQUIRE(sonare_key_mode_from_analysis(static_cast<SonareMode>(7), &mode) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "mode must be an integer in [0, 6]");
  REQUIRE(sonare_key_mode_from_analysis(SONARE_MODE_MAJOR, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "out_mode must not be NULL");

  REQUIRE(sonare_align_take_to_reference_ex(audio.data(), audio.size(), kRate, audio.data(),
                                            audio.size(), kRate, nullptr, nullptr, nullptr,
                                            nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(message() == "out_anchors must not be NULL");

  sonare_project_destroy(project);
#endif
}
