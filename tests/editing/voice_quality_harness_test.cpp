/// @file voice_quality_harness_test.cpp
/// @brief Self-validation of the voicelab evaluator against synthetic voices with known truth.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/resample.h"
#include "util/constants.h"
#include "voicelab/metrics.h"
#include "voicelab/synthetic_voice.h"

namespace {

using sonare::voicelab::VoiceSegment;
namespace vl = sonare::voicelab;

constexpr double kSr = 48000.0;
constexpr int kVowelA = 0;
constexpr float kLevelDb = -20.0f;
// Frames whose analysis window reaches the onset or release ramp are left out.
constexpr double kEdgeS = 0.15;

VoiceSegment steady(double seconds, float f0, int vowel = kVowelA, float ramp_ms = 10.0f) {
  return {seconds, f0, f0, 0.0f, 0.0f, true, vowel, kLevelDb, ramp_ms};
}

double cents(double a, double b) { return sonare::constants::kCentsPerOctave * std::log2(a / b); }

/// Median of the voiced interior frames of the evaluator's f0 track.
float median_f0(const std::vector<float>& x, double sr) {
  const double hop_s = 0.01;
  const std::vector<float> track = vl::estimate_f0_track(x.data(), x.size(), sr, hop_s);
  const auto edge = static_cast<std::size_t>(kEdgeS / hop_s);
  std::vector<float> inner;
  for (std::size_t i = edge; i + edge < track.size(); ++i) {
    if (track[i] > 0.0f) inner.push_back(track[i]);
  }
  if (inner.empty()) return 0.0f;
  std::nth_element(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(inner.size() / 2),
                   inner.end());
  return inner[inner.size() / 2];
}

/// Band-limited sawtooth (1/k partials) with every partial below 0.95 of the Nyquist of `sr`.
std::vector<float> sawtooth(double f0, double sr, std::size_t n) {
  std::vector<double> acc(n, 0.0);
  for (int k = 1; k * f0 < 0.95 * 0.5 * sr; ++k) {
    const double w = sonare::constants::kTwoPiD * k * f0 / sr;
    for (std::size_t i = 0; i < n; ++i) acc[i] += std::sin(w * static_cast<double>(i)) / k;
  }
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(0.05 * acc[i]);
  return out;
}

}  // namespace

TEST_CASE("f0 evaluator is within 1 cent on steady and gliding synthetic voices",
          "[voice_quality]") {
  for (const double sr : {48000.0, 44100.0}) {
    for (const float f0 : {110.0f, 220.0f, 440.0f}) {
      const std::vector<float> x = vl::synthesize_voice({steady(0.6, f0)}, sr);
      const double hop_s = 0.01;
      std::vector<bool> voiced;
      const std::vector<float> track =
          vl::estimate_f0_track(x.data(), x.size(), sr, hop_s, &voiced);
      const auto edge = static_cast<std::size_t>(kEdgeS / hop_s);
      REQUIRE(track.size() > 2 * edge);
      double worst = 0.0;
      bool all_voiced = true;
      for (std::size_t i = edge; i + edge < track.size(); ++i) {
        all_voiced = all_voiced && voiced[i] && track[i] > 0.0f;
        if (track[i] > 0.0f) worst = std::max(worst, std::abs(cents(track[i], f0)));
      }
      INFO("sr " << sr << " f0 " << f0 << " worst " << worst << " cent");
      CHECK(all_voiced);
      CHECK(worst <= 1.0);
    }
  }

  // An exponential glide: each frame is compared with the true f0 at its centre.
  std::vector<float> truth;
  const std::vector<float> x = vl::synthesize_voice(
      {{1.0, 200.0f, 240.0f, 0.0f, 0.0f, true, kVowelA, kLevelDb, 10.0f}}, kSr, &truth);
  REQUIRE(truth.size() == x.size());
  const double hop_s = 0.01;
  const std::vector<float> track = vl::estimate_f0_track(x.data(), x.size(), kSr, hop_s);
  const auto hop = static_cast<std::size_t>(std::lround(hop_s * kSr));
  const auto edge = static_cast<std::size_t>(kEdgeS / hop_s);
  double worst = 0.0;
  for (std::size_t i = edge; i + edge < track.size(); ++i) {
    REQUIRE(track[i] > 0.0f);
    worst = std::max(worst, std::abs(cents(track[i], truth[i * hop])));
  }
  INFO("glide worst " << worst << " cent");
  CHECK(worst <= 1.0);
}

