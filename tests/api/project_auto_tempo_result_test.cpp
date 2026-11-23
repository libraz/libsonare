#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "util/constants.h"

namespace {

std::vector<float> accented_clicks(int sample_rate) {
  constexpr double kBpm = 110.0;
  constexpr double kSeconds = 12.0;
  constexpr int kBeatsPerBar = 6;
  const auto sample_count = static_cast<size_t>(sample_rate * kSeconds);
  std::vector<float> samples(sample_count, 0.0f);
  const double beat_samples = sample_rate * sonare::constants::kSecondsPerMinute / kBpm;
  const auto click_samples = static_cast<size_t>(sample_rate * 0.01);
  for (int beat = 0; beat * beat_samples < static_cast<double>(sample_count); ++beat) {
    const auto start = static_cast<size_t>(std::lround(beat * beat_samples));
    const float gain = beat % kBeatsPerBar == 0 ? 1.0f : 0.5f;
    for (size_t frame = 0; frame < click_samples && start + frame < sample_count; ++frame) {
      samples[start + frame] =
          gain * static_cast<float>(std::sin(sonare::constants::kPiD * static_cast<double>(frame) /
                                             click_samples));
    }
  }
  return samples;
}

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message == nullptr ? "" : message;
}

struct StoredTimeSignatures {
  SonareProjectTimeSignatureList value{};

  StoredTimeSignatures() = default;
  ~StoredTimeSignatures() { sonare_free_project_time_signature_list(&value); }

  StoredTimeSignatures(const StoredTimeSignatures&) = delete;
  StoredTimeSignatures& operator=(const StoredTimeSignatures&) = delete;
};

}  // namespace

TEST_CASE("project auto tempo result reports selected tempo and detected meter",
          "[.][slow][c_api][project][tempo]") {
  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  const std::vector<float> samples = accented_clicks(kSampleRate);
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  sonare::MusicAnalyzer analyzer(audio);
  const sonare::TimeSignature analyzed_meter = analyzer.analyze().time_signature;

  SonareProject* raw_project = nullptr;
  REQUIRE(sonare_project_create(&raw_project) == SONARE_OK);
  const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> project(
      raw_project, &sonare_project_destroy);

  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t candidate_count = 0;
  REQUIRE(sonare_project_analyze_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                       nullptr, candidates, SONARE_PROJECT_MAX_TEMPO_CANDIDATES,
                                       &candidate_count) == SONARE_OK);
  REQUIRE(candidate_count > 0);

  SonareProjectAutoTempoResult without_applying_meter{};
  REQUIRE(sonare_project_auto_tempo_with_result(project.get(), samples.data(), samples.size(),
                                                kSampleRate, nullptr, 0, 0,
                                                &without_applying_meter) == SONARE_OK);
  CHECK(without_applying_meter.bpm == Catch::Approx(candidates[0].bpm).epsilon(0.01));
  CHECK(without_applying_meter.bpm_confidence ==
        Catch::Approx(candidates[0].confidence).epsilon(1.0e-6));
  CHECK(without_applying_meter.time_signature.numerator == analyzed_meter.numerator);
  CHECK(without_applying_meter.time_signature.denominator == analyzed_meter.denominator);
  CHECK(without_applying_meter.time_signature.confidence ==
        Catch::Approx(analyzed_meter.confidence).epsilon(1.0e-6));

  StoredTimeSignatures signatures;
  REQUIRE(sonare_project_time_signatures(project.get(), &signatures.value) == SONARE_OK);
  CHECK(signatures.value.items == nullptr);
  CHECK(signatures.value.count == 0);

  SonareProjectAutoTempoResult while_applying_meter{};
  REQUIRE(sonare_project_auto_tempo_with_result(project.get(), samples.data(), samples.size(),
                                                kSampleRate, nullptr, 0, 1,
                                                &while_applying_meter) == SONARE_OK);
  CHECK(while_applying_meter.bpm == Catch::Approx(without_applying_meter.bpm).epsilon(1.0e-6));
  CHECK(while_applying_meter.bpm_confidence ==
        Catch::Approx(without_applying_meter.bpm_confidence).epsilon(1.0e-6));
  CHECK(while_applying_meter.time_signature.numerator ==
        without_applying_meter.time_signature.numerator);
  CHECK(while_applying_meter.time_signature.denominator ==
        without_applying_meter.time_signature.denominator);
  CHECK(while_applying_meter.time_signature.confidence ==
        Catch::Approx(without_applying_meter.time_signature.confidence).epsilon(1.0e-6));
  REQUIRE(sonare_project_time_signatures(project.get(), &signatures.value) == SONARE_OK);
  CHECK(signatures.value.count == candidates[0].time_signature_count);
  CHECK(signatures.value.count > 0);
}

TEST_CASE("project auto tempo result refuses missing and malformed arguments by name",
          "[c_api][project][tempo]") {
  const std::vector<float> samples{0.0f};
  SonareProject* raw_project = nullptr;
  REQUIRE(sonare_project_create(&raw_project) == SONARE_OK);
  const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> project(
      raw_project, &sonare_project_destroy);

  CHECK(sonare_project_auto_tempo_with_result(project.get(), samples.data(), samples.size(),
                                              sonare::constants::kDefaultSampleRate, nullptr, 0, 0,
                                              nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  SonareProjectTempoOptions malformed_options{};
  SonareProjectAutoTempoResult result{120.0f, 1.0f, {4, 4, 1.0f}};
  CHECK(sonare_project_auto_tempo_with_result(
            project.get(), samples.data(), samples.size(), sonare::constants::kDefaultSampleRate,
            &malformed_options, 0, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_project_tempo_options_init");
  CHECK(result.bpm == 0.0f);
  CHECK(result.bpm_confidence == 0.0f);
  CHECK(result.time_signature.numerator == 0);
  CHECK(result.time_signature.denominator == 0);
  CHECK(result.time_signature.confidence == 0.0f);
}
