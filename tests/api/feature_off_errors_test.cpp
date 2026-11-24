#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>
#include <string_view>

#if (defined(SONARE_BUILD_ACOUSTIC_SIM) && !SONARE_BUILD_ACOUSTIC_SIM) ||                 \
    (defined(SONARE_BUILD_MIXING_ASSISTANT) && !SONARE_BUILD_MIXING_ASSISTANT) ||         \
    (defined(SONARE_BUILD_ASSIST) && !SONARE_BUILD_ASSIST && SONARE_BUILD_ARRANGEMENT) || \
    (defined(SONARE_BUILD_MASTERING) && !SONARE_BUILD_MASTERING) ||                       \
    (defined(SONARE_BUILD_VOICE_CHANGER) && !SONARE_BUILD_VOICE_CHANGER) ||               \
    (defined(SONARE_BUILD_PITCH_EDITOR) && !SONARE_BUILD_PITCH_EDITOR) ||                 \
    (defined(SONARE_BUILD_GRAPH) && !SONARE_BUILD_GRAPH) ||                               \
    (defined(SONARE_BUILD_PLAYBACK) && !SONARE_BUILD_PLAYBACK)
namespace {

void check_not_compiled_in(std::string_view message, std::string_view entry = {}) {
  CHECK(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_error_message() != nullptr);
  CHECK(std::string_view(sonare_last_error_message()) == message);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string_view(sonare_last_error_reason()) == "notCompiledIn");
  CHECK(sonare_last_refusal_subject_count() == 0);
  const char* recorded = sonare_last_refusal_entry();
  REQUIRE(recorded != nullptr);
  if (!entry.empty()) CHECK(std::string_view(recorded) == entry);
}

}  // namespace
#endif

#if defined(SONARE_BUILD_MIXING_ASSISTANT) && !SONARE_BUILD_MIXING_ASSISTANT
TEST_CASE("An unavailable mixing assistant records the source-class refusal",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = SONARE_BUILD_MIXING
                                           ? "mixing assistant support is not compiled in"
                                           : "mixing support is not compiled in";
  const char* names = sonare_mixing_assistant_source_class_names();
  REQUIRE(names != nullptr);
  CHECK(std::string_view(names).empty());
  check_not_compiled_in(message);
  for (const char* name : {"vocal", static_cast<const char*>(nullptr)}) {
    CHECK(sonare_mixing_assistant_source_class_from_name(name) == -1);
    check_not_compiled_in(message);
  }
}
#endif

#if defined(SONARE_BUILD_ASSIST) && !SONARE_BUILD_ASSIST && SONARE_BUILD_ARRANGEMENT
TEST_CASE("An unavailable composition assistant records the module-list refusal",
          "[c_api][compiled_out]") {
  CHECK(sonare_assist_module_ids() == nullptr);
  check_not_compiled_in("composition assist support is not compiled in", "assist_module_ids");
}
#endif

#if defined(SONARE_BUILD_VOICE_CHANGER) && !SONARE_BUILD_VOICE_CHANGER
TEST_CASE("An unavailable voice changer records preset metadata refusals",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "voice changer support is not compiled in";
  const char* names = sonare_realtime_voice_changer_preset_names();
  REQUIRE(names != nullptr);
  CHECK(std::string_view(names).empty());
  check_not_compiled_in(message);
  for (const auto ordinal :
       {SONARE_VC_PRESET_NEUTRAL_MONITOR, static_cast<SonareVoiceCharacterPreset>(-1)}) {
    CHECK(sonare_voice_character_preset_id(ordinal) == nullptr);
    check_not_compiled_in(message);
  }
}
#endif

