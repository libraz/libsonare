/// @file sonare_c_engine_contract_test.cpp
/// @brief Engine C ABI contract: version, capabilities, feature-flag
///        reporting, malformed JSON and the per-thread last-error channel.

#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/synth/sf2_player.h"
#include "sonare_c_engine_test_helpers.h"
#include "support/alloc_guard.h"
#include "support/c_strings.h"

namespace {

// Binds a patch-only NativeSynth (no GM programs, no sample bank) on a destination.
SonareError set_synth_patch(SonareEngine* engine, uint32_t destination_id,
                            const SonareSynthPatch* patch) {
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = destination_id;
  binding.patch = *patch;
  return sonare_engine_set_synth_instrument(engine, &binding);
}

}  // namespace

TEST_CASE("sonare_error_message", "[c_api]") {
  SECTION("returns messages for all error codes") {
    REQUIRE(std::strcmp(sonare_error_message(SONARE_OK), "OK") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_FILE_NOT_FOUND), "File not found") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_FORMAT), "Invalid format") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_DECODE_FAILED), "Decode failed") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_PARAMETER),
                        "Invalid parameter") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_OUT_OF_MEMORY), "Out of memory") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_UNKNOWN), "Unknown error") == 0);
  }

  SECTION("names the ABI mismatch a binding reports") {
    const std::string mismatch = sonare_error_message(SONARE_ERROR_ABI_MISMATCH);
    REQUIRE(mismatch == "Native library ABI mismatch");
    REQUIRE(mismatch != sonare_error_message(SONARE_ERROR_UNKNOWN));
  }
}

TEST_CASE("sonare_version", "[c_api]") {
  SECTION("returns version string") {
    const char* ver = sonare_version();
    REQUIRE(ver != nullptr);
    REQUIRE(std::strlen(ver) > 0);
  }

  SECTION("returns engine ABI version") { REQUIRE(sonare_engine_abi_version() > 0); }
}

