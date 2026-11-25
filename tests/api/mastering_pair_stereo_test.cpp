#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "mastering/api/named_processor.h"
#include "metering/lufs.h"
#include "sonare_c_test_helpers.h"
#include "util/json.h"

TEST_CASE("stereo pair processor admission needs no audio", "[c_api][mastering][stereo_pair]") {
  REQUIRE(sonare_validate_mastering_stereo_pair_processor("match.abCrossfade") == SONARE_OK);

  REQUIRE(sonare_validate_mastering_stereo_pair_processor("unknown") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "processor");
  CHECK_THAT(std::string(sonare_last_error_message()),
             Catch::Matchers::StartsWith("processor must be one of "));

  REQUIRE(sonare_validate_mastering_stereo_pair_processor("match.abSwitch") ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "processor");
  CHECK(std::string(sonare_last_error_message()) ==
        "processor does not support planar stereo pairs");
}

TEST_CASE("stereo pair processor admits the name before reading audio",
          "[c_api][mastering][stereo]") {
  SonareMasteringStereoResult out{};
  CHECK(sonare_mastering_apply_pair_processor_stereo("unknown", nullptr, nullptr, 0, nullptr,
                                                     nullptr, 0, 0, 0, nullptr, 0,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("processor must be one of") == 0);
  CHECK(sonare_mastering_apply_pair_processor_stereo("match.abSwitch", nullptr, nullptr, 0, nullptr,
                                                     nullptr, 0, 0, 0, nullptr, 0,
                                                     &out) == SONARE_ERROR_NOT_SUPPORTED);
  float samples[] = {0.0f, 0.1f};
  out.left = samples;
  out.right = samples;
  out.length = 2;
  CHECK(sonare_mastering_apply_pair_processor_stereo("match.abCrossfade", samples, samples, 2,
                                                     samples, samples, 2, 0, 48000, nullptr, 0,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("reference_sample_rate ") == 0);
  CHECK(out.left == nullptr);
  CHECK(out.right == nullptr);
  CHECK(out.length == 0);
}

TEST_CASE("stereo pair processor checks the audio before the parameter values",
          "[c_api][mastering][stereo]") {
  const float good[] = {0.0f, 0.1f};
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), 0.1f};
  const SonareMasteringParam params[] = {{"mix", std::numeric_limits<double>::quiet_NaN()}};
  SonareMasteringStereoResult out{};
  CHECK(sonare_mastering_apply_pair_processor_stereo("match.abCrossfade", bad, good, 2, good, good,
                                                     2, 48000, 48000, params, 1,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("source_left") == 0);
  CHECK_THROWS_WITH(sonare::mastering::api::apply_named_pair_processor_stereo(
                        "match.abCrossfade", good, good, 2, good, bad, 2, 48000, 48000,
                        {{"mix", std::numeric_limits<double>::quiet_NaN()}}),
                    Catch::Matchers::StartsWith("reference_right"));
  CHECK(sonare_mastering_apply_pair_processor_stereo("match.abCrossfade", good, good, 2, good, good,
                                                     2, 48000, 48000, params, 1,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("mix") != std::string::npos);
}

TEST_CASE("C mastering pair entries name the reference sample rate",
          "[c_api][mastering][pair_stereo]") {
  const float samples[] = {0.0f, 0.1f};
  const auto check_rate = [](SonareError status) {
    CHECK(status == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(std::string(sonare_last_error_message()),
               Catch::Matchers::StartsWith("reference_sample_rate "));
  };

  SonareMasteringResult result{};
  check_rate(sonare_mastering_apply_pair_processor("match.abCrossfade", samples, 2, samples, 2, 0,
                                                   48000, nullptr, 0, &result));
  char* analysis = nullptr;
  check_rate(sonare_mastering_analyze_pair("match.referenceLoudness", samples, 2, samples, 2, 0,
                                           48000, nullptr, 0, &analysis));
  check_rate(sonare_mastering_analyze_pair_stereo("match.referenceLoudness", samples, samples, 2,
                                                  samples, samples, 2, 0, 48000, nullptr, 0,
                                                  &analysis));

  float* left = nullptr;
  float* right = nullptr;
  size_t length = 0;
  check_rate(sonare_mastering_ab_match_loudness(samples, 2, samples, 2, 0, 48000, &left, &length,
                                                nullptr));
  check_rate(sonare_mastering_ab_match_loudness_stereo(samples, samples, 2, samples, samples, 2, 0,
                                                       48000, &left, &right, &length, nullptr));
}

TEST_CASE("C match EQ pair entries refuse a silent source or reference, naming it",
          "[c_api][mastering][pair_stereo]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> silent(static_cast<size_t>(kSampleRate), 0.0f);
  auto sounding = generate_sine(440.0f, kSampleRate, 1.0f);
  for (float& sample : sounding) sample *= 0.5f;
  const auto check_refusal = [](SonareError status, const std::string& role) {
    CHECK(status == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) == role + " has no measurable level");
  };

  const std::array<std::pair<const std::vector<float>*, const std::vector<float>*>, 2> takes{
      {{&silent, &sounding}, {&sounding, &silent}}};
  const std::array<const char*, 2> roles{"source", "reference"};
  for (size_t i = 0; i < takes.size(); ++i) {
    const auto& [source, reference] = takes[i];
    char* analysis = nullptr;
    check_refusal(sonare_mastering_analyze_pair(
                      "match.matchEqCurve", source->data(), source->size(), reference->data(),
                      reference->size(), kSampleRate, kSampleRate, nullptr, 0, &analysis),
                  roles[i]);
    CHECK(analysis == nullptr);
    SonareMasteringResult result{};
    check_refusal(sonare_mastering_apply_pair_processor(
                      "match.applyMatchEq", source->data(), source->size(), reference->data(),
                      reference->size(), kSampleRate, kSampleRate, nullptr, 0, &result),
                  roles[i]);
  }
}

TEST_CASE("C API pair analysis accepts planar stereo inputs", "[c_api][mastering][pair_stereo]") {
  constexpr int kSampleRate = 44100;
  auto source_left = generate_sine(440.0f, kSampleRate, 0.5f);
  auto source_right = generate_sine(660.0f, kSampleRate, 0.5f);
  auto reference_left = generate_sine(550.0f, 32000, 0.35f);
  auto reference_right = generate_sine(880.0f, 32000, 0.35f);
  for (float& sample : source_left) sample *= 0.15f;
  for (float& sample : source_right) sample *= 0.25f;
  for (float& sample : reference_left) sample *= 0.2f;
  for (float& sample : reference_right) sample *= 0.1f;

  const std::array<const char*, 5> analyses{"match.referenceLoudness", "match.tonalBalance",
                                            "match.tonalBalanceLogBands", "match.matchEqCurve",
                                            "match.estimateReferenceDelaySamples"};
  for (const char* analysis : analyses) {
    CAPTURE(analysis);
    char* json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair_stereo(
                analysis, source_left.data(), source_right.data(), source_left.size(),
                reference_left.data(), reference_right.data(), reference_left.size(), 32000,
                kSampleRate, nullptr, 0, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    REQUIRE(result.is_object());
    CHECK(result.size() > 0);

    if (std::string(analysis) == "match.referenceLoudness") {
      std::vector<float> interleaved;
      interleaved.reserve(source_left.size() * 2);
      for (std::size_t i = 0; i < source_left.size(); ++i) {
        interleaved.push_back(source_left[i]);
        interleaved.push_back(source_right[i]);
      }
      const double expected_lufs =
          sonare::metering::lufs_interleaved(interleaved.data(), source_left.size(), 2, kSampleRate)
              .integrated_lufs;
      CHECK_THAT(result["sourceLufs"].as_number(), Catch::Matchers::WithinAbs(expected_lufs, 0.01));
    }
  }

  // The existing mono entry remains valid for mono 1/1 inputs.
  char* mono_json = nullptr;
  REQUIRE(sonare_mastering_analyze_pair("match.referenceLoudness", source_left.data(),
                                        source_left.size(), reference_left.data(),
                                        reference_left.size(), 32000, kSampleRate, nullptr, 0,
                                        &mono_json) == SONARE_OK);
  REQUIRE(mono_json != nullptr);
  const auto mono_result = sonare::util::json::parse(mono_json);
  sonare_free_string(mono_json);
  const double expected_mono_lufs =
      sonare::metering::lufs_interleaved(source_left.data(), source_left.size(), 1, kSampleRate)
          .integrated_lufs;
  CHECK_THAT(mono_result["sourceLufs"].as_number(),
             Catch::Matchers::WithinAbs(expected_mono_lufs, 0.01));
}

TEST_CASE("C API duplicate-channel pair analysis agrees with mono",
          "[c_api][mastering][pair_stereo]") {
  constexpr int kSampleRate = 44100;
  constexpr double kTolerance = 1.0e-6;
  const auto source = generate_sine(440.0f, kSampleRate, 0.5f);
  const auto reference = generate_sine(880.0f, kSampleRate, 0.5f);

  const auto analyze_mono = [&](const char* name) {
    char* json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair(name, source.data(), source.size(), reference.data(),
                                          reference.size(), kSampleRate, kSampleRate, nullptr, 0,
                                          &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    return result;
  };
  const auto analyze_duplicate_stereo = [&](const char* name) {
    char* json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair_stereo(name, source.data(), source.data(), source.size(),
                                                 reference.data(), reference.data(),
                                                 reference.size(), kSampleRate, kSampleRate,
                                                 nullptr, 0, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    return result;
  };
  const auto compare_number = [=](const sonare::util::json::Value& mono_value,
                                  const sonare::util::json::Value& stereo_value) {
    if (mono_value.is_null()) {
      CHECK(stereo_value.is_null());
      return;
    }
    REQUIRE(mono_value.is_number());
    REQUIRE(stereo_value.is_number());
    CHECK_THAT(stereo_value.as_number(),
               Catch::Matchers::WithinAbs(mono_value.as_number(), kTolerance));
  };

  const auto mono_tonal = analyze_mono("match.tonalBalance");
  const auto stereo_tonal = analyze_duplicate_stereo("match.tonalBalance");
  const auto& mono_bands = mono_tonal["bands"].as_array();
  const auto& stereo_bands = stereo_tonal["bands"].as_array();
  REQUIRE_FALSE(mono_bands.empty());
  REQUIRE(stereo_bands.size() == mono_bands.size());
  for (std::size_t i = 0; i < mono_bands.size(); ++i) {
    CAPTURE(i);
    for (const char* key : {"lowHz", "highHz", "sourceDb", "referenceDb", "deviationDb"}) {
      CAPTURE(key);
      const auto* mono_value = mono_bands[i].find(key);
      const auto* stereo_value = stereo_bands[i].find(key);
      REQUIRE((mono_value == nullptr) == (stereo_value == nullptr));
      if (mono_value != nullptr) compare_number(*mono_value, *stereo_value);
    }
  }

  const auto mono_eq = analyze_mono("match.matchEqCurve");
  const auto stereo_eq = analyze_duplicate_stereo("match.matchEqCurve");
  for (const char* key : {"frequencies", "gainDb"}) {
    CAPTURE(key);
    const auto& mono_values = mono_eq[key].as_array();
    const auto& stereo_values = stereo_eq[key].as_array();
    REQUIRE_FALSE(mono_values.empty());
    REQUIRE(stereo_values.size() == mono_values.size());
    for (std::size_t i = 0; i < mono_values.size(); ++i) {
      CAPTURE(i);
      compare_number(mono_values[i], stereo_values[i]);
    }
  }
}

TEST_CASE("C API stereo pair delay matches the mono mid with its sign",
          "[c_api][mastering][pair_stereo]") {
  constexpr int kSampleRate = 44100;
  constexpr std::size_t kFrames = 128;
  constexpr std::size_t kShift = 13;
  constexpr std::size_t kPeakFrame = 11;
  constexpr double kTolerance = 1.0e-6;
  std::vector<float> source_left(kFrames), source_right(kFrames), source_mid(kFrames);
  std::vector<float> reference_left(kFrames + kShift), reference_right(kFrames + kShift);
  std::vector<float> reference_mid(kFrames + kShift);
  source_left[kPeakFrame] = 0.5f;
  source_left[26] = -0.25f;
  source_left[75] = 0.125f;
  source_right[46] = 0.5f;
  for (std::size_t i = 0; i < kFrames; ++i) {
    source_mid[i] = 0.5f * (source_left[i] + source_right[i]);
    reference_left[i + kShift] = source_right[i];
    reference_right[i + kShift] = source_left[i];
    reference_mid[i + kShift] = source_mid[i];
  }

  const auto analyze_stereo = [&](const float* source_l, const float* source_r,
                                  std::size_t source_length, const float* reference_l,
                                  const float* reference_r, std::size_t reference_length) {
    char* json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair_stereo("match.estimateReferenceDelaySamples", source_l,
                                                 source_r, source_length, reference_l, reference_r,
                                                 reference_length, kSampleRate, kSampleRate,
                                                 nullptr, 0, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    return result["delaySamples"].as_number();
  };
  const auto analyze_mono = [&](const float* source, std::size_t source_length,
                                const float* reference, std::size_t reference_length) {
    char* json = nullptr;
    REQUIRE(sonare_mastering_analyze_pair("match.estimateReferenceDelaySamples", source,
                                          source_length, reference, reference_length, kSampleRate,
                                          kSampleRate, nullptr, 0, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const auto result = sonare::util::json::parse(json);
    sonare_free_string(json);
    return result["delaySamples"].as_number();
  };

  const double stereo_forward =
      analyze_stereo(source_left.data(), source_right.data(), source_left.size(),
                     reference_left.data(), reference_right.data(), reference_left.size());
  const double mono_forward = analyze_mono(source_mid.data(), source_mid.size(),
                                           reference_mid.data(), reference_mid.size());
  CHECK_THAT(stereo_forward, Catch::Matchers::WithinAbs(kShift, kTolerance));
  CHECK_THAT(stereo_forward, Catch::Matchers::WithinAbs(mono_forward, kTolerance));

  const double stereo_reverse =
      analyze_stereo(reference_left.data(), reference_right.data(), reference_left.size(),
                     source_left.data(), source_right.data(), source_left.size());
  const double mono_reverse = analyze_mono(reference_mid.data(), reference_mid.size(),
                                           source_mid.data(), source_mid.size());
  CHECK_THAT(stereo_reverse, Catch::Matchers::WithinAbs(-static_cast<double>(kShift), kTolerance));
  CHECK_THAT(stereo_reverse, Catch::Matchers::WithinAbs(mono_reverse, kTolerance));
}

TEST_CASE("C API pair stereo analysis names refused inputs", "[c_api][mastering][pair_stereo]") {
  constexpr int kSampleRate = 44100;
  const auto left = generate_sine(440.0f, kSampleRate, 0.1f);
  const auto right = generate_sine(660.0f, kSampleRate, 0.1f);
  char* json = reinterpret_cast<char*>(static_cast<std::uintptr_t>(1));

  REQUIRE(sonare_mastering_analyze_pair_stereo("match.referenceLoudness", nullptr, right.data(),
                                               right.size(), left.data(), right.data(),
                                               right.size(), kSampleRate, kSampleRate, nullptr, 0,
                                               &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);
  CHECK_THAT(std::string(sonare_last_error_message()),
             Catch::Matchers::ContainsSubstring("source_left"));

  REQUIRE(sonare_mastering_analyze_pair_stereo("match.referenceLoudness", left.data(), right.data(),
                                               left.size(), left.data(), nullptr, left.size(),
                                               kSampleRate, kSampleRate, nullptr, 0,
                                               &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);
  CHECK_THAT(std::string(sonare_last_error_message()),
             Catch::Matchers::ContainsSubstring("reference_right"));

  REQUIRE(sonare_mastering_analyze_pair_stereo("match.referenceLoudness", left.data(), right.data(),
                                               left.size(), left.data(), right.data(), right.size(),
                                               0, kSampleRate, nullptr, 0,
                                               &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);
  CHECK_THAT(std::string(sonare_last_error_message()),
             Catch::Matchers::ContainsSubstring("reference_sample_rate"));

  REQUIRE(sonare_mastering_analyze_pair_stereo("match.referenceLoudness", left.data(), right.data(),
                                               left.size(), left.data(), right.data(), right.size(),
                                               kSampleRate, kSampleRate, nullptr, 0,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(std::string(sonare_last_error_message()), Catch::Matchers::ContainsSubstring("out"));
}
