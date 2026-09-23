/// @file sonare_c_engine_contract_test.cpp
/// @brief Engine C ABI contract: version, capabilities, feature-flag
///        reporting, malformed JSON and the per-thread last-error channel.

#include "sonare_c_engine_test_helpers.h"

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
  const char* json = sonare_capabilities_json();
  REQUIRE(json != nullptr);

  const auto capabilities = sonare::util::json::parse_strict(json);
  REQUIRE(capabilities["version"].as_string() == sonare_version());
  REQUIRE(capabilities["abi"]["project"].as_number() == SONARE_PROJECT_ABI_VERSION);
  REQUIRE(capabilities["abi"]["engine"].as_number() == sonare_engine_abi_version());
  REQUIRE(capabilities["features"]["ffmpeg"].as_bool() == (sonare_has_ffmpeg_support() != 0));
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
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

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
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

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
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  uint32_t dropped = 0xDEADBEEF;
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
  REQUIRE(sonare_engine_drain_external_midi(engine, events, 2, &drained) ==
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

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

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

TEST_CASE("sonare_engine_set_warp_voice_capacity enforces the documented ceiling",
          "[c_api][engine]") {
  // Drive the boundary from the core constant, same reasoning as the SysEx
  // ceiling test above: a change to the constant that leaves the define or the
  // header prose behind fails here and at the static_assert in
  // sonare_c_engine.cpp.
  constexpr uint32_t ceiling = sonare::engine::RealtimeEngine::kMaxWarpVoices;
  REQUIRE(ceiling == SONARE_ENGINE_MAX_WARP_VOICES);

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

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
  // The shared parser raises one JsonError; the entry points used to map it to
  // three different codes, including SONARE_ERROR_UNKNOWN. Syntactically
  // malformed input is SONARE_ERROR_INVALID_FORMAT everywhere, and never
  // UNKNOWN on any path.
  const char* malformed[] = {
      "not json", "{", "{\"a\":}", "{\"a\":1,}", "[1,", "", "{\"a\":1 \"b\":2}",
  };

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  for (const char* json : malformed) {
    INFO(json);
#if defined(SONARE_WITH_ARRANGEMENT)
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, json) == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_project_set_mixer_scene_json(project, json) == SONARE_ERROR_INVALID_FORMAT);
#endif
#if defined(SONARE_WITH_MIXING)
    REQUIRE(sonare_engine_set_master_strip_json(engine, json) == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_engine_set_track_strip_json(engine, 1, json) == SONARE_ERROR_INVALID_FORMAT);
#endif
#if defined(SONARE_WITH_MASTERING)
    SonareEq* eq = sonare_eq_create(48000.0, 512);
    REQUIRE(eq != nullptr);
    REQUIRE(sonare_eq_set_band(eq, 0, json) == SONARE_ERROR_INVALID_FORMAT);
    sonare_eq_destroy(eq);
#endif
  }

  sonare_project_destroy(project);
  sonare_engine_destroy(engine);
}

TEST_CASE("a scene JSON entry point separates a malformed document from an invalid value",
          "[c_api][engine][json]") {
  // INVALID_FORMAT means the document did not parse. A document that parses and
  // then fails a scene rule carries its own code, which is the difference
  // between "your file is corrupt" and "this value is not accepted".
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  const char* malformed = R"({"version":1,"strips":[)";
  const char* unsupported_version =
      R"({"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]})";
  const char* unknown_insert_slot =
      R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"nowhere","processor":"eq.parametric","params":"{}"}]}],"buses":[],"connections":[]})";

  REQUIRE(sonare_engine_set_master_strip_json(engine, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_master_strip_json(engine, unsupported_version) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_master_strip_json(engine, unknown_insert_slot) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_set_track_strip_json(engine, 1, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 1, unsupported_version) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, malformed) == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, unsupported_version) ==
          SONARE_ERROR_INVALID_PARAMETER);
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
    REQUIRE(std::string(sonare_last_error_message()).empty());

    record_detailed_error();
    REQUIRE(sonare_engine_prepare(nullptr, 48000.0, 128, 16, 16) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).empty());

#if defined(SONARE_WITH_ARRANGEMENT)
    record_detailed_error();
    REQUIRE(sonare_project_create(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).empty());
#endif  // defined(SONARE_WITH_ARRANGEMENT)
  }

  SECTION("pointer-returning constructors replace stale diagnostics") {
#if defined(SONARE_WITH_MIXING)
    REQUIRE(sonare_mixer_create(0, 128) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("sample_rate") != std::string::npos);
    REQUIRE(sonare_mixer_from_scene_json(nullptr, 48000, 128) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("scene JSON") != std::string::npos);
#endif  // defined(SONARE_WITH_MIXING)
#if defined(SONARE_WITH_MASTERING)
    REQUIRE(sonare_eq_create(0.0, 128) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("sample_rate") != std::string::npos);
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, 1) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()).find("params") != std::string::npos);
#endif  // defined(SONARE_WITH_MASTERING)
  }
}

TEST_CASE("sonare_last_error_message is per-thread across concurrent failures", "[c_api]") {
  // The two threads are sequenced through `phase`, so each read happens after
  // the other thread's most recent write: with shared storage a thread reads
  // the other's message, with thread-local storage it reads its own.
  const auto provoke_missing_file = [] {
    SonareAudio* audio = nullptr;
    const SonareError err = sonare_audio_from_file("sonare-no-such-file-thread-probe.wav", &audio);
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