TEST_CASE("sonare_capabilities_json", "[c_api]") {
  const std::string json = sonare_test::c_string(sonare_capabilities_json);

  const auto capabilities = sonare::util::json::parse_strict(json);
  REQUIRE(capabilities["version"].as_string() == sonare_version());
  REQUIRE(capabilities["abi"]["project"].as_number() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(capabilities["abi"]["engine"].as_number() == sonare_engine_abi_version());
  REQUIRE(capabilities["features"]["ffmpeg"].as_bool() == (sonare_has_feature("ffmpeg") != 0));
  // The assistant's entry points exist in every build and answer NOT_SUPPORTED
  // when it was compiled out, so this flag is the only way a caller can tell
  // the configurations apart. Compared against the build macro rather than
  // asserted true, so the check means something in either configuration.
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  REQUIRE(capabilities["features"]["mixingAssistant"].as_bool());
#else
  REQUIRE_FALSE(capabilities["features"]["mixingAssistant"].as_bool());
#endif
  REQUIRE(capabilities["decode"]["builtin"][static_cast<std::size_t>(0)].as_string() == "wav");
  REQUIRE(capabilities["decode"]["builtin"][static_cast<std::size_t>(1)].as_string() == "mp3");
  REQUIRE(capabilities["hardwareConcurrency"].as_number() >= 1);
}

TEST_CASE("sonare_engine MIDI scalar commands respect arrangement feature flag", "[c_api]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_OK);
#else
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 16, 0, 74, 100, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_graph_node/connection_count report feature availability consistently",
          "[c_api][engine][graph]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  size_t node_count = 123;
  size_t connection_count = 456;
#if defined(SONARE_WITH_GRAPH)
  REQUIRE(sonare_engine_graph_node_count(engine, &node_count) == SONARE_OK);
  REQUIRE(sonare_engine_graph_connection_count(engine, &connection_count) == SONARE_OK);
  REQUIRE(node_count == 0);
  REQUIRE(connection_count == 0);
#else
  // A feature-off build has no graph at all: report NOT_SUPPORTED like
  // sonare_engine_set_graph does, rather than SONARE_OK with a count of 0,
  // which a caller cannot distinguish from "compiled in and genuinely empty".
  REQUIRE(sonare_engine_graph_node_count(engine, &node_count) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_graph_connection_count(engine, &connection_count) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(node_count == 0);
  REQUIRE(connection_count == 0);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("engine MIDI getters define their out-parameter on every exit path",
          "[c_api][engine][midi]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  size_t dropped = 0xDEADBEEF;
  uint32_t automation_id = 0xDEADBEEF;
  size_t instrument_count = 123;
  size_t pending_count = 456;

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_external_midi_dropped_count(engine, &dropped) == SONARE_OK);
  // No instrument is bound to this destination, so the name cannot resolve.
  REQUIRE(sonare_engine_resolve_instrument_automation_id(engine, 7, "cutoff", &automation_id) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &instrument_count) == SONARE_OK);
  REQUIRE(sonare_engine_midi_input_pending_count(engine, &pending_count) == SONARE_OK);
#else
  REQUIRE(sonare_engine_external_midi_dropped_count(engine, &dropped) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_resolve_instrument_automation_id(engine, 7, "cutoff", &automation_id) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &instrument_count) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_midi_input_pending_count(engine, &pending_count) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(dropped == 0);
  REQUIRE(automation_id == 0);
  REQUIRE(instrument_count == 0);
  REQUIRE(pending_count == 0);

  // The rejected-buffer path writes the count before it validates max_events.
  size_t drained = 789;
  SonareExternalMidiEvent events[3] = {};
  REQUIRE(sonare_engine_drain_external_midi(engine, events, 3, &drained) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(drained == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_push_midi_sysex enforces the documented payload ceiling",
          "[c_api][engine][midi]") {
  // The header promises 1..512 bytes and the C-ABI guard spells that ceiling as
  // the engine constant. Drive the boundary from the constant so a change to it
  // that leaves the header prose behind fails here as well as at the
  // static_assert in sonare_c_engine.cpp.
  constexpr size_t ceiling = sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes;
  REQUIRE(ceiling == 512);

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  std::vector<uint8_t> payload(ceiling + 1, 0x00);
  payload.front() = 0xF0;
  payload[ceiling - 1] = 0xF7;
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, payload.data(), ceiling, -1) == SONARE_OK);
#else
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, payload.data(), ceiling, -1) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, payload.data(), ceiling + 1, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, payload.data(), 0, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, nullptr, ceiling, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("C SysEx distinguishes unsupported prepared controls from queue pressure",
          "[c_api][engine][midi][gs]") {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MASTERING)
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  sonare::midi::synth::Sf2PlayerConfig config;
  config.bank_rig_binding = false;
  // A real gain processor lacks the controls required by the GS overdrive
  // stage. Retrying an identical operation cannot repair that capability.
  config.insert_factory = [](std::string_view, std::string_view) {
    return sonare::mastering::api::make_insert("utility.gain", "{}");
  };
  sonare::midi::synth::Sf2Player player(config);
  REQUIRE(engine->engine.set_midi_instrument(7, &player));
  const uint8_t type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  REQUIRE(sonare_engine_push_midi_sysex(engine, 7, type, sizeof(type), 4096) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_sysex(engine, 7, type, sizeof(type), 4096) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(engine->engine.midi_instrument(7) == &player);

  // Queue pressure on a destination with nothing to prepare is retryable.
  const uint8_t gm_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  SonareError pressure = SONARE_OK;
  for (int i = 0; i < 64 && pressure == SONARE_OK; ++i) {
    pressure = sonare_engine_push_midi_sysex(engine, 9, gm_on, sizeof(gm_on), 4096);
  }
  REQUIRE(pressure == SONARE_ERROR_OUT_OF_MEMORY);
  REQUIRE(engine->engine.last_midi_sysex_push_status() ==
          sonare::engine::MidiSysExPushStatus::kQueueFull);
  REQUIRE(engine->engine.set_midi_instrument(7, nullptr));
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("C MIDI instrument bind maps an allocation failure to OUT_OF_MEMORY",
          "[c_api][engine][midi]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  SonareEngineBuiltinSynthConfig synth{};
  sonare_engine_builtin_synth_config_init(&synth);
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  auto* const original = engine->engine.midi_instrument(7);
  REQUIRE(original != nullptr);

  // A rebind restages the accepted clip bank; failing that copy is a resource
  // failure, not a refusal, and leaves the original binding and owner in place.
  // The payload dwarfs every allocation the replacement synth itself makes.
  std::vector<uint8_t> payload(size_t{1} << 20, 0);
  payload.front() = 0xF0;
  payload.back() = 0xF7;
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  clip.length_samples = 48000;
  sonare::midi::MidiEvent event;
  event.render_frame = 4096;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  clip.events.push_back(event);
  engine->engine.set_midi_clips({clip});

  SonareError result = SONARE_OK;
  {
    sonare::test::AllocationFailureGuard guard(payload.size());
    result = sonare_engine_set_builtin_instrument(engine, 7, &synth);
  }
  CHECK(result == SONARE_ERROR_OUT_OF_MEMORY);
  CHECK(engine->engine.last_midi_instrument_bind_status() ==
        sonare::engine::MidiInstrumentBindStatus::kOutOfMemory);
  REQUIRE(engine->builtin_instruments.size() == 1);
  REQUIRE(engine->builtin_instruments.front().second.get() == original);
  REQUIRE(engine->engine.midi_instrument(7) == original);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("MIDI instrument bind reports a refused scheduled SysEx as a preparation failure",
          "[c_api][engine][midi][gs]") {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MASTERING)
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  // The C bind entries map kPreparationFailed to SONARE_ERROR_INVALID_STATE;
  // no shipped C-constructed instrument refuses a valid GS frame, so the status
  // itself is asserted on a player whose inserts lack the overdrive controls.
  const uint8_t type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  clip.length_samples = 48000;
  sonare::midi::MidiEvent event;
  event.render_frame = 4096;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = type;
  event.sysex_payload_size = sizeof(type);
  clip.events.push_back(event);
  engine->engine.set_midi_clips({clip});

  sonare::midi::synth::Sf2PlayerConfig config;
  config.bank_rig_binding = false;
  config.insert_factory = [](std::string_view, std::string_view) {
    return sonare::mastering::api::make_insert("utility.gain", "{}");
  };
  sonare::midi::synth::Sf2Player player(config);
  REQUIRE_FALSE(engine->engine.set_midi_instrument(7, &player));
  REQUIRE(engine->engine.last_midi_instrument_bind_status() ==
          sonare::engine::MidiInstrumentBindStatus::kPreparationFailed);
  REQUIRE(engine->engine.midi_instrument(7) == nullptr);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("failed MIDI instrument clear retains the C-owned instrument",
          "[c_api][engine][midi][gs]") {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
  SonareEngineBuiltinSynthConfig synth{};
  sonare_engine_builtin_synth_config_init(&synth);
  synth.gain = 0.5f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  auto* const original = engine->engine.midi_instrument(7);
  REQUIRE(original != nullptr);

  // Clearing reparses the accepted clip bank transactionally. Fail its payload
  // allocation so the rack must retain the original binding and its owner.
  std::vector<uint8_t> payload(8192, 0);
  payload.front() = 0xF0;
  payload.back() = 0xF7;
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  clip.length_samples = 48000;
  sonare::midi::MidiEvent event;
  event.render_frame = 4096;
  event.ump.words[0] = 0x30000000u;
  event.ump.word_count = 2;
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  clip.events.push_back(event);
  engine->engine.set_midi_clips({clip});

  SonareError result = SONARE_OK;
  {
    sonare::test::AllocationFailureGuard guard(payload.size());
    result = sonare_engine_clear_midi_instrument(engine, 7);
  }
  CHECK(result == SONARE_ERROR_OUT_OF_MEMORY);
  REQUIRE(engine->builtin_instruments.size() == 1);
  REQUIRE(engine->builtin_instruments.front().second.get() == original);
  REQUIRE(engine->engine.midi_instrument(7) == original);
  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) == SONARE_OK);
  std::array<float, 128> left{};
  std::array<float, 128> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, nullptr, 2, 128) == SONARE_OK);
  REQUIRE(std::any_of(left.begin(), left.end(), [](float sample) { return sample != 0.0f; }));
  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  REQUIRE(engine->builtin_instruments.empty());
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("sonare_engine_set_warp_voice_capacity enforces the documented ceiling",
          "[c_api][engine]") {
  // Drive the boundary from the core constant, same reasoning as the SysEx
  // ceiling test above: a change to the constant that leaves the define or the
  // header prose behind fails here and at the static_assert in
  // sonare_c_engine.cpp.
  constexpr uint32_t ceiling = sonare::engine::RealtimeEngine::kMaxWarpVoices;
  REQUIRE(ceiling == SONARE_ENGINE_MAX_WARP_VOICES);

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  uint32_t voices = 0xDEADBEEF;
  REQUIRE(sonare_engine_warp_voice_capacity(engine, &voices) == SONARE_OK);
  REQUIRE(voices == 8);  // documented default

  REQUIRE(sonare_engine_set_warp_voice_capacity(engine, ceiling) == SONARE_OK);
  REQUIRE(sonare_engine_warp_voice_capacity(engine, &voices) == SONARE_OK);
  REQUIRE(voices == ceiling);

  // Rejected: state (including the getter's return value) is unchanged.
  REQUIRE(sonare_engine_set_warp_voice_capacity(engine, ceiling + 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_warp_voice_capacity(engine, &voices) == SONARE_OK);
  REQUIRE(voices == ceiling);

  REQUIRE(sonare_engine_set_warp_voice_capacity(engine, 0) == SONARE_OK);
  REQUIRE(sonare_engine_warp_voice_capacity(engine, &voices) == SONARE_OK);
  REQUIRE(voices == 0);

  REQUIRE(sonare_engine_set_warp_voice_capacity(nullptr, 4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_warp_voice_capacity(nullptr, &voices) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_warp_voice_capacity(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("malformed JSON exits every C-ABI entry point with the same code",
          "[c_api][engine][json]") {
  // Syntactically malformed input takes the code of what the JSON is: a document
  // is SONARE_ERROR_INVALID_FORMAT, a call's argument bag is
  // SONARE_ERROR_INVALID_PARAMETER, and no path reports UNKNOWN.
  const char* malformed[] = {
      "not json", "{", "{\"a\":}", "{\"a\":1,}", "[1,", "", "{\"a\":1 \"b\":2}",
  };

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);
#if defined(SONARE_WITH_ARRANGEMENT)
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
#endif

  for (const char* json : malformed) {
    INFO(json);
#if defined(SONARE_WITH_ARRANGEMENT)
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, json) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_project_set_mixer_scene_json(project, json) == SONARE_ERROR_INVALID_FORMAT);
#endif
#if defined(SONARE_WITH_MIXING)
    REQUIRE(sonare_engine_set_master_strip_json(engine, json) == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_engine_set_track_strip_json(engine, 1, json) == SONARE_ERROR_INVALID_FORMAT);
#endif
#if defined(SONARE_WITH_MASTERING)
    SonareEq* eq = nullptr;
    REQUIRE(sonare_eq_create(48000, 512, &eq) == SONARE_OK);
    REQUIRE(eq != nullptr);
    REQUIRE(sonare_eq_set_band(eq, 0, json) == SONARE_ERROR_INVALID_FORMAT);
    sonare_eq_destroy(eq);
#endif
  }

#if defined(SONARE_WITH_ARRANGEMENT)
  sonare_project_destroy(project);
#endif
  sonare_engine_destroy(engine);
}

TEST_CASE("a scene JSON entry point separates a malformed document from an invalid value",
          "[c_api][engine][json]") {
  // INVALID_FORMAT means the document did not parse. A document that parses and
  // then fails a scene rule carries its own code, which is the difference
  // between "your file is corrupt" and "this value is not accepted".
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  const char* malformed = R"({"version":2,"strips":[)";
  const char* unsupported_version =
      R"({"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]})";
  const char* unknown_insert_slot =
      R"({"version":2,"strips":[{"id":"master","inserts":[{"slot":"nowhere","processor":"eq.parametric","params":{}}]}],"buses":[],"connections":[]})";

  REQUIRE(sonare_engine_set_master_strip_json(engine, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_master_strip_json(engine, unsupported_version) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_master_strip_json(engine, unknown_insert_slot) ==
          SONARE_ERROR_INVALID_FORMAT);

  REQUIRE(sonare_engine_set_track_strip_json(engine, 1, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 1, unsupported_version) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, unsupported_version) ==
          SONARE_ERROR_INVALID_FORMAT);
