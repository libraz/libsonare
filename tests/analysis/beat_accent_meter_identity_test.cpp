#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "analysis/meter_analyzer.h"
#include "analysis/music_analyzer.h"
#include "util/constants.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;

namespace {

template <typename Callable>
void require_meter_refusal(Callable&& call, const std::vector<std::string>& subjects) {
  try {
    call();
  } catch (const SonareParameterRefusal& error) {
    CHECK(error.code() == ErrorCode::InvalidParameter);
    CHECK(error.subjects() == subjects);
    return;
  }
  FAIL("expected a meter parameter refusal");
}

}  // namespace

TEST_CASE("Music analysis publishes the evidence used to score meter",
          "[.][slow][beat_accent_meter]") {
  constexpr float kBpm = 120.0f;
  constexpr int kBeatCount = 12;
  const int sample_rate =
      GENERATE(constants::kDefaultSampleRate, static_cast<int>(constants::kDefaultDawSampleRate));
  const int beat_samples = sample_rate / 2;
  const int click_samples = sample_rate / 100;
  std::vector<float> samples(kBeatCount * beat_samples);
  for (int beat = 0; beat < kBeatCount; ++beat) {
    const float level = beat % 3 == 0 ? 0.9f : 0.35f;
    for (int i = 0; i < click_samples; ++i) {
      samples[beat * beat_samples + i] = level * (1.0f - static_cast<float>(i) / click_samples);
    }
  }

  MusicAnalyzerConfig config;
  config.start_bpm = kBpm;
  MusicAnalyzer analyzer(Audio::from_vector(std::move(samples), sample_rate), config);
  const AnalysisResult analyzed = analyzer.analyze();
  const auto& obs = analyzed.beat_observations;
  REQUIRE(analyzed.beats.size() >= 8);
  REQUIRE(obs.accent.size() == analyzed.beats.size());
  REQUIRE(obs.has_attack.size() == analyzed.beats.size());
  REQUIRE(obs.low_frequency_energy.size() == analyzed.beats.size());
  REQUIRE(obs.subdivision_accent.size() == analyzed.beats.size() - 1);
  REQUIRE(std::any_of(obs.has_attack.begin(), obs.has_attack.end(),
                      [](auto attack) { return attack == 1; }));

  const MeterBeats observations{obs.accent, obs.has_attack, obs.low_frequency_energy,
                                obs.subdivision_accent};
  const MeterResult rescored = estimate_meter_from_observations(observations, MeterConfig{});
  REQUIRE(rescored.searched);
  CHECK(rescored.time_signature.numerator == analyzed.time_signature.numerator);
  CHECK(rescored.time_signature.denominator == analyzed.time_signature.denominator);
  CHECK_THAT(rescored.time_signature.confidence,
             WithinAbs(analyzed.time_signature.confidence, 1.0e-6));
}

TEST_CASE("Meter observations distinguish absent subdivisions from measured zero accents",
          "[beat_accent_meter]") {
  constexpr int kBeatCount = 24;
  const float scale = GENERATE(1.0f, std::numeric_limits<float>::max() / 2.0f);
  MeterBeats observations;
  for (int beat = 0; beat < kBeatCount; ++beat) {
    const float level = scale * (beat % 3 == 0 ? 1.0f : 0.35f);
    observations.accent.push_back(level);
    observations.has_attack.push_back(1);
    if (beat + 1 < kBeatCount) observations.subdivision_accent.push_back(level);
  }

  const MeterResult compound = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK(compound.time_signature.numerator == 6);
  CHECK(compound.time_signature.denominator == 8);
  REQUIRE(compound.compound_score.has_value());
  CHECK_THAT(*compound.compound_score, WithinAbs(1.0, 1.0e-6));

  std::fill(observations.subdivision_accent.begin(), observations.subdivision_accent.end(), 0.0f);
  const MeterResult zero = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK(zero.time_signature.numerator == 3);
  CHECK(zero.time_signature.denominator == 4);
  REQUIRE(zero.compound_score.has_value());
  CHECK_THAT(*zero.compound_score, WithinAbs(0.0, 1.0e-6));

  observations.subdivision_accent.clear();
  const MeterResult absent = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK(absent.time_signature.numerator == 3);
  CHECK(absent.time_signature.denominator == 4);
  CHECK_FALSE(absent.compound_score.has_value());
}

