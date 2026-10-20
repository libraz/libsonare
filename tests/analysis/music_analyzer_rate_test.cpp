/// @file music_analyzer_rate_test.cpp
/// @brief MusicAnalyzer's melody and dynamics views step on the same time grid at every input
///        rate, the one its hop names at the analysis rate.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/music_analyzer.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

// A held 220 Hz tone with a slow swell, so YIN finds a pitch and the loudness curve moves.
Audio tone_at(int sample_rate) {
  constexpr float kSeconds = 3.0f;
  const int n = static_cast<int>(kSeconds * static_cast<float>(sample_rate));
  std::vector<float> samples(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float swell = 0.2f + 0.3f * std::sin(constants::kPi * t / kSeconds);
    samples[static_cast<size_t>(i)] = swell * std::sin(constants::kTwoPi * 220.0f * t);
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("MusicAnalyzer melody and dynamics share the analysis-rate hop at every input rate",
          "[music_analyzer][analysis-rate]") {
  const float hop_sec = static_cast<float>(MusicAnalyzerConfig().hop_length) /
                        static_cast<float>(kAnalysisSampleRate);
  for (const int sample_rate : {22050, 44100, 48000}) {
    INFO("sample_rate=" << sample_rate);
    MusicAnalyzer analyzer(tone_at(sample_rate));

    const auto& pitches = analyzer.melody_analyzer().contour().pitches;
    REQUIRE(pitches.size() > 2);
    CHECK_THAT(pitches[1].time - pitches[0].time, WithinAbs(hop_sec, 1e-4f));

    const auto& times = analyzer.dynamics_analyzer().loudness_curve().times;
    REQUIRE(times.size() > 2);
    CHECK_THAT(times[1] - times[0], WithinAbs(hop_sec, 1e-4f));
  }
}