#else
  REQUIRE(sonare_engine_set_master_strip_json(engine, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_last_error_message", "[c_api]") {
  SECTION("never returns null pointer") {
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
  }

  SECTION("captures detailed message when a C API call fails") {
    // 12 bytes of random non-audio data so format detection returns Unknown.
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    SonareAudio* audio = nullptr;

    SonareError err = sonare_audio_from_memory(garbage.data(), garbage.size(), &audio);
#ifdef SONARE_WITH_FFMPEG
    // With FFmpeg the buffer still fails to decode but the message comes from
    // FFmpeg rather than the static "Unsupported audio format" path.
    REQUIRE(err != SONARE_OK);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    REQUIRE(std::strlen(msg) > 0);
#else
    REQUIRE(err == SONARE_ERROR_INVALID_FORMAT);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    // The detailed message must be more informative than the generic code label.
    REQUIRE(std::string(msg).find("Unsupported audio format") != std::string::npos);
    REQUIRE(std::string(msg).find("WAV, MP3") != std::string::npos);
    REQUIRE(std::string(msg).find("ffmpeg") != std::string::npos);
#endif
    REQUIRE(audio == nullptr);
  }

  SECTION("validation early returns clear unrelated detailed errors") {
    const auto record_detailed_error = [] {
      const std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                            0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
      SonareAudio* audio = nullptr;
      REQUIRE(sonare_audio_from_memory(garbage.data(), garbage.size(), &audio) != SONARE_OK);
      REQUIRE(audio == nullptr);
      REQUIRE(std::strlen(sonare_last_error_message()) > 0);
    };

    record_detailed_error();
    REQUIRE(sonare_audio_from_memory(nullptr, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "data must not be NULL when length is not 0");

    record_detailed_error();
    REQUIRE(sonare_engine_prepare(nullptr, 48000, 128, 16, 16, 0) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) == "engine must not be NULL");

#if defined(SONARE_WITH_ARRANGEMENT)
    record_detailed_error();
    REQUIRE(sonare_project_create(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()) == "out must not be NULL");
#endif  // defined(SONARE_WITH_ARRANGEMENT)
  }

  SECTION("pointer-returning constructors replace stale diagnostics") {
#if defined(SONARE_WITH_MIXING)
    SonareMixer* refused_mixer = nullptr;
    REQUIRE(sonare_mixer_create(0, 128, &refused_mixer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_mixer == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("sample_rate") != std::string::npos);
    REQUIRE(sonare_mixer_from_scene_json(nullptr, 48000, 128) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()) == "json must not be NULL");
#endif  // defined(SONARE_WITH_MIXING)
#if defined(SONARE_WITH_MASTERING)
    SonareEq* refused_eq = nullptr;
    REQUIRE(sonare_eq_create(0, 128, &refused_eq) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_eq == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("sample_rate") != std::string::npos);
    SonareStreamingMasteringChain* refused_chain = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create("{", nullptr, &refused_chain) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_chain == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("config") != std::string::npos);
#endif  // defined(SONARE_WITH_MASTERING)
  }
}