TEST_CASE("Meter observation validation accepts omitted optional evidence", "[beat_accent_meter]") {
  MeterBeats observations;
  observations.accent = {1.0f, 0.35f, 0.35f, 1.0f, 0.35f, 0.35f, 1.0f, 0.35f};
  observations.low_frequency_energy = {16.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  observations.subdivision_accent.assign(observations.accent.size() - 1, 0.0f);

  const MeterResult all_attacks = estimate_meter_from_observations(observations, MeterConfig{});
  REQUIRE(all_attacks.searched);
  CHECK(all_attacks.unobserved_beats.empty());

  observations.has_attack.assign(observations.accent.size(), 1);
  observations.has_attack.front() = 0;
  const MeterResult masked = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK(masked.unobserved_beats == std::vector<int>{0});

  observations.low_frequency_energy.clear();
  const MeterResult without_energy = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK(without_energy.unobserved_beats.empty());

  observations.accent.resize(1);
  observations.has_attack.clear();
  observations.subdivision_accent.clear();
  const MeterResult one_beat = estimate_meter_from_observations(observations, MeterConfig{});
  CHECK_FALSE(one_beat.searched);
  CHECK(one_beat.time_signature.numerator == 4);
  CHECK(one_beat.time_signature.denominator == 4);
  CHECK_THAT(one_beat.time_signature.confidence, WithinAbs(0.0, 1.0e-6));
}

TEST_CASE("Meter observation validation rejects malformed vector lengths", "[beat_accent_meter]") {
  MeterBeats observations{{1.0f, 0.5f, 0.25f}, {}, {}, {}};
  const auto reject = [](const MeterBeats& input) {
    (void)estimate_meter_from_observations(input, MeterConfig{});
  };

  require_meter_refusal([&] { reject({}); }, {"accent"});

  MeterBeats wrong_attacks = observations;
  wrong_attacks.has_attack = {1, 0};
  require_meter_refusal([&] { reject(wrong_attacks); }, {"has_attack", "accent"});

  MeterBeats wrong_energy = observations;
  wrong_energy.low_frequency_energy = {0.2f, 0.1f};
  require_meter_refusal([&] { reject(wrong_energy); }, {"low_frequency_energy", "accent"});

  MeterBeats wrong_subdivision = observations;
  wrong_subdivision.subdivision_accent = {0.1f};
  require_meter_refusal([&] { reject(wrong_subdivision); }, {"subdivision_accent", "accent"});

  MeterBeats one_beat_with_interval{{1.0f}, {}, {}, {0.0f}};
  require_meter_refusal([&] { reject(one_beat_with_interval); }, {"subdivision_accent", "accent"});
}

TEST_CASE("Meter observation validation rejects invalid values and options",
          "[beat_accent_meter]") {
  const MeterBeats valid{{1.0f, 0.0f, 0.25f}, {1, 0, 1}, {0.0f, 0.1f, 0.3f}, {0.1f, 0.0f}};
  const auto reject = [](const MeterBeats& input, const MeterConfig& config = MeterConfig{}) {
    (void)estimate_meter_from_observations(input, config);
  };
  REQUIRE_NOTHROW(reject(valid));
  const float invalid_values[] = {-0.1f, std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()};
  for (float invalid : invalid_values) {
    MeterBeats bad_accent = valid;
    bad_accent.accent[1] = invalid;
    require_meter_refusal([&] { reject(bad_accent); }, {"accent"});

    MeterBeats bad_energy = valid;
    bad_energy.low_frequency_energy[1] = invalid;
    require_meter_refusal([&] { reject(bad_energy); }, {"low_frequency_energy"});

    MeterBeats bad_subdivision = valid;
    bad_subdivision.subdivision_accent[1] = invalid;
    require_meter_refusal([&] { reject(bad_subdivision); }, {"subdivision_accent"});
  }

  MeterBeats bad_flag = valid;
  bad_flag.has_attack[1] = 2;
  require_meter_refusal([&] { reject(bad_flag); }, {"has_attack"});

  MeterConfig bad_config;
  bad_config.candidate_numerators.clear();
  require_meter_refusal([&] { reject(valid, bad_config); }, {"candidate_numerators"});
}
