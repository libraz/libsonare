/// @file sonare_c_project_tempo_test.cpp
/// @brief Project tempo estimation through the C ABI at input rates above the analysis rate.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "util/constants.h"

namespace {

// A click track at 110 BPM accented every sixth beat; the beat tracker running at the caller's
// rate rather than the analysis rate reads it as a three.
std::vector<float> accented_clicks(int sample_rate) {
  constexpr double kBpm = 110.0;
  constexpr double kSeconds = 12.0;
  constexpr int kBeatsPerBar = 6;
  const auto n = static_cast<size_t>(sample_rate * kSeconds);
  std::vector<float> samples(n, 0.0f);
  const double beat_samples = sample_rate * 60.0 / kBpm;
  const auto click = static_cast<size_t>(sample_rate * 0.01);
  for (int beat = 0; beat * beat_samples < static_cast<double>(n); ++beat) {
    const auto start = static_cast<size_t>(std::lround(beat * beat_samples));
    const float gain = beat % kBeatsPerBar == 0 ? 1.0f : 0.5f;
    for (size_t i = 0; i < click && start + i < n; ++i) {
      samples[start + i] =
          gain * static_cast<float>(std::sin(sonare::constants::kPiD * static_cast<double>(i) /
                                             static_cast<double>(click)));
    }
  }
  return samples;
}

struct TempoReading {
  float bpm = 0.0f;
  int numerator = 0;
  int denominator = 0;
  size_t time_signature_count = 0;
  float applied_bpm = 0.0f;
};

TempoReading read_tempo(const std::vector<float>& samples, int sample_rate) {
  TempoReading reading;
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES] = {};
  size_t count = 0;
  REQUIRE(sonare_project_analyze_tempo(project, samples.data(), samples.size(), sample_rate,
                                       nullptr, candidates, SONARE_PROJECT_MAX_TEMPO_CANDIDATES,
                                       &count) == SONARE_OK);
  REQUIRE(count >= 1);
  reading.bpm = candidates[0].bpm;
  reading.numerator = candidates[0].first_time_signature.numerator;
  reading.denominator = candidates[0].first_time_signature.denominator;
  reading.time_signature_count = candidates[0].time_signature_count;
  SonareProjectAutoTempoResult applied{};
  REQUIRE(sonare_project_auto_tempo(project, samples.data(), samples.size(), sample_rate, nullptr,
                                    0, 1, &applied) == SONARE_OK);
  reading.applied_bpm = applied.bpm;
  sonare_project_destroy(project);
  return reading;
}

}  // namespace

TEST_CASE("project tempo estimation reads the meter the full analysis reads at any input rate",
          "[c_api][project][tempo]") {
  const std::vector<float> at_44100 = accented_clicks(44100);
  const std::vector<float> at_48000 = accented_clicks(48000);
  const TempoReading a = read_tempo(at_44100, 44100);
  const TempoReading b = read_tempo(at_48000, 48000);

  CHECK(a.numerator == b.numerator);
  CHECK(a.denominator == b.denominator);
  CHECK(a.time_signature_count == b.time_signature_count);
  CHECK(a.bpm == Catch::Approx(b.bpm).epsilon(0.01));
  CHECK(a.applied_bpm == a.bpm);
  CHECK(b.applied_bpm == b.bpm);

  for (const auto& [samples, rate, reading] :
       {std::tuple{&at_44100, 44100, a}, std::tuple{&at_48000, 48000, b}}) {
    sonare::MusicAnalyzer analyzer(
        sonare::Audio::from_buffer(samples->data(), samples->size(), rate));
    const sonare::TimeSignature analyzed = analyzer.analyze().time_signature;
    CHECK(reading.numerator == analyzed.numerator);
    CHECK(reading.denominator == analyzed.denominator);
  }
}
