#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/music_analyzer.h"
#include "mir/tempo_estimator_bridge.h"
#include "util/constants.h"

TEST_CASE("auto tempo candidates retain the full analysis meter", "[.][slow][mir][tempo]") {
  constexpr int kSampleRate = sonare::constants::kDefaultSampleRate;
  constexpr int kBeatCount = 48;
  constexpr double kBeatSeconds =
      sonare::constants::kSecondsPerMinute / sonare::constants::kDefaultBpm;
  constexpr double kFirstClickSeconds = 0.13;
  constexpr int kClickFrames = kSampleRate / 100;
  const double end_seconds = kFirstClickSeconds + (kBeatCount - 1) * kBeatSeconds + 1.0;
  const auto frames = static_cast<size_t>(std::ceil(end_seconds * kSampleRate));
  std::vector<float> samples(frames, 0.0f);
  for (int beat = 0; beat < kBeatCount; ++beat) {
    const double seconds = kFirstClickSeconds + beat * kBeatSeconds;
    const auto start = static_cast<size_t>(std::lround(seconds * kSampleRate));
    const float gain = beat % 4 == 0 ? 1.0f : 0.5f;
    for (int frame = 0; frame < kClickFrames; ++frame) {
      samples[start + frame] =
          gain * static_cast<float>(std::sin(sonare::constants::kPiD * frame / kClickFrames));
    }
  }
  const auto audio = sonare::Audio::from_vector(std::move(samples), kSampleRate);

  sonare::MusicAnalyzer analysis(audio);
  const auto expected = analysis.beat_analyzer().time_signature();
  const auto candidates = sonare::mir::estimate_tempo_from_audio(audio);
  REQUIRE(expected.confidence > 0.0f);
  REQUIRE(candidates.size() == 3);
  for (const auto& estimate : candidates) {
    CAPTURE(estimate.label);
    CHECK(estimate.meter.numerator == expected.numerator);
    CHECK(estimate.meter.denominator == expected.denominator);
    CHECK(std::abs(estimate.meter.confidence - expected.confidence) <= 1.0e-6f);
  }
}

TEST_CASE("auto tempo preserves a meter estimate with zero confidence", "[mir][tempo]") {
  sonare::mir::BeatAnalysisInput input;
  input.time_signature = {3, 8, 0.0f};
  const auto candidates = sonare::mir::estimate_tempo(input);
  REQUIRE(candidates.size() == 1);
  CHECK(candidates.front().meter.numerator == 3);
  CHECK(candidates.front().meter.denominator == 8);
  CHECK(candidates.front().meter.confidence == 0.0f);
}