TEST_CASE("f0 evaluator marks white noise unvoiced", "[voice_quality]") {
  const std::vector<float> x = vl::synthesize_voice(
      {{1.0, 220.0f, 220.0f, 0.0f, 0.0f, false, kVowelA, kLevelDb, 10.0f}}, kSr);
  std::vector<bool> voiced;
  const std::vector<float> track = vl::estimate_f0_track(x.data(), x.size(), kSr, 0.01, &voiced);
  REQUIRE_FALSE(track.empty());
  CHECK(std::count(voiced.begin(), voiced.end(), true) == 0);
  CHECK(std::all_of(track.begin(), track.end(), [](float f) { return f == 0.0f; }));
}

TEST_CASE("formant evaluator is within 2% at partial spacings 110 and 220 Hz", "[voice_quality]") {
  for (int vowel = 0; vowel < 5; ++vowel) {
    const std::array<float, 3> truth = vl::vowel_formants_hz(vowel);
    for (const float f0 : {110.0f, 220.0f}) {
      const std::vector<float> x = vl::synthesize_voice({steady(0.5, f0, vowel)}, kSr);
      const std::array<float, 3> formants = vl::estimate_formants(x.data(), x.size(), kSr);
      for (std::size_t k = 0; k < 3; ++k) {
        const double error = std::abs(formants[k] / truth[k] - 1.0);
        INFO("vowel " << vowel << " f0 " << f0 << " F" << (k + 1) << " measured " << formants[k]
                      << " true " << truth[k]);
        CHECK(error <= 0.02);
      }
    }
  }
}

TEST_CASE("log-spectral distance self-error at 440 Hz partial spacing is at most 1 dB",
          "[voice_quality]") {
  // The same vowel at 220 Hz spacing is the reference envelope.
  for (int vowel = 0; vowel < 5; ++vowel) {
    const std::vector<float> reference = vl::synthesize_voice({steady(0.5, 220.0f, vowel)}, kSr);
    const std::vector<float> test = vl::synthesize_voice({steady(0.5, 440.0f, vowel)}, kSr);
    const float lsd =
        vl::log_spectral_distance_db(reference.data(), test.data(), reference.size(), kSr, 1.0f);
    INFO("vowel " << vowel << " LSD " << lsd << " dB");
    CHECK(lsd <= 1.0f);
  }
}

TEST_CASE("log-spectral distance follows the reference warp", "[voice_quality]") {
  // Playing the 110 Hz vowel back 1.2x faster scales every frequency, envelope included, by 1.2.
  const std::vector<float> reference = vl::synthesize_voice({steady(1.0, 110.0f)}, kSr);
  const std::vector<float> scaled =
      sonare::resample(reference.data(), reference.size(), 48000, 40000);
  const std::size_t n = std::min(reference.size(), scaled.size());
  const float warped = vl::log_spectral_distance_db(reference.data(), scaled.data(), n, kSr, 1.2f);
  const float unwarped =
      vl::log_spectral_distance_db(reference.data(), scaled.data(), n, kSr, 1.0f);
  INFO("warped " << warped << " dB, unwarped " << unwarped << " dB");
  // The 3 dB envelope bound: the warped comparison passes it, the unwarped one fails it.
  CHECK(warped <= 3.0f);
  CHECK(unwarped > 3.0f);
}

TEST_CASE("perfectly periodic 440 Hz voice has inharmonic energy at most -60 dB",
          "[voice_quality]") {
  const std::vector<float> x = vl::synthesize_voice({steady(1.0, 440.0f)}, kSr);
  const float f0 = median_f0(x, kSr);
  REQUIRE(f0 > 0.0f);
  const float db = vl::inharmonic_energy_db(x.data(), x.size(), kSr, f0);
  INFO("measured f0 " << f0 << " inharmonic " << db << " dB");
  CHECK(db <= -60.0f);
}

