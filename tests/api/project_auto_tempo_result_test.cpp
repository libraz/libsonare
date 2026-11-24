#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "support/alloc_guard.h"
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

using Project = std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)>;

Project seeded_project() {
  SonareProject* raw = nullptr;
  REQUIRE(sonare_project_create(&raw) == SONARE_OK);
  Project project(raw, sonare_project_destroy);
  const SonareProjectTempoSegment tempo{0.0, 71.0, 0.0};
  const SonareProjectTimeSignatureSegment meter{0.0, 3, 8};
  REQUIRE(sonare_project_set_tempo_segments(project.get(), &tempo, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(project.get(), &meter, 1) == SONARE_OK);
  return project;
}

std::string serialized(const SonareProject* project) {
  char* document = nullptr;
  size_t size = 0;
  REQUIRE(sonare_project_serialize(project, &document, &size) == SONARE_OK);
  const std::unique_ptr<char, decltype(&sonare_free_string)> owned(document, sonare_free_string);
  return std::string(owned.get(), size);
}

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
  REQUIRE(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                    nullptr, 0, 0, &without_applying_meter) == SONARE_OK);
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
  REQUIRE(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                    nullptr, 0, int32_t{256}, &while_applying_meter) == SONARE_OK);
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

  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  SonareProjectTempoOptions malformed_options{};
  CHECK(sonare_project_auto_tempo(project.get(), nullptr, samples.size(), kSampleRate,
                                  &malformed_options, 0, 0,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "audio must be a non-empty buffer");
  CHECK(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                  &malformed_options, 0, 0,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_project_tempo_options_init");

  SonareProjectAutoTempoResult result{120.0f, 1.0f, {4, 4, 1.0f}};
  CHECK(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(),
                                  sonare::constants::kDefaultSampleRate, &malformed_options, 0, 0,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_project_tempo_options_init");
  CHECK(result.bpm == 0.0f);
  CHECK(result.bpm_confidence == 0.0f);
  CHECK(result.time_signature.numerator == 0);
  CHECK(result.time_signature.denominator == 0);
  CHECK(result.time_signature.confidence == 0.0f);
}

TEST_CASE("project tempo analysis refuses a short options struct before outputs",
          "[c_api][project][tempo]") {
  const auto project = seeded_project();
  const float sample = 0.0f;
  SonareProjectTempoOptions options{};
  options.struct_size = sizeof(options.struct_size);
  size_t count = 42;
  CHECK(sonare_project_analyze_tempo(project.get(), &sample, 1,
                                     sonare::constants::kDefaultSampleRate, &options, nullptr, 1,
                                     &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_project_tempo_options_init");
  CHECK(count == 0);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("project auto tempo rolls back when installing the meter fails",
          "[.][slow][c_api][project][tempo][allocation]") {
  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  const auto samples = accented_clicks(kSampleRate);
  const auto apply = [&](SonareProject* project, SonareProjectAutoTempoResult* out) {
    return sonare_project_auto_tempo(project, samples.data(), samples.size(), kSampleRate, nullptr,
                                     0, 1, out);
  };

  const auto measure = [&] {
    const auto project = seeded_project();
    SonareProjectAutoTempoResult result{};
    SonareError error;
    size_t count;
    {
      sonare::test::AllocationGuard guard;
      error = apply(project.get(), &result);
      count = guard.count();
    }
    REQUIRE(error == SONARE_OK);
    StoredTimeSignatures signatures;
    REQUIRE(sonare_project_time_signatures(project.get(), &signatures.value) == SONARE_OK);
    REQUIRE(signatures.value.count > 0);
    CHECK(result.bpm != Catch::Approx(71.0));
    return count;
  };

  (void)measure();
  const size_t allocations = measure();
  REQUIRE(allocations > 0);
  REQUIRE(measure() == allocations);

  const auto project = seeded_project();
  const std::string before = serialized(project.get());
  SonareProjectAutoTempoResult result{120.0f, 1.0f, {4, 4, 1.0f}};
  SonareError error;
  {
    // The final allocation copies the detected meter into the model, after the tempo edit.
    sonare::test::AllocationFailureAtGuard fail_meter(allocations);
    error = apply(project.get(), &result);
  }
  REQUIRE(error == SONARE_ERROR_OUT_OF_MEMORY);
  CHECK(result.bpm == 0.0f);
  CHECK(result.bpm_confidence == 0.0f);
  CHECK(result.time_signature.numerator == 0);
  CHECK(result.time_signature.denominator == 0);
  CHECK(result.time_signature.confidence == 0.0f);
  CHECK(serialized(project.get()) == before);

  REQUIRE(sonare_project_undo(project.get()) == SONARE_OK);
  StoredTimeSignatures signatures;
  REQUIRE(sonare_project_time_signatures(project.get(), &signatures.value) == SONARE_OK);
  CHECK(signatures.value.count == 0);
  REQUIRE(sonare_project_redo(project.get()) == SONARE_OK);
  CHECK(serialized(project.get()) == before);

  REQUIRE(apply(project.get(), &result) == SONARE_OK);
  CHECK(serialized(project.get()) != before);
  REQUIRE(sonare_project_undo(project.get()) == SONARE_OK);
  CHECK(serialized(project.get()) == before);
}
#endif

TEST_CASE("project auto tempo validates the ranked candidate before the output pointer",
          "[.][slow][c_api][project][tempo]") {
  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  const auto samples = accented_clicks(kSampleRate);
  const auto project = seeded_project();
  const std::string before = serialized(project.get());
  CHECK(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                  nullptr, SONARE_PROJECT_MAX_TEMPO_CANDIDATES, 1,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "candidate_index must be the index of a tempo candidate of this take");
  CHECK(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                  nullptr, 0, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(serialized(project.get()) == before);
}
