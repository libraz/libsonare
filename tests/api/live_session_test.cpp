#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "c_api/sonare_c_internal.h"

namespace {

class Driver final : public sonare::engine::LiveDriver {
 public:
  explicit Driver(sonare::engine::RealtimeEngine& engine) : engine_(engine) {}

  void stop() noexcept override {
    stopped = true;
    engine_.detach_live_driver(this);
  }

  bool stopped = false;

 private:
  sonare::engine::RealtimeEngine& engine_;
};

}  // namespace

TEST_CASE("device-driven C engine refuses manual processing without changing last error",
          "[c_api][live]") {
  SonareEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  const std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)> engine(
      raw, sonare_engine_destroy);
  Driver driver(engine->engine);
  const std::unique_ptr<Driver, void (*)(Driver*)> attachment(&driver,
                                                              [](Driver* d) { d->stop(); });
  REQUIRE(sonare_engine_prepare(raw, 48000, 128, 0, 0, 2) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(raw, -1, 128, 0, 0, 2) == SONARE_ERROR_INVALID_PARAMETER);
  const std::string saved_message = sonare_last_error_message();
  REQUIRE(engine->engine.try_attach_live_driver(&driver));
  float sample = 1.0f;
  float* channels[] = {&sample};
  CHECK(sonare_engine_process(raw, channels, nullptr, 1, 1) == SONARE_ERROR_INVALID_STATE);
  CHECK(sample == 1.0f);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(saved_message == sonare_last_error_message());
  CHECK(sonare_engine_prepare(raw, 48000, 128, 0, 0, 2) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "engineDeviceDriven");
  CHECK(sonare_engine_set_tempo(raw, 120.0) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "engineDeviceDriven");
}

TEST_CASE("destroying a device-driven C engine stops its driver", "[c_api][live]") {
  SonareEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  Driver driver(raw->engine);
  std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)> engine(raw,
                                                                         sonare_engine_destroy);
  REQUIRE(engine->engine.try_attach_live_driver(&driver));
  engine.reset();
  CHECK(driver.stopped);
}

