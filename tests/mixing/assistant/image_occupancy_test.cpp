/// @file image_occupancy_test.cpp
/// @brief Contract of the mixing assistant's stereo image occupancy and
///        mono-fold risk analysis.

#include "mixing/assistant/image_occupancy.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "mixing/meter.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::kMaxMonoCompatWidth;
using sonare::mixing::assistant::analyze_image_occupancy;
using sonare::mixing::assistant::analyze_mono_risks;
using sonare::mixing::assistant::ImageOccupancy;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::kBandNames;
using sonare::mixing::assistant::kPanBucketCount;
using sonare::mixing::assistant::MonoRisk;
using sonare::mixing::assistant::TrackInput;
using sonare::mixing::assistant::TrackProfile;
using sonare::mixing::assistant::TrackProfileConfig;

namespace {

constexpr int kSampleRate = 48000;

// 0.5 s. Long enough for several dozen hops at the default analysis geometry and
// short enough that the whole file stays inside the default test budget.
constexpr std::size_t kFrameCount = 24000;

// Test tones sit in the middle of their band, far enough from either edge that
// window leakage cannot move the band a tone is measured in.
constexpr float kLowToneHz = 120.0f;        // kBands[1], low (60-250 Hz)
constexpr float kSecondLowToneHz = 180.0f;  // kBands[1] as well
constexpr float kMidToneHz = 1000.0f;       // kBands[3], mid (500-2000 Hz)
constexpr float kHighMidToneHz = 3000.0f;   // kBands[4], highMid (2-6 kHz)
constexpr float kSecondHighMidToneHz = 4000.0f;

constexpr int kLowBand = 1;
constexpr int kHighMidBand = 4;
constexpr int kCentreBucket = kPanBucketCount / 2;
constexpr int kHardLeftBucket = 0;
constexpr int kHardRightBucket = kPanBucketCount - 1;

constexpr std::size_t kHistogramSize =
    static_cast<std::size_t>(kBandCount) * static_cast<std::size_t>(kPanBucketCount);

std::vector<float> silence() { return std::vector<float>(kFrameCount, 0.0f); }

void add_tone(std::vector<float>& buffer, float hz, float amplitude) {
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    const double phase = sonare::constants::kTwoPiD * static_cast<double>(hz) *
                         static_cast<double>(i) / static_cast<double>(kSampleRate);
    buffer[i] += amplitude * static_cast<float>(std::sin(phase));
  }
}

std::vector<float> tone(float hz, float amplitude) {
  std::vector<float> buffer = silence();
  add_tone(buffer, hz, amplitude);
  return buffer;
}

// The profiles are hand-built rather than measured: these cases pin the image
// analysis, and only the fields it reads (usable, strip_id, analysis geometry)
// matter to it.
TrackProfile usable_profile(const std::string& id, int channel_count) {
  const TrackProfileConfig geometry;
  TrackProfile profile;
  profile.strip_id = id;
  profile.usable = true;
  profile.channel_count = channel_count;
  profile.bands.n_fft = geometry.n_fft;
  profile.bands.hop_length = geometry.hop_length;
  profile.bands.sample_rate = kSampleRate;
  return profile;
}

TrackInput make_track(const std::string& id, const std::vector<float>& left,
                      const std::vector<float>* right) {
  TrackInput track;
  track.id = id;
  track.left = left.data();
  track.right = right != nullptr ? right->data() : nullptr;
  track.frame_count = left.size();
  track.sample_rate = kSampleRate;
  return track;
}

void require_empty_image(const ImageOccupancy& image) {
  REQUIRE(image.histogram.size() == kHistogramSize);
  REQUIRE(image.crowding.size() == static_cast<std::size_t>(kBandCount));
  REQUIRE(image.crowded.size() == static_cast<std::size_t>(kBandCount));
  for (float value : image.histogram) REQUIRE_THAT(value, WithinAbs(0.0f, 1e-12f));
  for (float value : image.crowding) REQUIRE_THAT(value, WithinAbs(0.0f, 1e-12f));
  for (bool flag : image.crowded) REQUIRE_FALSE(flag);
}

}  // namespace