TEST_CASE("inharmonic energy detects aliasing from naive resampling", "[voice_quality]") {
  // A bright 220 Hz source decimated by 2 with no lowpass: +12 semitones, partials above
  // the new Nyquist fold back between the 440 Hz harmonics.
  const std::size_t n = 48000;
  const std::vector<float> source = sawtooth(220.0, kSr, 2 * n);
  std::vector<float> aliased(n);
  for (std::size_t i = 0; i < n; ++i) aliased[i] = source[2 * i];
  const std::vector<float> clean = sawtooth(440.0, kSr, n);

  const float f0_aliased = median_f0(aliased, kSr);
  const float f0_clean = median_f0(clean, kSr);
  REQUIRE(f0_aliased > 0.0f);
  REQUIRE(f0_clean > 0.0f);
  const float db_aliased = vl::inharmonic_energy_db(aliased.data(), n, kSr, f0_aliased);
  const float db_clean = vl::inharmonic_energy_db(clean.data(), n, kSr, f0_clean);
  INFO("aliased " << db_aliased << " dB (f0 " << f0_aliased << "), clean " << db_clean << " dB (f0 "
                  << f0_clean << ")");
  CHECK(db_aliased >= -30.0f);
  CHECK(db_clean <= -60.0f);
}

TEST_CASE("latency estimate recovers a known delay within one sample", "[voice_quality]") {
  std::vector<VoiceSegment> in_segments;
  std::vector<VoiceSegment> shifted_segments;
  for (int i = 0; i < 5; ++i) {
    const bool voiced = i % 2 == 0;
    in_segments.push_back({0.2, 220.0f, 220.0f, 0.0f, 0.0f, voiced, kVowelA, kLevelDb, 5.0f});
    // +7 semitones, same timing: the waveforms differ and only the envelopes line up.
    shifted_segments.push_back({0.2, 329.6f, 329.6f, 0.0f, 0.0f, voiced, kVowelA, kLevelDb, 5.0f});
  }
  const std::vector<float> in = vl::synthesize_voice(in_segments, kSr);
  const std::vector<float> shifted = vl::synthesize_voice(shifted_segments, kSr);
  const int max_lag = 2400;
  for (const int delay : {0, 1, 717, 2263}) {
    std::vector<float> copy(in.size(), 0.0f);
    std::vector<float> moved(in.size(), 0.0f);
    for (std::size_t i = static_cast<std::size_t>(delay); i < in.size(); ++i) {
      copy[i] = 0.5f * in[i - static_cast<std::size_t>(delay)];
      moved[i] = shifted[i - static_cast<std::size_t>(delay)];
    }
    const int wave =
        vl::estimate_latency_samples(in.data(), copy.data(), in.size(), max_lag, false);
    const int env = vl::estimate_latency_samples(in.data(), copy.data(), in.size(), max_lag, true);
    const int env_shifted =
        vl::estimate_latency_samples(in.data(), moved.data(), in.size(), max_lag, true);
    INFO("delay " << delay << " waveform " << wave << " envelope " << env << " shifted envelope "
                  << env_shifted);
    CHECK(std::abs(wave - delay) <= 1);
    CHECK(std::abs(env - delay) <= 1);
    // Envelope tolerance for a +-7 semitone output.
    CHECK(std::abs(env_shifted - delay) <= 48);
  }
}

TEST_CASE("click counter reads alternating voice and noise as clean and finds injected clicks",
          "[voice_quality]") {
  // 10 s of 200 ms voiced vowel / equal-RMS white noise, 5 ms raised-cosine edges.
  std::vector<VoiceSegment> segments;
  for (int i = 0; i < 50; ++i) {
    segments.push_back({0.2, 220.0f, 220.0f, 0.0f, 0.0f, i % 2 == 0, kVowelA, kLevelDb, 5.0f});
  }
  const std::vector<float> stimulus = vl::synthesize_voice(segments, kSr);
  CHECK(vl::count_clicks(stimulus.data(), stimulus.size(), kSr) == 0);

  // Inside the voiced segment starting at 4.0 s.
  const auto at = static_cast<std::size_t>(4.1 * kSr) + 77;
  std::vector<float> step = stimulus;
  for (std::size_t i = at; i < step.size(); ++i) step[i] += 0.3f;
  CHECK(vl::count_clicks(step.data(), step.size(), kSr) == 1);
  // A one-sample impulse is a jump up and a jump back down.
  std::vector<float> impulse = stimulus;
  impulse[at] += 0.3f;
  CHECK(vl::count_clicks(impulse.data(), impulse.size(), kSr) == 2);
  // A step inside the first 10 ms is not counted.
  std::vector<float> early = stimulus;
  const auto early_at = static_cast<std::size_t>(0.002 * kSr);
  for (std::size_t i = early_at; i < early.size(); ++i) early[i] += 0.3f;
  CHECK(vl::count_clicks(early.data(), early.size(), kSr) == 0);
}
