/// @file phase_alignment_test.cpp
/// @brief Pairwise time and polarity measurement tests for the mixing assistant.

#include "mixing/assistant/phase_alignment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace assistant = sonare::mixing::assistant;

using Catch::Matchers::WithinAbs;

namespace {

constexpr int kSampleRate = 48000;
// 0.3 s is long enough that the +/-30 ms search still leaves a core segment of
// ~11500 samples, which is far more than the correlation needs to be decisive.
constexpr std::size_t kFrames = 14400;
// A 5 ms offset: well inside the default search range and unambiguous.
constexpr std::size_t kDelaySamples = 240;

std::vector<float> noise(std::size_t frames, uint32_t seed) {
  std::vector<float> samples(frames);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
  for (auto& sample : samples) sample = distribution(rng);
  return samples;
}

// The same material arriving `delay` samples later, optionally polarity-flipped
// via a negative scale.
std::vector<float> delayed(const std::vector<float>& source, std::size_t delay, float scale) {
  std::vector<float> samples(source.size(), 0.0f);
  for (std::size_t i = delay; i < source.size(); ++i) {
    samples[i] = scale * source[i - delay];
  }
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

assistant::TrackProfile profile_of(const std::string& id, bool usable = true) {
  assistant::TrackProfile profile;
  profile.strip_id = id;
  profile.usable = usable;
  return profile;
}

// Measures one pair and returns its single alignment entry.
assistant::PairAlignment measure(const std::vector<float>& reference,
                                 const std::vector<float>& target, bool reference_usable = true,
                                 bool target_usable = true) {
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", reference),
                                                     mono_track("b", target)};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a", reference_usable),
                                                         profile_of("b", target_usable)};
  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 1);
  return alignments.front();
}

}  // namespace

TEST_CASE("phase alignment reports a known delay with the documented sign", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 1u);
  const std::vector<float> late = delayed(source, kDelaySamples, 1.0f);

  // Reference first, target second: the target arrives later, so the lag is
  // positive -- the reference is what would need delaying to meet it.
  const assistant::PairAlignment forward = measure(source, late);
  REQUIRE(forward.reference_index == 0);
  REQUIRE(forward.target_index == 1);
  REQUIRE(forward.related);
  REQUIRE_FALSE(forward.polarity_opposed);
  REQUIRE(forward.lag_samples == static_cast<int>(kDelaySamples));
  REQUIRE_THAT(forward.correlation, WithinAbs(1.0f, 0.01f));

  // Swapping the two flips the sign and nothing else: the target now arrives
  // earlier than the reference.
  const assistant::PairAlignment reversed = measure(late, source);
  REQUIRE(reversed.related);
  REQUIRE(reversed.lag_samples == -static_cast<int>(kDelaySamples));
  REQUIRE_THAT(reversed.correlation, WithinAbs(1.0f, 0.01f));
}

TEST_CASE("phase alignment detects a polarity-flipped copy", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 2u);
  const std::vector<float> flipped = delayed(source, 0, -1.0f);

  const assistant::PairAlignment pair = measure(source, flipped);
  REQUIRE(pair.related);
  REQUIRE(pair.polarity_opposed);
  REQUIRE(pair.lag_samples == 0);
  REQUIRE_THAT(pair.correlation, WithinAbs(-1.0f, 0.01f));
}

TEST_CASE("phase alignment separates delay from polarity", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 3u);
  const std::vector<float> late_and_flipped = delayed(source, kDelaySamples, -1.0f);

  const assistant::PairAlignment pair = measure(source, late_and_flipped);
  REQUIRE(pair.related);
  REQUIRE(pair.polarity_opposed);
  REQUIRE(pair.lag_samples == static_cast<int>(kDelaySamples));
  REQUIRE_THAT(pair.correlation, WithinAbs(-1.0f, 0.01f));
}

TEST_CASE("phase alignment leaves unrelated tracks alone", "[mixing][assistant]") {
  const std::vector<float> first = noise(kFrames, 4u);
  const std::vector<float> second = noise(kFrames, 5u);

  const assistant::PairAlignment pair = measure(first, second);
  REQUIRE_FALSE(pair.related);
  REQUIRE(std::abs(pair.correlation) < assistant::kDefaultMinAbsCorrelation);
}