TEST_CASE("Centred mono tracks pile into the middle pan bucket", "[mixing][assistant]") {
  std::vector<float> first = tone(kLowToneHz, 0.5f);
  add_tone(first, kHighMidToneHz, 0.5f);
  std::vector<float> second = tone(kSecondLowToneHz, 0.5f);
  add_tone(second, kSecondHighMidToneHz, 0.5f);

  const std::vector<TrackInput> tracks = {make_track("a", first, nullptr),
                                          make_track("b", second, nullptr)};
  const std::vector<TrackProfile> profiles = {usable_profile("a", 1), usable_profile("b", 1)};

  const ImageOccupancy image = analyze_image_occupancy(tracks, profiles);

  // A mono track has one position, so every occupied band collapses onto centre.
  REQUIRE_THAT(image.at(kLowBand, kCentreBucket), WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(image.at(kHighMidBand, kCentreBucket), WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(image.crowding[kLowBand], WithinAbs(1.0f, 1e-5f));
  REQUIRE_THAT(image.crowding[kHighMidBand], WithinAbs(1.0f, 1e-5f));
  REQUIRE(image.crowded[kLowBand]);
  REQUIRE(image.crowded[kHighMidBand]);
}

TEST_CASE("Hard panning the same sources relieves the crowding", "[mixing][assistant]") {
  std::vector<float> first = tone(kLowToneHz, 0.5f);
  add_tone(first, kHighMidToneHz, 0.5f);
  std::vector<float> second = tone(kSecondLowToneHz, 0.5f);
  add_tone(second, kSecondHighMidToneHz, 0.5f);
  const std::vector<float> mute = silence();

  const std::vector<TrackInput> centred = {make_track("a", first, nullptr),
                                           make_track("b", second, nullptr)};
  const std::vector<TrackProfile> mono_profiles = {usable_profile("a", 1), usable_profile("b", 1)};

  // The same material, one source hard left and the other hard right.
  const std::vector<TrackInput> panned = {make_track("a", first, &mute),
                                          make_track("b", mute, &second)};
  const std::vector<TrackProfile> stereo_profiles = {usable_profile("a", 2),
                                                     usable_profile("b", 2)};

  const ImageOccupancy centred_image = analyze_image_occupancy(centred, mono_profiles);
  const ImageOccupancy panned_image = analyze_image_occupancy(panned, stereo_profiles);

  REQUIRE(panned_image.crowding[kLowBand] < centred_image.crowding[kLowBand]);
  REQUIRE(panned_image.crowding[kHighMidBand] < centred_image.crowding[kHighMidBand]);
  REQUIRE_FALSE(panned_image.crowded[kLowBand]);
  REQUIRE_FALSE(panned_image.crowded[kHighMidBand]);
  REQUIRE_THAT(panned_image.at(kLowBand, kHardLeftBucket), WithinAbs(0.5f, 0.05f));
  REQUIRE_THAT(panned_image.at(kLowBand, kHardRightBucket), WithinAbs(0.5f, 0.05f));
  REQUIRE_THAT(panned_image.at(kLowBand, kCentreBucket), WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("One stereo track occupies a different bucket per band", "[mixing][assistant]") {
  // Low content shared equally by both channels, high content on the right only:
  // the two bands of one track have to land in different buckets.
  const std::vector<float> left = tone(kLowToneHz, 0.5f);
  std::vector<float> right = tone(kLowToneHz, 0.5f);
  add_tone(right, kHighMidToneHz, 0.5f);

  const std::vector<TrackInput> tracks = {make_track("wide", left, &right)};
  const std::vector<TrackProfile> profiles = {usable_profile("wide", 2)};

  const ImageOccupancy image = analyze_image_occupancy(tracks, profiles);

  REQUIRE_THAT(image.at(kLowBand, kCentreBucket), WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(image.at(kHighMidBand, kHardRightBucket), WithinAbs(1.0f, 1e-4f));
  REQUIRE_THAT(image.at(kHighMidBand, kCentreBucket), WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Every occupied band of the image histogram sums to one", "[mixing][assistant]") {
  std::vector<float> centred = tone(kLowToneHz, 0.5f);
  add_tone(centred, kMidToneHz, 0.5f);
  std::vector<float> wide_left = tone(kSecondLowToneHz, 0.4f);
  std::vector<float> wide_right = tone(kSecondLowToneHz, 0.4f);
  add_tone(wide_right, kHighMidToneHz, 0.6f);
  const std::vector<float> mute = silence();

  const std::vector<TrackInput> tracks = {make_track("centred", centred, nullptr),
                                          make_track("wide", wide_left, &wide_right),
                                          make_track("left", centred, &mute)};
  const std::vector<TrackProfile> profiles = {usable_profile("centred", 1),
                                              usable_profile("wide", 2), usable_profile("left", 2)};

  const ImageOccupancy image = analyze_image_occupancy(tracks, profiles);
  REQUIRE(image.histogram.size() == kHistogramSize);

  for (int band = 0; band < kBandCount; ++band) {
    float sum = 0.0f;
    for (int bucket = 0; bucket < kPanBucketCount; ++bucket) sum += image.at(band, bucket);
    // A band is either normalized to one or carries no energy at all; nothing in
    // between is a valid histogram row.
    const bool unoccupied = std::abs(sum) < 1e-6f;
    const bool normalized = std::abs(sum - 1.0f) < 1e-4f;
    INFO("band " << kBandNames[band] << " sums to " << sum);
    REQUIRE((unoccupied || normalized));
  }
}

TEST_CASE("A polarity-inverted track is reported as a mono risk", "[mixing][assistant]") {
  const std::vector<float> centre = tone(kMidToneHz, 0.5f);
  const std::vector<float> inverted = tone(kMidToneHz, -0.5f);
  const std::vector<float> duplicate = centre;

  const std::vector<TrackInput> tracks = {make_track("mono", centre, nullptr),
                                          make_track("flipped", centre, &inverted),
                                          make_track("correlated", centre, &duplicate)};
  const std::vector<TrackProfile> profiles = {
      usable_profile("mono", 1), usable_profile("flipped", 2), usable_profile("correlated", 2)};

  const std::vector<MonoRisk> risks = analyze_mono_risks(tracks, profiles);

  // The mono track has no image to lose and the correlated one folds cleanly, so
  // only the inverted pair is reported.
  REQUIRE(risks.size() == 1);
  REQUIRE(risks[0].track_index == 1);
  REQUIRE(risks[0].strip_id == "flipped");
  REQUIRE_THAT(risks[0].correlation, WithinAbs(-1.0f, 1e-4f));
  // The mid collapses outright, so the reported width is the finite sentinel
  // standing in for an infinite side/mid ratio.
  REQUIRE_THAT(risks[0].width, WithinAbs(kMaxMonoCompatWidth, 1.0f));
  // The tone sits in the mid band: the low-end flag is a separate detection and
  // must not ride along with a broadband verdict.
  REQUIRE_FALSE(risks[0].wide_low_end);
}

TEST_CASE("A track that is wide only in the low end raises the low-end flag",
          "[mixing][assistant]") {
  std::vector<float> left = tone(kLowToneHz, 0.5f);
  add_tone(left, kHighMidToneHz, 0.5f);
  // Same material with the low tone in opposite polarity and the top end shared.
  std::vector<float> right = tone(kLowToneHz, -0.5f);
  add_tone(right, kHighMidToneHz, 0.5f);

  std::vector<float> safe_left = tone(kLowToneHz, 0.5f);
  add_tone(safe_left, kHighMidToneHz, 0.5f);
  const std::vector<float> safe_right = safe_left;

  const std::vector<TrackInput> tracks = {make_track("lows", left, &right),
                                          make_track("safe", safe_left, &safe_right)};
  const std::vector<TrackProfile> profiles = {usable_profile("lows", 2), usable_profile("safe", 2)};

  const std::vector<MonoRisk> risks = analyze_mono_risks(tracks, profiles);

  REQUIRE(risks.size() == 1);
  REQUIRE(risks[0].track_index == 0);
  REQUIRE(risks[0].strip_id == "lows");
  REQUIRE(risks[0].wide_low_end);
}

TEST_CASE("Degenerate track lists analyse to an empty result", "[mixing][assistant]") {
  SECTION("no tracks at all") {
    const std::vector<TrackInput> tracks;
    const std::vector<TrackProfile> profiles;

    ImageOccupancy image;
    REQUIRE_NOTHROW(image = analyze_image_occupancy(tracks, profiles));
    require_empty_image(image);
    REQUIRE(analyze_mono_risks(tracks, profiles).empty());
  }

  SECTION("every track excluded by its profile") {
    const std::vector<float> samples = tone(kMidToneHz, 0.5f);
    const std::vector<float> other = tone(kLowToneHz, -0.5f);
    const std::vector<TrackInput> tracks = {make_track("a", samples, nullptr),
                                            make_track("b", samples, &other)};
    std::vector<TrackProfile> profiles = {usable_profile("a", 1), usable_profile("b", 2)};
    for (TrackProfile& profile : profiles) {
      profile.usable = false;
      profile.exclusion_reason = "silent";
    }

    ImageOccupancy image;
    REQUIRE_NOTHROW(image = analyze_image_occupancy(tracks, profiles));
    require_empty_image(image);
    REQUIRE(analyze_mono_risks(tracks, profiles).empty());
  }

  SECTION("a usable profile pointing at a null buffer") {
    TrackInput broken;
    broken.id = "broken";
    broken.frame_count = kFrameCount;
    broken.sample_rate = kSampleRate;
    const std::vector<TrackInput> tracks = {broken};
    const std::vector<TrackProfile> profiles = {usable_profile("broken", 2)};

    ImageOccupancy image;
    REQUIRE_NOTHROW(image = analyze_image_occupancy(tracks, profiles));
    require_empty_image(image);
    REQUIRE(analyze_mono_risks(tracks, profiles).empty());
  }

  SECTION("fewer profiles than tracks") {
    const std::vector<float> samples = tone(kMidToneHz, 0.5f);
    const std::vector<TrackInput> tracks = {make_track("a", samples, nullptr),
                                            make_track("b", samples, nullptr)};
    const std::vector<TrackProfile> profiles;

    ImageOccupancy image;
    REQUIRE_NOTHROW(image = analyze_image_occupancy(tracks, profiles));
    require_empty_image(image);
    REQUIRE(analyze_mono_risks(tracks, profiles).empty());
  }
}
