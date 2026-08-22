/// @file phase_alignment_test.cpp
/// @brief Pairwise time and polarity measurement tests for the mixing assistant.

#include "mixing/assistant/phase_alignment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
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

TEST_CASE("the lag search costs one pass over the excerpt per pair", "[mixing][assistant]") {
  // The cost of this pass is invisible in its result: evaluating every lag
  // through one transform pair and evaluating them one at a time return the
  // same alignments. So the shape of the work is asserted rather than its wall
  // clock, which would pass on a fast machine whichever shape was there.
  const std::vector<float> source = noise(kFrames, 21u);
  const std::vector<float> late = delayed(source, kDelaySamples, 1.0f);
  const std::vector<float> other = noise(kFrames, 22u);
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", source), mono_track("b", late),
                                                     mono_track("c", other)};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a"), profile_of("b"),
                                                         profile_of("c")};

  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  REQUIRE(alignments.size() == 3);

  const assistant::PhaseAlignmentCost cost = assistant::last_phase_alignment_cost();
  // Every pair here is well conditioned, so every one of them is measured and
  // every one recomputes its winning lag exactly once. A search that walked the
  // lags one at a time would report 2 * lag_range + 1 passes per pair instead --
  // 2881 at the default 30 ms range and 48 kHz.
  REQUIRE(cost.measured_pairs == 3);
  REQUIRE(cost.core_product_passes == 3);
}

TEST_CASE("widening the lag search does not multiply the work", "[mixing][assistant]") {
  // The bound a caller budgets against is "one pass per pair", and it has to
  // hold whatever the search range is: a cost that grows with the range is the
  // per-lag shape returning, whether or not the answers change with it.
  const std::vector<float> source = noise(kFrames, 23u);
  const std::vector<float> late = delayed(source, kDelaySamples, 1.0f);
  const std::vector<assistant::TrackInput> tracks = {mono_track("a", source),
                                                     mono_track("b", late)};
  const std::vector<assistant::TrackProfile> profiles = {profile_of("a"), profile_of("b")};

  assistant::PhaseAlignmentConfig narrow;
  narrow.max_lag_ms = 5.0f;
  assistant::analyze_phase_alignment(tracks, profiles, narrow);
  const assistant::PhaseAlignmentCost narrow_cost = assistant::last_phase_alignment_cost();

  assistant::PhaseAlignmentConfig wide;
  wide.max_lag_ms = assistant::kMaxSearchLagMs;
  assistant::analyze_phase_alignment(tracks, profiles, wide);
  const assistant::PhaseAlignmentCost wide_cost = assistant::last_phase_alignment_cost();

  // Twenty times the lag range, measured over the same pair, for the same work.
  REQUIRE(narrow_cost.measured_pairs == 1);
  REQUIRE(wide_cost.measured_pairs == 1);
  REQUIRE(wide_cost.core_product_passes == narrow_cost.core_product_passes);
  REQUIRE(wide_cost.core_product_passes == 1);
}

TEST_CASE("a full session's pairwise pass stays inside its budget",
          "[.][slow][mixing][assistant]") {
  // A secondary guard on the structural cases above: those pin the shape of the
  // work, this pins that the shape is still the one that finishes. Tagged slow
  // because it builds and analyses a 24-track session.
  //
  // The threshold is deliberately far above the measurement it protects. This
  // session runs in about 0.2 s in a release build; the bound is the 5 s a
  // 24-track session was required to come in under, which the per-lag search
  // missed by two orders of magnitude at roughly 19 s. Only the return of that
  // shape can trip this, not a slow or loaded machine.
  constexpr int kTrackCount = 24;
  constexpr std::size_t kSessionFrames = kSampleRate;  // 1 s per track.
  constexpr double kBudgetMs = 5000.0;

  std::vector<std::vector<float>> buffers;
  buffers.reserve(kTrackCount);
  for (int index = 0; index < kTrackCount; ++index) {
    buffers.push_back(noise(kSessionFrames, static_cast<uint32_t>(100 + index)));
  }
  std::vector<assistant::TrackInput> tracks;
  std::vector<assistant::TrackProfile> profiles;
  for (int index = 0; index < kTrackCount; ++index) {
    const std::string id = "t" + std::to_string(index);
    tracks.push_back(mono_track(id, buffers[static_cast<std::size_t>(index)]));
    profiles.push_back(profile_of(id));
  }

  const auto start = std::chrono::steady_clock::now();
  const std::vector<assistant::PairAlignment> alignments =
      assistant::analyze_phase_alignment(tracks, profiles);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

  REQUIRE(alignments.size() == kTrackCount * (kTrackCount - 1) / 2);
  // The per-pair cost is what a caller multiplies by the pair count, so it is
  // reported either way rather than only on failure.
  INFO("24 tracks, " << alignments.size() << " pairs: " << elapsed_ms << " ms ("
                     << elapsed_ms / static_cast<double>(alignments.size()) << " ms/pair)");
  REQUIRE(elapsed_ms < kBudgetMs);
}
