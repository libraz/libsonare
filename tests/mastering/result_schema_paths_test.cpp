#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/streaming_preview.h"
#include "support/schema_paths.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 22050;
constexpr std::size_t kLength = kSampleRate;

std::vector<float> tones(float low_hz, float high_hz, float amplitude) {
  std::vector<float> samples(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = amplitude * (0.6f * std::sin(sonare::constants::kTwoPi * low_hz * t) +
                              0.4f * std::sin(sonare::constants::kTwoPi * high_hz * t));
  }
  return samples;
}

std::set<std::string> as_set(const std::vector<std::string>& paths) {
  return std::set<std::string>(paths.begin(), paths.end());
}

// Drops everything below @p opaque_prefix, which a list publishes as one leaf.
std::set<std::string> without_descendants(std::set<std::string> paths,
                                          const std::string& opaque_prefix) {
  for (auto it = paths.begin(); it != paths.end();) {
    it = it->rfind(opaque_prefix + ".", 0) == 0 ? paths.erase(it) : std::next(it);
  }
  return paths;
}

}  // namespace

TEST_CASE("the assistant result schema list matches what the writer emits",
          "[mastering][assistant][schema]") {
  const auto samples = tones(220.0f, 1760.0f, 0.2f);
  const auto result =
      sonare::mastering::assistant::suggest_chain(samples.data(), samples.size(), kSampleRate);
  REQUIRE_FALSE(result.explanation.empty());
  const auto actual = without_descendants(
      sonare::test::schema_paths_of(sonare::mastering::assistant::assistant_result_to_json(result)),
      "chainConfig.params");
  REQUIRE(actual == as_set(sonare::mastering::assistant::assistant_result_schema_paths()));
}

TEST_CASE("the streaming preview schema list matches what the writer emits",
          "[mastering][maximizer][schema]") {
  const auto samples = tones(220.0f, 1760.0f, 0.2f);
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  const auto expected = as_set(sonare::mastering::maximizer::streaming_preview_schema_paths());
  REQUIRE(sonare::test::schema_paths_of(sonare::mastering::maximizer::streaming_preview_to_json(
              sonare::mastering::maximizer::streaming_preview(audio))) == expected);

  // A silent take serializes its non-finite loudness as null under the same keys.
  const std::vector<float> silence(kLength, 0.0f);
  const sonare::Audio quiet =
      sonare::Audio::from_buffer(silence.data(), silence.size(), kSampleRate);
  REQUIRE(sonare::test::schema_paths_of(sonare::mastering::maximizer::streaming_preview_to_json(
              sonare::mastering::maximizer::streaming_preview(quiet))) == expected);
}

TEST_CASE("each pair analysis schema list matches what its writer emits",
          "[mastering][match][schema]") {
  namespace api = sonare::mastering::api;
  const auto source = tones(220.0f, 1760.0f, 0.2f);
  const auto reference = tones(330.0f, 2200.0f, 0.4f);
  const auto paths_of = [&](const char* name) {
    return sonare::test::schema_paths_of(
        api::analyze_named_pair(name, source.data(), reference.data(), source.size(), kSampleRate));
  };

  REQUIRE(paths_of("match.referenceLoudness") ==
          as_set(api::match_reference_loudness_schema_paths()));
  REQUIRE(paths_of("match.tonalBalance") == as_set(api::match_tonal_balance_schema_paths()));
  REQUIRE(paths_of("match.tonalBalanceLogBands") ==
          as_set(api::match_tonal_balance_schema_paths()));
  REQUIRE(paths_of("match.matchEqCurve") == as_set(api::match_eq_curve_schema_paths()));
  REQUIRE(paths_of("match.estimateReferenceDelaySamples") ==
          as_set(api::match_reference_delay_schema_paths()));

  // A silent source writes its non-finite measurements as null under the same keys.
  const std::vector<float> silence(kLength, 0.0f);
  REQUIRE(sonare::test::schema_paths_of(api::analyze_named_pair(
              "match.referenceLoudness", silence.data(), reference.data(), silence.size(),
              kSampleRate)) == as_set(api::match_reference_loudness_schema_paths()));
}

TEST_CASE("each stereo analysis schema list matches what its writer emits",
          "[mastering][stereo][schema]") {
  namespace api = sonare::mastering::api;
  const auto left = tones(220.0f, 1760.0f, 0.2f);
  const auto right = tones(330.0f, 1760.0f, 0.2f);
  // The log-band default tops out at 20 kHz, above this fixture's Nyquist.
  const std::vector<api::Param> params = {{"highHz", 10000.0}};
  const auto paths_of = [&](const char* name, const std::vector<float>& second) {
    return sonare::test::schema_paths_of(api::analyze_named_stereo(
        name, left.data(), second.data(), left.size(), kSampleRate, params));
  };

  REQUIRE(paths_of("stereo.monoCompatCheck", right) ==
          as_set(api::stereo_mono_compat_schema_paths()));
  REQUIRE(paths_of("stereo.monoCompatCheckLogBands", right) ==
          as_set(api::stereo_mono_compat_log_bands_schema_paths()));

  // A fully out-of-phase pair has infinite width, written as null under the same key.
  std::vector<float> inverted(left);
  for (float& sample : inverted) sample = -sample;
  REQUIRE(paths_of("stereo.monoCompatCheck", inverted) ==
          as_set(api::stereo_mono_compat_schema_paths()));
}