TEST_CASE("sonare_last_error_code carries the code a NULL handle cannot", "[c_api]") {
  SECTION("is SONARE_OK when nothing is recorded") {
    // A successful error-returning entry clears whatever an earlier call recorded.
    char* note = nullptr;
    REQUIRE(sonare_hz_to_note(440.0f, &note) == SONARE_OK);
    sonare_free_string(note);
    REQUIRE(std::string(sonare_last_error_message()).empty());
    REQUIRE(sonare_last_error_code() == SONARE_OK);
  }

  SECTION("matches the code an error-returning call returned") {
    const std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                          0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    SonareAudio* audio = nullptr;
    const SonareError err = sonare_audio_from_memory(garbage.data(), garbage.size(), &audio);
    REQUIRE(err != SONARE_OK);
    REQUIRE(sonare_last_error_code() == err);
    // Reading the message is a diagnostic accessor and leaves the code alone.
    REQUIRE(std::strlen(sonare_last_error_message()) > 0);
    REQUIRE(sonare_last_error_code() == err);
  }

#if defined(SONARE_WITH_MIXING)
  SECTION("scene loads report the code each failure maps to") {
    // JsonError is a parse failure: InvalidFormat, as on the engine scene entry points.
    REQUIRE(sonare_mixer_from_scene_json("{", 48000, 128) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(std::strlen(sonare_last_error_message()) > 0);

    // A document that parses but breaks a scene rule is the document's defect: InvalidFormat.
    const char* unsupported_version =
        R"({"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]})";
    REQUIRE(sonare_mixer_from_scene_json(unsupported_version, 48000, 128) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_FORMAT);

    REQUIRE(sonare_mixer_from_scene_json(nullptr, 48000, 128) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);

    // A valid document whose size arguments the nested create refuses.
    const char* valid = R"({"version":2,"strips":[{"id":"a"}],"buses":[],"connections":[]})";
    REQUIRE(sonare_mixer_from_scene_json(valid, 0, 128) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("mixer constructors and lookups record a code on every NULL return") {
    SonareMixer* refused_mixer = nullptr;
    REQUIRE(sonare_mixer_create(0, 128, &refused_mixer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_mixer == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);

    SonareMixer* mixer = nullptr;
    REQUIRE(sonare_mixer_create(48000, 128, &mixer) == SONARE_OK);
    REQUIRE(mixer != nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_OK);

    REQUIRE(sonare_mixer_add_strip(nullptr, "a", nullptr) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    SonareMixerAddStripOptions bad_oversample;
    sonare_mixer_add_strip_options_init(&bad_oversample);
    bad_oversample.true_peak_oversample = 17;
    REQUIRE(sonare_mixer_add_strip(mixer, "a", &bad_oversample) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mixer_add_strip(mixer, "a", nullptr) != nullptr);
    REQUIRE(sonare_mixer_add_strip(mixer, "a", nullptr) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);

    REQUIRE(sonare_mixer_strip_at(mixer, 1) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::strlen(sonare_last_error_message()) > 0);
    REQUIRE(sonare_mixer_strip_by_id(mixer, "missing") == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::strlen(sonare_last_error_message()) > 0);
    REQUIRE(sonare_mixer_strip_by_id(mixer, "a") != nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_OK);

    // A later successful error-returning call clears the recorded code.
    REQUIRE(sonare_mixer_strip_by_id(mixer, "missing") == nullptr);
    size_t groups = 99;
    REQUIRE(sonare_mixer_vca_group_count(mixer, &groups) == SONARE_OK);
    REQUIRE(sonare_last_error_code() == SONARE_OK);
    REQUIRE(std::string(sonare_last_error_message()).empty());

    sonare_mixer_destroy(mixer);
  }
#endif  // defined(SONARE_WITH_MIXING)

#if defined(SONARE_WITH_MASTERING)
  SECTION("mastering constructors record a code on validation failure") {
    SonareEq* refused_eq = nullptr;
    REQUIRE(sonare_eq_create(0, 128, &refused_eq) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_eq == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    SonareStreamingMasteringChain* refused_chain = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create("{", nullptr, &refused_chain) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_chain == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }
#endif  // defined(SONARE_WITH_MASTERING)

  SECTION("the retune and sample bank constructors record a code") {
#if defined(SONARE_WITH_VOICE_CHANGER)
    SonareStreamingRetune* refused_retune = nullptr;
    REQUIRE(sonare_streaming_retune_create(std::numeric_limits<float>::quiet_NaN(), 1.0f, 0,
                                           &refused_retune) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_retune == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
#else
    SonareStreamingRetune* refused_retune = nullptr;
    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &refused_retune) ==
            SONARE_ERROR_NOT_SUPPORTED);
    REQUIRE(refused_retune == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
#endif  // defined(SONARE_WITH_VOICE_CHANGER)
    SonareSampleBank* bank = nullptr;
    REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
#if defined(SONARE_WITH_ARRANGEMENT)
    REQUIRE(bank != nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_OK);
    sonare_sample_bank_destroy(bank);
#else
    REQUIRE(bank == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_NOT_SUPPORTED);
#endif  // defined(SONARE_WITH_ARRANGEMENT)
  }
}

TEST_CASE("sonare_last_error_message is per-thread across concurrent failures", "[c_api]") {
  // The two threads are sequenced through `phase`, so each read happens after
  // the other thread's most recent write: with shared storage a thread reads
  // the other's message, with thread-local storage it reads its own.
  const auto provoke_missing_file = [] {
    SonareAudio* audio = nullptr;
    const SonareError err =
        sonare_audio_from_file("sonare-no-such-file-thread-probe.wav", nullptr, &audio);
    CHECK(err != SONARE_OK);
    CHECK(audio == nullptr);
    return std::string(sonare_last_error_message());
  };
  const auto provoke_bad_format = [] {
    const std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                          0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    SonareAudio* audio = nullptr;
    const SonareError err = sonare_audio_from_memory(garbage.data(), garbage.size(), &audio);
    CHECK(err != SONARE_OK);
    CHECK(audio == nullptr);
    return std::string(sonare_last_error_message());
  };

  // Preconditions: without two distinct non-empty messages the isolation
  // assertions below cannot distinguish shared storage from thread-local.
  const std::string file_message = provoke_missing_file();
  const std::string format_message = provoke_bad_format();
  REQUIRE_FALSE(file_message.empty());
  REQUIRE_FALSE(format_message.empty());
  REQUIRE(file_message != format_message);

  std::atomic<int> phase{0};
  const auto await = [&phase](int target) {
    while (phase.load(std::memory_order_acquire) < target) std::this_thread::yield();
  };

  std::string file_thread_own;
  std::string file_thread_observed;
  std::string format_thread_own;
  std::string format_thread_observed;

  std::thread file_thread([&] {
    file_thread_own = provoke_missing_file();
    phase.store(1, std::memory_order_release);
    await(2);  // The other thread has since recorded its own message.
    file_thread_observed = sonare_last_error_message();
    provoke_missing_file();
    phase.store(3, std::memory_order_release);
  });

  std::thread format_thread([&] {
    await(1);
    format_thread_own = provoke_bad_format();
    phase.store(2, std::memory_order_release);
    await(3);  // The other thread has since re-recorded its own message.
    format_thread_observed = sonare_last_error_message();
  });

  file_thread.join();
  format_thread.join();

  CHECK(file_thread_own == file_message);
  CHECK(format_thread_own == format_message);
  CHECK(file_thread_observed == file_message);
  CHECK(format_thread_observed == format_message);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine_parameter_info describes a hosted instrument's reserved parameter",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  REQUIRE(set_synth_patch(engine, 1, &patch) == SONARE_OK);

  uint32_t id = 0;
  REQUIRE(sonare_engine_resolve_instrument_automation_id(engine, 1, "cutoffHz", &id) == SONARE_OK);

  SonareParameterInfo info{};
  REQUIRE(sonare_engine_parameter_info(engine, id, &info) == SONARE_OK);
  CHECK(std::strcmp(info.name, "cutoffHz") == 0);
  CHECK(std::strcmp(info.unit, "Hz") == 0);
  CHECK(info.min_value == 10.0f);
  CHECK(info.max_value == 22000.0f);
  CHECK(info.default_value == 12000.0f);  // NativeSynthPatch{}.cutoff_hz
  CHECK(info.rt_safe == 1);

  // A slot no resolve call has ever minted stays unresolvable.
  const uint32_t unassigned = 0xC0000000u | (31u << 16u);
  REQUIRE(sonare_engine_parameter_info(engine, unassigned, &info) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_synth_instrument follows GM program changes only when asked",
          "[c_api][engine]") {
  constexpr uint32_t kDestination = 1;
  constexpr int kBlock = 128;
  constexpr size_t kFrames = 4096;

  // A program change one sample before the note-on so the resolved patch is
  // settled before note_on() reads the channel's program/bank state.
  auto render = [](uint8_t use_gm_programs, uint8_t program) -> std::vector<float> {
    SonareEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000, kBlock, 16, 16, 0) == SONARE_OK);

    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    std::strncpy(patch.preset, "sawLead", sizeof(patch.preset) - 1);

    SonareSynthInstrumentBinding binding{};
    binding.destination_id = kDestination;
    binding.patch = patch;
    binding.use_gm_programs = use_gm_programs;
    binding.sample_bank = nullptr;
    REQUIRE(sonare_engine_set_synth_instrument(engine, &binding) == SONARE_OK);

    const SonareEngineMidiEvent events[] = {
        {0, midi1_word(0xC, 0, program, 0), 0, 0, 0, 1, 0, 0, 0},
        {1, midi1_word(0x9, 0, 60, 100), 0, 0, 0, 1, 0, 0, 0},
    };
    SonareEngineMidiClipSchedule clip;
    sonare_engine_midi_clip_schedule_init(&clip);
    clip.gain = 1.0f;
    clip.id = 1;
    clip.track_id = kDestination;
    clip.length_samples = static_cast<int64_t>(kFrames);
    clip.destination_id = kDestination;
    clip.events = events;
    clip.event_count = 2;
    REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
    REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

    std::vector<float> left(kFrames, 0.0f);
    std::vector<float> right(kFrames, 0.0f);
    for (size_t at = 0; at < kFrames; at += kBlock) {
      float* channels[] = {left.data() + at, right.data() + at};
      REQUIRE(sonare_engine_process(engine, channels, nullptr, 2, kBlock) == SONARE_OK);
    }
    sonare_engine_destroy(engine);
    return left;
  };

  const std::vector<float> gm_piano = render(1, 0);
  const std::vector<float> gm_violin = render(1, 40);
  REQUIRE(peak_abs(gm_piano) > 0.0f);
  REQUIRE(peak_abs(gm_violin) > 0.0f);
  float max_diff = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) {
    max_diff = std::max(max_diff, std::abs(gm_piano[i] - gm_violin[i]));
  }
  REQUIRE(max_diff > 1e-4f);

  const std::vector<float> fixed_piano = render(0, 0);
  const std::vector<float> fixed_violin = render(0, 40);
  REQUIRE(fixed_piano == fixed_violin);

  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.patch = patch;
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_set_synth_instrument(nullptr, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_synth_instrument(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine_parameter_info describes a compressor insert on every strip kind",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  // Embedded-object params (the writer's own form, scene_json.cpp) rather than
  // the legacy escaped-string form, so the compressor's quotes need no escaping.
  constexpr const char* kEq = R"({"slot":"pre","processor":"eq.parametric","params":{}})";
  constexpr const char* kCompressorPre =
      R"({"slot":"pre","processor":"dynamics.compressor",)"
      R"("params":{"thresholdDb":-18,"ratio":2,"attackMs":10,"releaseMs":100,"kneeDb":0}})";
  constexpr const char* kCompressorPost =
      R"({"slot":"post","processor":"dynamics.compressor",)"
      R"("params":{"thresholdDb":-18,"ratio":2,"attackMs":10,"releaseMs":100,"kneeDb":0}})";
  // A leading pre-fader EQ puts the compressor at insert index 1 on the
  // track/master strips, so the id decode is exercised on a non-zero index
  // and not just the degenerate single-insert case.
  const std::string track_json = std::string(R"({"version":2,"strips":[{"id":"s","inserts":[)") +
                                 kEq + "," + kCompressorPost +
                                 R"(]}],"buses":[],"connections":[]})";
  const std::string master_json =
      std::string(R"({"version":2,"strips":[{"id":"master","inserts":[)") + kEq + "," +
      kCompressorPost + R"(]}],"buses":[],"connections":[]})";
  // sonare_engine_set_bus_strip_json reads scene.buses[0], not scene.strips[0].
  const std::string bus_json =
      std::string(R"({"version":2,"strips":[],"buses":[{"id":"1","inserts":[)") + kCompressorPre +
      R"(]}],"connections":[]})";

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, track_json.c_str()) == SONARE_OK);

  SonareEngineBus buses[] = {{1, 0.0f, 0, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_json.c_str()) == SONARE_OK);

  REQUIRE(sonare_engine_set_master_strip_json(engine, master_json.c_str()) == SONARE_OK);

  uint32_t track_id = 0;
  uint32_t bus_id = 0;
  uint32_t master_id = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 1, "thresholdDb",
                                                           &track_id) == SONARE_OK);
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "thresholdDb", &bus_id) ==
          SONARE_OK);
  REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 1, "thresholdDb", &master_id) ==
          SONARE_OK);

  // Ground truth for the entry the three ids above must all agree with,
  // read directly off the catalog rather than through parameter_info.
  const auto catalog = sonare::util::json::parse(
      sonare::mastering::api::insert_param_info_json("dynamics.compressor"));
  const sonare::util::json::Value* threshold = nullptr;
  for (const auto& entry : catalog.as_array()) {
    if (entry["name"].as_string() == "thresholdDb") {
      threshold = &entry;
      break;
    }
  }
  REQUIRE(threshold != nullptr);
  const std::string expected_unit =
      (*threshold)["unit"].is_null() ? std::string() : (*threshold)["unit"].as_string();
  const float expected_min = (*threshold)["min"].is_null() ? -std::numeric_limits<float>::infinity()
                                                           : (*threshold)["min"].as_float();
  const float expected_max = (*threshold)["max"].is_null() ? std::numeric_limits<float>::infinity()
                                                           : (*threshold)["max"].as_float();
  const float expected_default =
      (*threshold)["default"].is_null() ? 0.0f : (*threshold)["default"].as_float();

  for (uint32_t id : {track_id, bus_id, master_id}) {
    SonareParameterInfo info{};
    REQUIRE(sonare_engine_parameter_info(engine, id, &info) == SONARE_OK);
    CHECK(std::strcmp(info.name, "thresholdDb") == 0);
    CHECK(std::strcmp(info.unit, expected_unit.c_str()) == 0);
    CHECK(info.min_value == expected_min);
    CHECK(info.max_value == expected_max);
    CHECK(info.default_value == expected_default);
  }

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_parameter_info describes the lane fader and master width targets",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

  SonareParameterInfo fader{};
  REQUIRE(sonare_engine_parameter_info(engine, engine_track_lane_target(engine, 10, "faderDb"),
                                       &fader) == SONARE_OK);
  CHECK(std::strcmp(fader.name, "faderDb") == 0);
  CHECK(std::strcmp(fader.unit, "dB") == 0);
  CHECK(fader.min_value == sonare::constants::kFloorDb);
  CHECK(fader.max_value == 24.0f);
  CHECK(fader.default_value == 0.0f);

  SonareParameterInfo width{};
  REQUIRE(sonare_engine_parameter_info(
              engine, engine_master_param_target(sonare::engine::MixingRuntime::kWidth), &width) ==
          SONARE_OK);
  CHECK(std::strcmp(width.name, "width") == 0);
  CHECK(width.min_value == 0.0f);
  CHECK(width.max_value == 2.0f);
  CHECK(width.default_value == 1.0f);

  // Selector 0 is track 10's fader/pan entry, whose param 0 is neither, and no
  // strip was ever set, so the id names nothing.
  SonareParameterInfo missing{};
  REQUIRE(sonare_engine_parameter_info(engine, 0xE0000000u, &missing) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}