TEST_CASE("phase alignment survives a silent track without a NaN", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 6u);
  const std::vector<float> silence(kFrames, 0.0f);

  const assistant::PairAlignment pair = measure(source, silence);
  REQUIRE_FALSE(pair.related);
  REQUIRE_FALSE(pair.polarity_opposed);
  REQUIRE(pair.lag_samples == 0);
  REQUIRE(std::isfinite(pair.correlation));
  REQUIRE_THAT(pair.correlation, WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("phase alignment reports zero lag for an identical track", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 7u);

  const assistant::PairAlignment pair = measure(source, source);
  REQUIRE(pair.related);
  REQUIRE(pair.lag_samples == 0);
  REQUIRE_FALSE(pair.polarity_opposed);
  REQUIRE_THAT(pair.correlation, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("phase alignment refuses to invent a lag beyond the search range",
          "[mixing][assistant]") {
  // 200 ms is far outside the +/-30 ms the search covers. The honest answer is
  // "no relationship found"; a confident small lag would be worse than nothing,
  // because the caller would delay a track by it.
  constexpr std::size_t kLongFrames = 24000;
  constexpr std::size_t kOutOfRangeDelay = 9600;
  const std::vector<float> source = noise(kLongFrames, 8u);
  const std::vector<float> very_late = delayed(source, kOutOfRangeDelay, 1.0f);

  const assistant::PairAlignment pair = measure(source, very_late);
  REQUIRE_FALSE(pair.related);
  REQUIRE(std::abs(pair.correlation) < assistant::kDefaultMinAbsCorrelation);
}

TEST_CASE("phase alignment compares stereo tracks as their mono sum", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 9u);
  const std::vector<float> late = delayed(source, kDelaySamples, 1.0f);

  assistant::TrackInput stereo = mono_track("a", source);
  stereo.right = source.data();
  const std::vector<assistant::TrackInput> tracks = {stereo, mono_track("b", late)};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a"), profile_of("b")};

  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 1);
  REQUIRE(alignments.front().related);
  REQUIRE(alignments.front().lag_samples == static_cast<int>(kDelaySamples));
}

TEST_CASE("phase alignment measures tracks of different lengths", "[mixing][assistant]") {
  const std::vector<float> longer = noise(24000, 10u);
  const std::vector<float> shorter(longer.begin(),
                                   longer.begin() + static_cast<std::ptrdiff_t>(kFrames));

  const assistant::PairAlignment pair = measure(shorter, longer);
  REQUIRE(pair.related);
  REQUIRE(pair.lag_samples == 0);
  REQUIRE_THAT(pair.correlation, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("phase alignment skips a pair containing an unusable track", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 11u);
  const std::vector<float> late = delayed(source, kDelaySamples, 1.0f);

  // The pair is still reported, so the matrix stays complete; it is simply not
  // something to act on.
  const assistant::PairAlignment pair = measure(source, late, /*reference_usable=*/true,
                                                /*target_usable=*/false);
  REQUIRE(pair.reference_index == 0);
  REQUIRE(pair.target_index == 1);
  REQUIRE_FALSE(pair.related);
  REQUIRE(pair.lag_samples == 0);
  REQUIRE_THAT(pair.correlation, WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("phase alignment handles degenerate track counts", "[mixing][assistant]") {
  const std::vector<float> source = noise(kFrames, 12u);

  REQUIRE(assistant::analyze_phase_alignment({}, {}).empty());
  REQUIRE(assistant::analyze_phase_alignment({mono_track("a", source)}, {profile_of("a")}).empty());
  // A profile list shorter than the track list is not an error: the tracks it
  // does not cover simply have no usable profile.
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", source),
                                                     mono_track("b", source)};
  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, {});
  REQUIRE(alignments.size() == 1);
  REQUIRE_FALSE(alignments.front().related);
}

TEST_CASE("phase alignment leaves unmeasurable pairs alone", "[mixing][assistant]") {
  // Too short to correlate over: the search would be decided by a handful of
  // samples, so the pair is reported unmeasured rather than guessed at.
  const std::vector<float> tiny = noise(64, 15u);
  const assistant::PairAlignment short_pair = measure(tiny, tiny);
  REQUIRE_FALSE(short_pair.related);
  REQUIRE(short_pair.lag_samples == 0);
  REQUIRE_THAT(short_pair.correlation, WithinAbs(0.0f, 0.0001f));

  // A lag counted in samples has no shared meaning across two sample rates.
  const std::vector<float> source = noise(kFrames, 16u);
  assistant::TrackInput resampled = mono_track("b", source);
  resampled.sample_rate = kSampleRate / 2;
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", source), resampled};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a"), profile_of("b")};

  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 1);
  REQUIRE_FALSE(alignments.front().related);
  REQUIRE(alignments.front().lag_samples == 0);
}

TEST_CASE("phase alignment emits one ascending entry per unordered pair", "[mixing][assistant]") {
  const std::vector<float> first = noise(kFrames, 13u);
  const std::vector<float> second = delayed(first, kDelaySamples, 1.0f);
  const std::vector<float> third = noise(kFrames, 14u);
  const std::vector<float> fourth(kFrames, 0.0f);

  const std::vector<assistant::TrackInput> tracks = {
      mono_track("a", first), mono_track("b", second), mono_track("c", third),
      mono_track("d", fourth)};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a"), profile_of("b"),
                                                         profile_of("c"), profile_of("d")};

  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 6);
  for (const assistant::PairAlignment& pair : alignments) {
    INFO("pair " << pair.reference_index << " -> " << pair.target_index);
    REQUIRE(pair.reference_index < pair.target_index);
    REQUIRE(std::isfinite(pair.correlation));
  }
  // Only the delayed copy is a real relationship.
  REQUIRE(alignments.front().related);
  REQUIRE(alignments.front().lag_samples == static_cast<int>(kDelaySamples));
  for (std::size_t index = 1; index < alignments.size(); ++index) {
    INFO("entry " << index);
    REQUIRE_FALSE(alignments[index].related);
  }
}