#if defined(SONARE_BUILD_VOICE_CHANGER) && SONARE_BUILD_VOICE_CHANGER
TEST_CASE("Voice changer preset metadata preserves the last error", "[c_api][voice_changer]") {
  for (const auto getter : {
           +[] { return sonare_realtime_voice_changer_preset_names(); },
           +[] { return sonare_voice_character_preset_id(SONARE_VC_PRESET_NEUTRAL_MONITOR); },
       }) {
    char* json = nullptr;
    REQUIRE(sonare_realtime_voice_changer_preset_json(nullptr, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(json == nullptr);
    REQUIRE(sonare_last_error_message() != nullptr);
    const std::string message(sonare_last_error_message());
    REQUIRE(getter() != nullptr);
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_last_error_message() != nullptr);
    CHECK(std::string_view(sonare_last_error_message()) == message);
  }
}
#endif

#if defined(SONARE_BUILD_MASTERING) && !SONARE_BUILD_MASTERING
TEST_CASE("Unavailable mastering metadata records its refusal", "[c_api][compiled_out]") {
  constexpr std::string_view message = "mastering support is not compiled in";
  for (const auto names :
       {sonare_mastering_preset_names, sonare_mastering_processor_names,
        sonare_mastering_pair_processor_names, sonare_mastering_pair_analysis_names,
        sonare_mastering_stereo_analysis_names, sonare_mastering_insert_names,
        sonare_mastering_platform_names}) {
    CHECK(names() == nullptr);
    check_not_compiled_in(message);
  }
  for (const char* name : {"spotify", static_cast<const char*>(nullptr)}) {
    CHECK(sonare_mastering_platform_from_name(name) == -1);
    check_not_compiled_in(message);
    CHECK(sonare_mastering_preset_from_name(name) == -1);
    check_not_compiled_in(message);
  }
  sonare_eq_clear(nullptr);
  check_not_compiled_in(message, "eq_clear");
  sonare_eq_clear_sidechain(nullptr);
  check_not_compiled_in(message, "eq_clear_sidechain");
  sonare_eq_set_auto_gain(nullptr, 1);
  check_not_compiled_in(message, "eq_set_auto_gain");
  CHECK(sonare_eq_last_auto_gain_db(nullptr) == 0.0f);
  check_not_compiled_in(message);
  CHECK(sonare_eq_latency_samples(nullptr) == 0);
  check_not_compiled_in(message);
  CHECK(sonare_streaming_mastering_chain_latency_samples(nullptr) == 0);
  check_not_compiled_in(message);
  CHECK(sonare_streaming_mastering_chain_stage_names(nullptr) == nullptr);
  check_not_compiled_in(message);
}
#endif

#if defined(SONARE_BUILD_PITCH_EDITOR) && !SONARE_BUILD_PITCH_EDITOR
TEST_CASE("An unavailable vocal editor records its detailed and common refusal",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "pitch editor support is not compiled in";
  const float sample = 0.25f;
  for (const int rate : {48000, -1}) {
    SonareVocalSession* session = nullptr;
    CHECK(sonare_vocal_session_create(&sample, 1, 1, rate, nullptr, &session) ==
          SONARE_ERROR_NOT_SUPPORTED);
    CHECK(session == nullptr);
    check_not_compiled_in(message);
  }
  SonareVocalAnalysisResult result;
  sonare_vocal_analysis_result_init(&result);
  result.sample_rate = 48000;
  CHECK(sonare_vocal_session_analysis(nullptr, &result) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(result.struct_size == sizeof(result));
  CHECK(result.sample_rate == 0);
  check_not_compiled_in(message);
  SonareVocalErrorDetail detail;
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  CHECK(detail.reason == SONARE_VOCAL_REASON_UNSUPPORTED);
}
#endif

#if defined(SONARE_BUILD_GRAPH) && !SONARE_BUILD_GRAPH
TEST_CASE("An unavailable engine graph is refused before inspecting its dimensions",
          "[c_api][compiled_out]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  SonareEngineGraphNode node;
  sonare_engine_graph_node_init(&node);
  SonareEngineGraphSpec spec{};
  std::snprintf(node.id, sizeof(node.id), "%s", "test");
  std::snprintf(spec.input_node, sizeof(spec.input_node), "%s", node.id);
  std::snprintf(spec.output_node, sizeof(spec.output_node), "%s", node.id);
  spec.nodes = &node;
  spec.node_count = 1;
  for (const int channels : {2, 0}) {
    spec.num_channels = channels;
    CHECK(sonare_engine_set_graph(engine, &spec) == SONARE_ERROR_NOT_SUPPORTED);
    check_not_compiled_in("graph support is not compiled in", "engine_set_graph");
  }
  sonare_engine_destroy(engine);
  CHECK(sonare_engine_set_graph(nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in("graph support is not compiled in", "engine_set_graph");
  size_t count = 23;
  CHECK(sonare_engine_graph_node_count(nullptr, &count) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(count == 0);
  CHECK(sonare_engine_graph_node_count(nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in("graph support is not compiled in", "engine_graph_node_count");
  count = 23;
  CHECK(sonare_engine_graph_connection_count(nullptr, &count) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(count == 0);
  CHECK(sonare_engine_graph_connection_count(nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in("graph support is not compiled in", "engine_graph_connection_count");
}
#endif

#if defined(SONARE_BUILD_PITCH_EDITOR) && !SONARE_BUILD_PITCH_EDITOR
TEST_CASE("An unavailable pitch editor refuses before validating audio and note positions",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "pitch editor support is not compiled in";
  const float samples[4] = {0.2f, 0.3f, 0.2f, 0.1f};
  const float f0 = 220.0f;
  float sentinel = 0.0f;
  const auto check_call = [&](std::string_view entry, auto call) {
    float* out = &sentinel;
    size_t count = 23;
    CHECK(call(&out, &count) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(out == nullptr);
    CHECK(count == 0);
    check_not_compiled_in(message, entry);
  };
  constexpr int invalid_rate = -1;
  check_call("pitch_correct_to_midi", [&](float** out, size_t* count) {
    return sonare_pitch_correct_to_midi(samples, 4, invalid_rate, 60.0f, 62.0f, out, count);
  });
  check_call("pitch_correct_to_midi_timevarying", [&](float** out, size_t* count) {
    return sonare_pitch_correct_to_midi_timevarying(samples, 4, invalid_rate, &f0, nullptr, nullptr,
                                                    1, 128, 62.0f, out, count);
  });
  check_call("pitch_correct_timevarying", [&](float** out, size_t* count) {
    return sonare_pitch_correct_timevarying(samples, 4, invalid_rate, &f0, nullptr, nullptr, 1, 128,
                                            nullptr, out, count);
  });
  check_call("auto_tune", [&](float** out, size_t* count) {
    return sonare_auto_tune(samples, 4, invalid_rate, nullptr, nullptr, out, count, nullptr);
  });
  check_call("note_stretch", [&](float** out, size_t* count) {
    return sonare_note_stretch(samples, 4, 48000, -1, 2, 1.25f, out, count);
  });
  check_call("note_move", [&](float** out, size_t* count) {
    return sonare_note_move(samples, 4, 48000, 0, 2, -1, out, count);
  });
}
#endif

#if defined(SONARE_BUILD_PLAYBACK) && !SONARE_BUILD_PLAYBACK
TEST_CASE("An unavailable playback renderer records the realtime entry refusal",
          "[c_api][compiled_out]") {
  const float sample = 0.25f;
  float output = -0.25f;
  CHECK(sonare_playback_renderer_process_interleaved(nullptr, &sample, 1, &output, 1, 1) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(output == -0.25f);
  check_not_compiled_in("playback support is not compiled in",
                        "playback_renderer_process_interleaved");

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  CHECK(sonare_last_refusal_entry() == nullptr);
  sonare_engine_destroy(engine);
}
#endif

#if defined(SONARE_BUILD_ACOUSTIC_SIM) && !SONARE_BUILD_ACOUSTIC_SIM
TEST_CASE("The unavailable acoustic simulation refuses NULL inputs and outputs",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "acoustic simulation support is not compiled in";
  CHECK(sonare_room_morph(nullptr, {}, {}, nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "room_morph");
  CHECK(sonare_room_morph_stereo(nullptr, nullptr, {}, {}, nullptr, nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "room_morph_stereo");
  float sentinel = 1.0f;
  float* left = &sentinel;
  float* right = &sentinel;
  size_t length = 23;
  CHECK(sonare_room_morph_stereo(nullptr, nullptr, {}, {}, nullptr, &left, &right, &length) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(left == nullptr);
  CHECK(right == nullptr);
  CHECK(length == 0);
  check_not_compiled_in(message, "room_morph_stereo");
}
#endif

#if defined(SONARE_BUILD_ASSIST) && !SONARE_BUILD_ASSIST && SONARE_BUILD_ARRANGEMENT
TEST_CASE("The unavailable composition assistant refuses NULL inputs and outputs",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "composition assist support is not compiled in";
  CHECK(sonare_project_assist_apply_json(nullptr, nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "project_assist_apply_json");
  CHECK(sonare_project_assist_preview_json(nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "project_assist_preview_json");
}
#endif

#if defined(SONARE_BUILD_MIXING_ASSISTANT) && !SONARE_BUILD_MIXING_ASSISTANT
TEST_CASE("The unavailable mixing assistant refuses NULL inputs and outputs",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = SONARE_BUILD_MIXING
                                           ? "mixing assistant support is not compiled in"
                                           : "mixing support is not compiled in";
  CHECK(sonare_mixing_assistant_suggest(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},
                                        {}, nullptr, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "mixing_assistant_suggest");
  char sentinel = 0;
  char* json = &sentinel;
  CHECK(sonare_mixing_assistant_suggest(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},
                                        {}, nullptr, {}, &json) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(json == nullptr);
  check_not_compiled_in(message, "mixing_assistant_suggest");
}
#endif

#if defined(SONARE_BUILD_PITCH_EDITOR) && !SONARE_BUILD_PITCH_EDITOR
TEST_CASE("The unavailable pitch editor refuses NULL inputs and outputs", "[c_api][compiled_out]") {
  constexpr std::string_view message = "pitch editor support is not compiled in";
  CHECK(sonare_auto_tune(nullptr, {}, {}, nullptr, nullptr, nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "auto_tune");
  CHECK(sonare_extract_notes(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {}, nullptr,
                             nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "extract_notes");
  float sentinel = 1.0f;
  SonareNoteObjectsResult extracted{};
  extracted.amplitude = &sentinel;
  extracted.amplitude_count = 23;
  CHECK(sonare_extract_notes(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {}, nullptr,
                             &extracted) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(extracted.amplitude == nullptr);
  CHECK(extracted.amplitude_count == 0);
  check_not_compiled_in(message, "extract_notes");

  float* audio = &sentinel;
  CHECK(sonare_render_notes(nullptr, {}, {}, nullptr, {}, nullptr, {}, nullptr, nullptr, {}, {},
                            nullptr, &audio, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(audio == nullptr);
  check_not_compiled_in(message, "render_notes");
  CHECK(sonare_merge_notes(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {}, nullptr, nullptr, {},
                           nullptr, {}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "merge_notes");
  CHECK(sonare_note_move(nullptr, {}, {}, {}, {}, {}, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "note_move");
  CHECK(sonare_note_segments(nullptr, nullptr, nullptr, {}, {}, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "note_segments");
  CHECK(sonare_note_stretch(nullptr, {}, {}, {}, {}, {}, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "note_stretch");
  CHECK(sonare_pitch_correct_timevarying(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {},
                                         nullptr, nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "pitch_correct_timevarying");
  CHECK(sonare_pitch_correct_to_midi(nullptr, {}, {}, {}, {}, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "pitch_correct_to_midi");
  CHECK(sonare_pitch_correct_to_midi_timevarying(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {},
                                                 {}, nullptr,
                                                 nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "pitch_correct_to_midi_timevarying");
  CHECK(sonare_render_notes(nullptr, {}, {}, nullptr, {}, nullptr, {}, nullptr, nullptr, {}, {},
                            nullptr, nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "render_notes");
  CHECK(sonare_scale_correction_semitones({}, {}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "scale_correction_semitones");
  CHECK(sonare_scale_pitch_class_enabled({}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "scale_pitch_class_enabled");
  CHECK(sonare_scale_quantize_midi({}, {}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "scale_quantize_midi");
  CHECK(sonare_split_note(nullptr, {}, {}, nullptr, nullptr, nullptr, {}, {}, nullptr, nullptr, {},
                          nullptr, {}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "split_note");
}
#endif

#if defined(SONARE_BUILD_PITCH_EDITOR) && !SONARE_BUILD_PITCH_EDITOR && SONARE_BUILD_ARRANGEMENT
TEST_CASE("The unavailable project vocal editor refuses NULL inputs and outputs",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "pitch editor support is not compiled in";
  CHECK(sonare_project_apply_vocal_edit(nullptr, nullptr, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "project_apply_vocal_edit");
  CHECK(sonare_project_rehydrate_vocal_edits(nullptr, nullptr, {}, nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "project_rehydrate_vocal_edits");
}
#endif

#if defined(SONARE_BUILD_VOICE_CHANGER) && !SONARE_BUILD_VOICE_CHANGER
TEST_CASE("The unavailable voice changer refuses NULL inputs and outputs",
          "[c_api][compiled_out]") {
  constexpr std::string_view message = "voice changer support is not compiled in";
  CHECK(sonare_streaming_retune_create({}, {}, {}, nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "streaming_retune_create");
  CHECK(sonare_voice_change(nullptr, {}, {}, nullptr, nullptr, nullptr) ==
        SONARE_ERROR_NOT_SUPPORTED);
  check_not_compiled_in(message, "voice_change");
}
#endif
