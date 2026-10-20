/// @file phase_alignment_outlier_test.cpp
/// @brief One loud click must not pull the pair's analysis window onto itself.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "mixing/assistant/phase_alignment.h"

namespace assistant = sonare::mixing::assistant;

using Catch::Matchers::WithinAbs;

namespace {

constexpr int kSampleRate = 48000;
// 12 s: long enough for a robust reference to set one event aside.
constexpr std::size_t kFrames = 12 * kSampleRate;
constexpr std::size_t kDelaySamples = 240;
constexpr std::size_t kClickStart = 6 * kSampleRate;
constexpr std::size_t kClickLength = 64;
// About +40 dB over the steady material's RMS.
constexpr float kClickAmplitude = 30.0f;

std::vector<float> noise(std::size_t frames, uint32_t seed) {
  std::vector<float> samples(frames);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
  for (auto& sample : samples) sample = distribution(rng);
  return samples;
}

std::vector<float> delayed(const std::vector<float>& source, std::size_t delay) {
  std::vector<float> samples(source.size(), 0.0f);
  for (std::size_t i = delay; i < source.size(); ++i) samples[i] = source[i - delay];
  return samples;
}

assistant::TrackInput mono_track(const std::string& id, const std::vector<float>& samples) {
  assistant::TrackInput track;
  track.id = id;
  track.left = samples.data();
  track.frame_count = samples.size();
  track.sample_rate = kSampleRate;
  return track;
}

assistant::TrackProfile usable_profile(const std::string& id) {
  assistant::TrackProfile profile;
  profile.strip_id = id;
  profile.usable = true;
  return profile;
}

assistant::PairAlignment measure(const std::vector<float>& reference,
                                 const std::vector<float>& target) {
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", reference),
                                                     mono_track("b", target)};
  const std::vector<assistant::TrackProfile> profiles = {usable_profile("a"), usable_profile("b")};
  const auto alignments = assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 1);
  return alignments.front();
}

}  // namespace

TEST_CASE("phase alignment positive control: a steady pair is measured without a click",
          "[mixing][assistant]") {
  const std::vector<float> steady = noise(kFrames, 7u);
  const assistant::PairAlignment pair = measure(steady, delayed(steady, kDelaySamples));

  REQUIRE(pair.related);
  REQUIRE(pair.lag_samples == static_cast<int>(kDelaySamples));
  REQUIRE_THAT(pair.correlation, WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("phase alignment ignores a lone loud click on one track when choosing the window",
          "[mixing][assistant]") {
  const std::vector<float> steady = noise(kFrames, 7u);
  std::vector<float> clicked = steady;
  for (std::size_t i = 0; i < kClickLength; ++i) clicked[kClickStart + i] += kClickAmplitude;

  const assistant::PairAlignment control = measure(steady, delayed(steady, kDelaySamples));
  const assistant::PairAlignment pair = measure(clicked, delayed(steady, kDelaySamples));

  // Windowed onto the click, the reference energy is mostly click that the
  // target does not carry, which collapses the normalized correlation.
  REQUIRE(pair.lag_samples == control.lag_samples);
  REQUIRE(pair.related);
  REQUIRE_THAT(pair.correlation, WithinAbs(control.correlation, 0.05f));
}
