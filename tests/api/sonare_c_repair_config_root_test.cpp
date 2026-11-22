#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 22050;

std::vector<float> test_signal() {
  std::vector<float> samples(4096);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t);
  }
  return samples;
}

std::string last_error() { return sonare_last_error_message(); }

std::string first_subject() {
  const char* subject = sonare_last_refusal_subject(0);
  return subject == nullptr ? std::string() : std::string(subject);
}

}  // namespace

TEST_CASE("config struct members are refused under the config parameter", "[c_api][repair_root]") {
  const std::vector<float> samples = test_signal();

  SECTION("declick names threshold") {
    SonareDeclickConfig config;
    sonare_declick_config_init(&config);
    config.threshold = -1.0f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;
    SonareDeclickResult result{};
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), kSampleRate, &config,
                                            &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == "config.threshold must be finite and in (0, 10]");
    CHECK(first_subject() == "config.threshold");
  }

  SECTION("detect_clipping names clip_threshold_db") {
    SonareDeclipConfig config;
    sonare_declip_config_init(&config);
    config.clip_threshold_db = 1.0f;
    config.lpc_order = 36;
    config.iterations = 2;
    SonareClipDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), kSampleRate,
                                                    &config,
                                                    &detection) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == "config.clip_threshold_db must be finite and in [-890, 0]");
    CHECK(first_subject() == "config.clip_threshold_db");
  }

  SECTION("trim_silence names window_ms") {
    SonareTrimSilenceConfig config;
    sonare_trim_silence_config_init(&config);
    config.threshold_db = -60.0f;
    config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
    config.gate_lufs = -60.0f;
    config.window_ms = 0.0f;
    SonareTrimSilenceResult result{};
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), kSampleRate,
                                                 &config,
                                                 &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == "config.window_ms must be finite and in (0, 10000]");
    CHECK(first_subject() == "config.window_ms");
  }

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  SECTION("transcribe names min_note_division") {
    SonareTranscribeConfig config;
    sonare_transcribe_config_init(&config);
    config.min_note_division = 129;
    SonareTranscribeResult result{};
    REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                              &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == "config.min_note_division must be an integer in [1, 128]");
    CHECK(first_subject() == "config.min_note_division");
    sonare_free_transcribe_result(&result);
  }
#endif
}