TEST_CASE("C live device lists and session entry points follow feature availability",
          "[c_api][live]") {
  SonareLiveSessionConfig config{};
  sonare_live_session_config_init(&config);
  CHECK(config.struct_size == sizeof(config));
  CHECK(config.block_size == sonare::engine::RealtimeEngine::kDefaultOfflineBlock);
  CHECK(config.audio_output == SONARE_LIVE_DEVICE_NONE);
  CHECK(config.audio_input == SONARE_LIVE_DEVICE_NONE);
  CHECK(config.midi_input == SONARE_LIVE_DEVICE_NONE);

  SonareLiveAudioDeviceList audio{};
  audio.count = 42;
  const auto audio_result = sonare_live_audio_devices(&audio);
  if (audio_result == SONARE_ERROR_NOT_SUPPORTED) {
    REQUIRE(sonare_last_error_reason() != nullptr);
    CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
    CHECK(audio.items == nullptr);
    CHECK(audio.count == 0);
    SonareLiveMidiDeviceList midi{};
    midi.count = 42;
    CHECK(sonare_live_midi_sources(&midi) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(midi.items == nullptr);
    CHECK(midi.count == 0);
    SonareLiveSession* session = nullptr;
    CHECK(sonare_live_session_create(nullptr, &config, &session) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(session == nullptr);
    SonareLiveSessionStatus status{};
    status.running = 1;
    CHECK(sonare_live_session_status(nullptr, &status) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(status.running == 0);
    SonareMidiEventList capture{};
    capture.count = 42;
    CHECK(sonare_live_session_captured_midi(nullptr, &capture) == SONARE_ERROR_NOT_SUPPORTED);
    CHECK(capture.items == nullptr);
    CHECK(capture.count == 0);
    return;
  }

  REQUIRE(audio_result == SONARE_OK);
  sonare_free_live_audio_device_list(&audio);
  CHECK(audio.items == nullptr);
  CHECK(audio.count == 0);
  SonareLiveMidiDeviceList midi{};
  REQUIRE(sonare_live_midi_sources(&midi) == SONARE_OK);
  sonare_free_live_midi_device_list(&midi);
  CHECK(midi.items == nullptr);
  CHECK(midi.count == 0);
}

TEST_CASE("C live device predicate agrees with the session create refusal", "[c_api][live]") {
  SonareLiveSessionConfig config{};
  sonare_live_session_config_init(&config);
  SonareLiveSession* session = nullptr;
  const auto result = sonare_live_session_create(nullptr, &config, &session);
  const char* reason = sonare_last_error_reason();
  const bool stub = result == SONARE_ERROR_NOT_SUPPORTED && reason != nullptr &&
                    std::string(reason) == "notCompiledIn";
  REQUIRE((stub || result == SONARE_ERROR_INVALID_PARAMETER));
  CHECK(sonare_has_live_devices() == (stub ? 0 : 1));
}

TEST_CASE("C live session refuses invalid settings before opening devices", "[c_api][live]") {
  SonareLiveAudioDeviceList audio{};
  const auto available = sonare_live_audio_devices(&audio);
  sonare_free_live_audio_device_list(&audio);
  if (available == SONARE_ERROR_NOT_SUPPORTED) SKIP("Live devices are disabled");
  REQUIRE(available == SONARE_OK);
  SonareEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  const std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)> engine(
      raw, sonare_engine_destroy);
  SonareLiveSessionConfig config{};
  sonare_live_session_config_init(&config);
  SonareLiveSession* session = nullptr;
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.audio_output") != std::string::npos);
  config.audio_output = SONARE_LIVE_DEVICE_BY_UID;
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.audio_output_uid") !=
        std::string::npos);
  config.audio_output = SONARE_LIVE_DEVICE_DEFAULT;
  config.midi_input = SONARE_LIVE_DEVICE_DEFAULT;
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.midi_input") != std::string::npos);
  config.midi_input = SONARE_LIVE_DEVICE_NONE;
  config.audio_output_uid = "unexpected";
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.audio_output_uid") !=
        std::string::npos);
  config.audio_output_uid = nullptr;
  config.sample_rate = -1;
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.sample_rate") != std::string::npos);
  config.sample_rate = 0;
  config.block_size = 0;
  CHECK(sonare_live_session_create(raw, &config, &session) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("config.block_size") != std::string::npos);
  CHECK(session == nullptr);
}

TEST_CASE("a C live session can outlive its engine", "[.][c_api][live][hardware]") {
  SonareLiveAudioDeviceList audio{};
  const auto available = sonare_live_audio_devices(&audio);
  if (available == SONARE_ERROR_NOT_SUPPORTED) SKIP("Live devices are disabled");
  REQUIRE(available == SONARE_OK);
  bool has_default_output = false;
  if (audio.items != nullptr) {
    has_default_output = std::any_of(audio.items, audio.items + audio.count, [](const auto& item) {
      return item.is_default_output && item.output_channels > 0;
    });
  }
  sonare_free_live_audio_device_list(&audio);
  if (!has_default_output) SKIP("No default audio output is available");

  SonareEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)> engine(raw,
                                                                         sonare_engine_destroy);
  SonareLiveSessionConfig config{};
  sonare_live_session_config_init(&config);
  config.audio_output = SONARE_LIVE_DEVICE_DEFAULT;
  SonareLiveSession* handle = nullptr;
  REQUIRE(sonare_live_session_create(raw, &config, &handle) == SONARE_OK);
  const std::unique_ptr<SonareLiveSession, decltype(&sonare_live_session_destroy)> session(
      handle, sonare_live_session_destroy);
  SonareLiveSession* second = nullptr;
  CHECK(sonare_live_session_create(raw, &config, &second) == SONARE_ERROR_INVALID_STATE);
  CHECK(second == nullptr);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "engineDeviceDriven");
  SonareLiveSessionStatus status{};
  REQUIRE(sonare_live_session_status(handle, &status) == SONARE_OK);
  REQUIRE(status.running == 1);
  engine.reset();
  REQUIRE(sonare_live_session_status(handle, &status) == SONARE_OK);
  CHECK(status.running == 0);
}
