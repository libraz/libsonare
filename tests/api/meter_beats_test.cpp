#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "sonare_c_test_helpers.h"
#include "support/rate_material.h"
#include "util/constants.h"
#include "util/json.h"

namespace {

SonareMeterBeats initialized_meter_beats(const float* accent, size_t beat_count,
                                         const uint8_t* has_attack = nullptr,
                                         const float* low_frequency_energy = nullptr,
                                         const float* subdivision_accent = nullptr) {
  SonareMeterBeats beats{};
  sonare_meter_beats_init(&beats);
  beats.accent = accent;
  beats.has_attack = has_attack;
  beats.low_frequency_energy = low_frequency_energy;
  beats.subdivision_accent = subdivision_accent;
  beats.beat_count = beat_count;
  return beats;
}

}  // namespace

TEST_CASE("sonare_meter_beats_init writes an empty sized record", "[c_api][meter_beats]") {
  SonareMeterBeats beats{};
  beats.accent = reinterpret_cast<const float*>(static_cast<uintptr_t>(1));
  beats.has_attack = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(1));
  beats.low_frequency_energy = reinterpret_cast<const float*>(static_cast<uintptr_t>(1));
  beats.subdivision_accent = reinterpret_cast<const float*>(static_cast<uintptr_t>(1));
  beats.beat_count = 17;

  sonare_meter_beats_init(&beats);
  REQUIRE(beats.struct_size == sizeof(beats));
  REQUIRE(beats.accent == nullptr);
  REQUIRE(beats.has_attack == nullptr);
  REQUIRE(beats.low_frequency_energy == nullptr);
  REQUIRE(beats.subdivision_accent == nullptr);
  REQUIRE(beats.beat_count == 0);
  sonare_meter_beats_init(nullptr);
}