#endif  // defined(SONARE_WITH_MIXING)

TEST_CASE("sonare_engine_upsert_clip and remove_clip change one clip and keep the rest",
          "[c_api][engine]") {
  constexpr int kFrames = 64;
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, kFrames, 16, 16, 0) == SONARE_OK);

  std::array<float, kFrames> quarter{};
  std::array<float, kFrames> half{};
  std::array<float, kFrames> eighth{};
  quarter.fill(0.25f);
  half.fill(0.5f);
  eighth.fill(0.125f);
  const float* quarter_ch[] = {quarter.data()};
  const float* half_ch[] = {half.data()};
  const float* eighth_ch[] = {eighth.data()};
  const auto make_clip = [](uint32_t id, const float* const* channels) {
    SonareEngineClip clip;
    sonare_engine_clip_init(&clip);
    clip.id = id;
    clip.channels = channels;
    clip.num_channels = 1;
    clip.num_samples = kFrames;
    clip.gain = 1.0f;
    return clip;
  };
  const SonareEngineClip initial[] = {make_clip(1, quarter_ch), make_clip(2, half_ch)};
  REQUIRE(sonare_engine_set_clips(engine, initial, 2) == SONARE_OK);

  const auto count = [&] {
    size_t n = 0;
    REQUIRE(sonare_engine_clip_count(engine, &n) == SONARE_OK);
    return n;
  };
  const auto first_sample = [&] {
    REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
    std::array<float, kFrames> out{};
    float* io[] = {out.data()};
    REQUIRE(sonare_engine_render_offline(engine, io, 1, kFrames, kFrames, nullptr) == SONARE_OK);
    return out[0];
  };
  REQUIRE(first_sample() == Catch::Approx(0.75f));

  SonareEngineClip louder = make_clip(1, quarter_ch);
  louder.gain = 2.0f;
  REQUIRE(sonare_engine_upsert_clip(engine, &louder) == SONARE_OK);
  REQUIRE(count() == 2);
  REQUIRE(first_sample() == Catch::Approx(1.0f));

  const SonareEngineClip added = make_clip(3, eighth_ch);
  REQUIRE(sonare_engine_upsert_clip(engine, &added) == SONARE_OK);
  REQUIRE(count() == 3);
  REQUIRE(sonare_engine_remove_clip(engine, 2) == SONARE_OK);
  REQUIRE(count() == 2);
  REQUIRE(first_sample() == Catch::Approx(0.625f));

  // A refused change leaves the published set alone.
  REQUIRE(sonare_engine_remove_clip(engine, 2) == SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineClip invalid = make_clip(1, quarter_ch);
  invalid.gain = -1.0f;
  REQUIRE(sonare_engine_upsert_clip(engine, &invalid) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_upsert_clip(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(count() == 2);
  REQUIRE(first_sample() == Catch::Approx(0.625f));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_prepare reports the whole-hertz rate it prepared", "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 44100, 128, 8, 8, 0) == SONARE_OK);
  SonareTransportState state{};
  REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
  REQUIRE(state.sample_rate == 44100);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_prepare reads 0 as the library default for every sized argument",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 0, 0, 0, 0) == SONARE_OK);

  // 0 resolves to a 128-frame block and 64 channels: a 128 block renders, 129 does not.
  constexpr int kChannels = 64;
  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(256, 0.0f));
  std::vector<float*> planes;
  for (auto& plane : storage) planes.push_back(plane.data());
  REQUIRE(sonare_engine_render_offline(engine, planes.data(), kChannels, 128, 128, nullptr) ==
          SONARE_OK);
  REQUIRE(sonare_engine_render_offline(engine, planes.data(), kChannels, 129, 129, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  planes.push_back(nullptr);
  REQUIRE(sonare_engine_render_offline(engine, planes.data(), kChannels + 1, 128, 128, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A negative block or channel count is still refused.
  REQUIRE(sonare_engine_prepare(engine, 48000, -1, 0, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 48000, 0, 0, 0, -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 48000, 0, 0, 0, 0) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline resolves the automatic block against the prepared block",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 64, 8, 8, 0) == SONARE_OK);

  SonareEngineBounceOptions options;
  sonare_engine_bounce_options_init(&options);
  REQUIRE(options.struct_size == sizeof(SonareEngineBounceOptions));
  REQUIRE(options.block_size == SONARE_ENGINE_BLOCK_SIZE_AUTO);
  options.total_frames = 256;

  // num_channels is required: the default-filled struct is refused until it is set.
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  sonare_free_engine_bounce_result(&result);

  options.num_channels = 2;
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.frames == 256);
  sonare_free_engine_bounce_result(&result);

  for (const int bad_block : {0, -2}) {
    options.block_size = bad_block;
    REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_engine_bounce_result(&result);
  }
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_drain_external_midi accepts max_events 0 as a no-op", "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 8, 8, 0) == SONARE_OK);

  size_t count = 789;
  REQUIRE(sonare_engine_drain_external_midi(engine, nullptr, 0, &count) == SONARE_OK);
  REQUIRE(count == 0);

  SonareExternalMidiEvent events[2] = {};
  count = 789;
  REQUIRE(sonare_engine_drain_external_midi(engine, events, 2, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine transport refuses a negative position and never moves the playhead",
          "[c_api][engine]") {
  constexpr int kFrames = 64;
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, kFrames, 16, 16, 0) == SONARE_OK);
  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  float* io[] = {left.data(), right.data()};
  const auto position = [&] {
    SonareTransportState state{};
    REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
    return state.sample_position;
  };

  REQUIRE(sonare_engine_seek_sample(engine, 4800, -1) == SONARE_OK);
  REQUIRE(sonare_engine_process(engine, io, nullptr, 2, kFrames) == SONARE_OK);
  const int64_t before = position();
  REQUIRE(before == 4800);
  for (int64_t negative : {int64_t{-1}, int64_t{-48000}, std::numeric_limits<int64_t>::min()}) {
    CAPTURE(negative);
    REQUIRE(sonare_engine_seek_sample(engine, negative, -1) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_engine_process(engine, io, nullptr, 2, kFrames) == SONARE_OK);
    REQUIRE(position() == before);
  }
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_count_in_end_sample answers start_sample for a count of no bars",
          "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 8, 8, 0) == SONARE_OK);
  for (int bars : {0, -1, -4}) {
    for (int64_t start : {int64_t{0}, int64_t{12345}}) {
      CAPTURE(bars, start);
      int64_t end = -7;
      REQUIRE(sonare_engine_count_in_end_sample(engine, start, bars, &end) == SONARE_OK);
      REQUIRE(end == start);
    }
  }
  int64_t end = 0;
  REQUIRE(sonare_engine_count_in_end_sample(engine, 0, 1, &end) == SONARE_OK);
  REQUIRE(end > 0);
  REQUIRE(sonare_engine_count_in_end_sample(engine, -1, 0, &end) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_count_in_end_sample(engine, -1, 2, &end) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_capture_punch refuses an enabled empty window",
          "[c_api][engine][capture]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 8, 8, 0) == SONARE_OK);
  SonareEngineCaptureStatus status{};

  REQUIRE(sonare_engine_set_capture_punch(engine, 64, 128, 1) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.punch_enabled == 1);

  // Refused rather than stored as "punch off", which would record the whole timeline.
  REQUIRE(sonare_engine_set_capture_punch(engine, 256, 256, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.punch_enabled == 1);

  // An empty window that is not enabled is simply "punch off".
  REQUIRE(sonare_engine_set_capture_punch(engine, 256, 256, 0) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.punch_enabled == 0);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_capture_timing checks the offset and punch it produces together",
          "[c_api][engine][capture]") {
  constexpr int kRate = 48000;
  constexpr int64_t kLoopSamples = 100;
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kRate, 64, 8, 8, 0) == SONARE_OK);
  // At 60 bpm one quarter note is one second, so the loop is kLoopSamples long.
  REQUIRE(sonare_engine_set_tempo(engine, 60.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_loop(engine, 0.0, static_cast<double>(kLoopSamples) / kRate, 1) ==
          SONARE_OK);
  std::vector<float> plane(1000, 0.0f);
  float* channels[1] = {plane.data()};
  const SonareEngineCaptureBuffer buffer{channels, 1, static_cast<int64_t>(plane.size())};
  REQUIRE(sonare_engine_set_capture_buffer(engine, &buffer) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_punch(engine, 0, kLoopSamples, 0) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);
  // With the punch off the offset has no effect, so a long one is kept.
  REQUIRE(sonare_engine_set_record_offset_samples(engine, kLoopSamples + 1) == SONARE_OK);
  SonareEngineCaptureStatus status{};

  SECTION("a setup valid as a whole is accepted whatever the separate setters would refuse") {
    REQUIRE(sonare_engine_set_capture_punch(engine, 0, kLoopSamples, 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_engine_set_capture_timing(engine, 40, 0, kLoopSamples, 1) == SONARE_OK);
    REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
    CHECK(status.record_offset_samples == 40);
    CHECK(status.punch_enabled == 1);
    CHECK(status.armed == 1);
  }

  SECTION("a refused setup changes neither half") {
    REQUIRE(sonare_engine_set_capture_timing(engine, kLoopSamples + 1, 10, kLoopSamples, 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message())
              .find("record_offset_samples must be at most the loop length, 100 samples, while "
                    "capture is armed with punch on") != std::string::npos);
    REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
    CHECK(status.record_offset_samples == kLoopSamples + 1);
    CHECK(status.punch_enabled == 0);
    CHECK(status.armed == 1);
  }

  SECTION("the punch window is refused by its own names") {
    CHECK(sonare_engine_set_capture_timing(engine, 0, -1, kLoopSamples, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find("punch_start_sample") != std::string::npos);
    CHECK(sonare_engine_set_capture_timing(engine, 0, 50, 40, 0) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message())
              .find("punch_end_sample must not be before punch_start_sample") != std::string::npos);
    CHECK(sonare_engine_set_capture_timing(engine, 0, 50, 50, 1) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message())
              .find("punch_end_sample must not equal punch_start_sample") != std::string::npos);
    CHECK(sonare_engine_set_capture_timing(nullptr, 0, 0, 1, 1) == SONARE_ERROR_INVALID_PARAMETER);
  }

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_GRAPH)
TEST_CASE("sonare_engine_set_graph refuses a connection mix outside 0 and 1", "[c_api][engine]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 64, 8, 8, 0) == SONARE_OK);
  SonareEngineGraphNode nodes[2];
  for (auto& element : nodes) sonare_engine_graph_node_init(&element);
  std::strncpy(nodes[0].id, "in", sizeof(nodes[0].id) - 1);
  std::strncpy(nodes[1].id, "out", sizeof(nodes[1].id) - 1);
  nodes[0].num_ports = 1;
  nodes[1].num_ports = 1;
  SonareEngineGraphConnection connection{};
  std::strncpy(connection.source_node, "in", sizeof(connection.source_node) - 1);
  std::strncpy(connection.dest_node, "out", sizeof(connection.dest_node) - 1);
  SonareEngineGraphSpec spec{};
  spec.nodes = nodes;
  spec.node_count = 2;
  spec.connections = &connection;
  spec.connection_count = 1;
  std::strncpy(spec.input_node, "in", sizeof(spec.input_node) - 1);
  std::strncpy(spec.output_node, "out", sizeof(spec.output_node) - 1);
  spec.num_channels = 1;
  for (int mix : {-1, 2, 7}) {
    CAPTURE(mix);
    connection.mix = mix;
    REQUIRE(sonare_engine_set_graph(engine, &spec) == SONARE_ERROR_INVALID_PARAMETER);
  }
  for (int mix : {0, 1}) {
    CAPTURE(mix);
    connection.mix = mix;
    REQUIRE(sonare_engine_set_graph(engine, &spec) == SONARE_OK);
  }
  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("Create and prepare entries refuse a rate or block size outside their ceilings",
          "[c_api][engine][limits]") {
  constexpr int kBlockCeiling = 1 << 20;
#if defined(SONARE_WITH_MIXING)
  for (const int rate : {7999, 384001, 1000000000}) {
    CAPTURE(rate);
    SonareMixer* refused_mixer = nullptr;
    REQUIRE(sonare_mixer_create(rate, 128, &refused_mixer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_mixer == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }
  for (const int block : {-1, kBlockCeiling + 1, std::numeric_limits<int>::max()}) {
    CAPTURE(block);
    SonareMixer* refused_mixer = nullptr;
    REQUIRE(sonare_mixer_create(48000, block, &refused_mixer) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(refused_mixer == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(384000, kBlockCeiling, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  sonare_mixer_destroy(mixer);
#endif  // defined(SONARE_WITH_MIXING)

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  for (const int block : {kBlockCeiling + 1, std::numeric_limits<int>::max()}) {
    CAPTURE(block);
    REQUIRE(sonare_engine_prepare(engine, 48000, block, 1, 1, 0) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_engine_prepare(engine, 48000, block, 1, 1, 2) == SONARE_ERROR_INVALID_PARAMETER);
  }
  sonare_engine_destroy(engine);
}