TEST_CASE("sonare_estimate_meter_json reads the supplied beat evidence", "[c_api][meter_beats]") {
  constexpr size_t kBeatCount = 24;
  std::vector<float> accent(kBeatCount);
  std::vector<float> subdivision(kBeatCount - 1);
  std::vector<float> no_subdivision(kBeatCount - 1, 0.0f);
  for (size_t i = 0; i < kBeatCount; ++i) {
    accent[i] = i % 3 == 0 ? 1.0f : 0.35f;
    if (i + 1 < kBeatCount) subdivision[i] = accent[i];
  }

  SonareMeterOptions options;
  sonare_meter_options_init(&options);
  const auto estimate = [&](const float* subdivision_accent) {
    SonareMeterBeats beats =
        initialized_meter_beats(accent.data(), accent.size(), nullptr, nullptr, subdivision_accent);
    char* json = nullptr;
    REQUIRE(sonare_estimate_meter_json(&beats, &options, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    return result;
  };

  const auto compound = estimate(subdivision.data());
  CHECK(compound["timeSignature"]["numerator"].as_number() == 6.0);
  CHECK(compound["timeSignature"]["denominator"].as_number() == 8.0);

  // A present all-zero stream means measured silence; an absent stream means
  // the estimator cannot measure subdivision evidence.
  const auto measured_zero = estimate(no_subdivision.data());
  CHECK(measured_zero["timeSignature"]["numerator"].as_number() == 3.0);
  CHECK(measured_zero["timeSignature"]["denominator"].as_number() == 4.0);
  const auto unmeasured = estimate(nullptr);
  CHECK(unmeasured["timeSignature"]["numerator"].as_number() == 3.0);
  CHECK(unmeasured["timeSignature"]["denominator"].as_number() == 4.0);

  const float masked_accent[] = {1.0f, 0.35f, 0.35f, 1.0f, 0.35f, 0.35f, 1.0f, 0.35f};
  const float beat_energy[] = {16.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  const uint8_t no_attack[] = {0, 1, 1, 1, 1, 1, 1, 1};
  SonareMeterBeats masked = initialized_meter_beats(masked_accent, 8, no_attack, beat_energy);
  char* json = nullptr;
  REQUIRE(sonare_estimate_meter_json(&masked, &options, &json) == SONARE_OK);
  const auto masked_result = sonare::util::json::parse(json);
  sonare_free_string(json);

  SonareMeterBeats all_attacks = initialized_meter_beats(masked_accent, 8, nullptr, beat_energy);
  json = nullptr;
  REQUIRE(sonare_estimate_meter_json(&all_attacks, &options, &json) == SONARE_OK);
  const auto all_attacks_result = sonare::util::json::parse(json);
  sonare_free_string(json);
  // A loud beat with no attack is masked and left out of scoring, which sharpens the 3/4 reading.
  CHECK(masked_result["timeSignature"]["numerator"].as_number() == 3.0);
  CHECK(all_attacks_result["timeSignature"]["numerator"].as_number() == 3.0);
  const double masked_confidence = masked_result["timeSignature"]["confidence"].as_number();
  const double attacked_confidence = all_attacks_result["timeSignature"]["confidence"].as_number();
  CAPTURE(masked_confidence, attacked_confidence);
  CHECK(masked_confidence > attacked_confidence + 0.05);
}

TEST_CASE("sonare_estimate_meter_json reproduces the meter in analysis JSON",
          "[.][slow][c_api][meter_beats]") {
  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  const auto audio =
      sonare::test::make_rate_material(sonare::test::RateMaterial::Clicks, kSampleRate);
  SonareMusicAnalyzeOptions analysis_options;
  sonare_music_analyze_options_init(&analysis_options);
  analysis_options.bpm_min = 100.0f;
  analysis_options.bpm_max = 140.0f;
  analysis_options.start_bpm = 120.0f;

  char* json = nullptr;
  REQUIRE(sonare_analyze_json(audio.data(), audio.size(), kSampleRate, &analysis_options, &json) ==
          SONARE_OK);
  REQUIRE(json != nullptr);
  const auto analyzed = sonare::util::json::parse(json);
  sonare_free_string(json);

  const auto& observations = analyzed["beatObservations"];
  const size_t beat_count = analyzed["beats"].as_array().size();
  REQUIRE(beat_count >= 8);
  const auto read_floats = [&](const char* name) {
    std::vector<float> result;
    for (const auto& value : observations[name].as_array()) result.push_back(value.as_float());
    return result;
  };
  const auto accent = read_floats("accent");
  const auto energy = read_floats("lowFrequencyEnergy");
  const auto subdivision = read_floats("subdivisionAccent");
  std::vector<uint8_t> attacks;
  for (const auto& value : observations["hasAttack"].as_array()) {
    REQUIRE(value.is_bool());
    attacks.push_back(value.as_bool());
  }
  REQUIRE(accent.size() == beat_count);
  REQUIRE(attacks.size() == beat_count);
  REQUIRE(energy.size() == beat_count);
  REQUIRE(subdivision.size() == beat_count - 1);

  SonareMeterOptions meter_options;
  sonare_meter_options_init(&meter_options);
  const auto beats = initialized_meter_beats(accent.data(), beat_count, attacks.data(),
                                             energy.data(), subdivision.data());
  json = nullptr;
  REQUIRE(sonare_estimate_meter_json(&beats, &meter_options, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const auto rescored = sonare::util::json::parse(json);
  sonare_free_string(json);
  CHECK(rescored["searched"].as_bool());
  const auto& expected = analyzed["timeSignature"];
  const auto& actual = rescored["timeSignature"];
  CHECK(actual["numerator"].as_int() == expected["numerator"].as_int());
  CHECK(actual["denominator"].as_int() == expected["denominator"].as_int());
  CHECK_THAT(actual["confidence"].as_number(),
             Catch::Matchers::WithinAbs(expected["confidence"].as_number(), 1.0e-6));
}

TEST_CASE("sonare_estimate_meter_json validates the input record", "[c_api][meter_beats]") {
  const float accent[] = {1.0f, 0.35f, 0.35f, 1.0f, 0.35f, 0.35f, 1.0f, 0.35f};
  const uint8_t invalid_attack[] = {1, 1, 1, 1, 2, 1, 1, 1};
  SonareMeterOptions options;
  sonare_meter_options_init(&options);
  const auto sentinel = []() { return reinterpret_cast<char*>(static_cast<uintptr_t>(1)); };

  SECTION("accepts the current and append-only larger record sizes") {
    SonareMeterBeats beats = initialized_meter_beats(accent, 8);
    for (uint32_t size :
         {static_cast<uint32_t>(sizeof(beats)), static_cast<uint32_t>(sizeof(beats) + 4)}) {
      beats.struct_size = size;
      char* json = nullptr;
      REQUIRE(sonare_estimate_meter_json(&beats, &options, &json) == SONARE_OK);
      REQUIRE(json != nullptr);
      sonare_free_string(json);
    }
  }

  SECTION("rejects a short or uninitialized record size") {
    SonareMeterBeats beats = initialized_meter_beats(accent, 8);
    for (uint32_t size : {0u, static_cast<uint32_t>(sizeof(beats) - 1)}) {
      beats.struct_size = size;
      char* json = sentinel();
      CHECK(sonare_estimate_meter_json(&beats, &options, &json) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(json == nullptr);
      CHECK(std::string(sonare_last_error_message()) ==
            "beats.struct_size must be set by sonare_meter_beats_init");
    }
  }

  SECTION("rejects invalid observations through the core validator") {
    SonareMeterBeats beats = initialized_meter_beats(accent, 8);
    beats.has_attack = invalid_attack;
    char* json = sentinel();
    CHECK(sonare_estimate_meter_json(&beats, &options, &json) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()).find("beats.has_attack") != std::string::npos);

    const float non_finite_accent[] = {
        1.0f, 0.35f, 0.35f, 1.0f, 0.35f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.35f};
    beats = initialized_meter_beats(non_finite_accent, 8);
    json = sentinel();
    CHECK(sonare_estimate_meter_json(&beats, &options, &json) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()).find("beats.accent") != std::string::npos);
  }

  SECTION("lets the core reject an empty series and clears output on every refusal") {
    SonareMeterBeats empty = initialized_meter_beats(nullptr, 0);
    char* json = sentinel();
    CHECK(sonare_estimate_meter_json(&empty, &options, &json) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()).find("beats.accent") != std::string::npos);
  }

  SECTION("refuses null required values and unrepresentable counts") {
    char* json = sentinel();
    CHECK(sonare_estimate_meter_json(nullptr, &options, &json) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()) == "beats must not be NULL");

    SonareMeterBeats no_accent = initialized_meter_beats(nullptr, 8);
    json = sentinel();
    CHECK(sonare_estimate_meter_json(&no_accent, &options, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()) ==
          "beats.accent must not be NULL when beats.beat_count is not 0");

    SonareMeterBeats oversized =
        initialized_meter_beats(accent, std::numeric_limits<size_t>::max());
    json = sentinel();
    CHECK(sonare_estimate_meter_json(&oversized, &options, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(std::string(sonare_last_error_message()).find("beats.beat_count") != std::string::npos);
  }
}
