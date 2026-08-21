/// @file masking_test.cpp
/// @brief Contract of the pairwise band dominance measurement.

#include "mixing/assistant/masking.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPi;
using sonare::mixing::assistant::analyze_band_dominance;
using sonare::mixing::assistant::analyze_track_profiles;
using sonare::mixing::assistant::BandDominance;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::kBandNames;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::TrackInput;
using sonare::mixing::assistant::TrackProfile;
using sonare::mixing::assistant::TrackProfileConfig;

namespace {

constexpr int kTestSampleRate = 44100;
constexpr int kTestNfft = 2048;
constexpr int kTestHop = 512;

// Long enough to clear TrackProfileConfig::min_duration_sec with margin, short
// enough that a profiling pass over two tracks stays well inside the fast tier.
constexpr float kTestDurationSec = 0.5f;

// Indices into kBands / kBandNames.
constexpr int kLowBand = 1;   // 60-250 Hz
constexpr int kMidBand = 3;   // 500-2000 Hz
constexpr int kHighBand = 5;  // 6000-12000 Hz

// Builds a profile whose band envelope is written directly, so the expected
// dominance can be worked out by hand rather than inferred from an STFT.
TrackProfile make_track(const std::string& id, int n_frames) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.usable = true;
  profile.bands.n_frames = n_frames;
  profile.bands.n_fft = kTestNfft;
  profile.bands.hop_length = kTestHop;
  profile.bands.sample_rate = kTestSampleRate;
  profile.bands.energy.assign(
      static_cast<std::size_t>(kBandCount) * static_cast<std::size_t>(n_frames), 0.0f);
  return profile;
}

void set_band_energy(TrackProfile& profile, int band, int first_frame, int last_frame,
                     float energy) {
  const int n_frames = profile.bands.n_frames;
  for (int frame = first_frame; frame <= last_frame && frame < n_frames; ++frame) {
    profile.bands.energy[static_cast<std::size_t>(band) * static_cast<std::size_t>(n_frames) +
                         static_cast<std::size_t>(frame)] = energy;
  }
}

void scale_track_energy(TrackProfile& profile, float factor) {
  for (float& value : profile.bands.energy) value *= factor;
}

float band_energy_sum(const TrackProfile& profile, int band) {
  float total = 0.0f;
  for (int frame = 0; frame < profile.bands.n_frames; ++frame) {
    total += profile.bands.at(band, frame);
  }
  return total;
}

// Wraps the result the way callers read it, which also pins the flat index
// against MixProfile::dominance_at.
MixProfile measure(const std::vector<TrackProfile>& profiles) {
  MixProfile mix;
  mix.track_count = static_cast<int>(profiles.size());
  mix.dominance = analyze_band_dominance(profiles);
  return mix;
}

// Raw indexing, needed where dominance_at would answer from its own guards
// instead of from the matrix.
BandDominance flat_at(const std::vector<BandDominance>& dominance, std::size_t track_count,
                      std::size_t masker, std::size_t maskee, int band) {
  const std::size_t index = (masker * track_count + maskee) * static_cast<std::size_t>(kBandCount) +
                            static_cast<std::size_t>(band);
  REQUIRE(index < dominance.size());
  return dominance[index];
}

// Deterministic band-limited noise: equal-amplitude partials spread across
// [low_hz, high_hz], their phases stepped by the golden angle so they do not
// all align at t = 0 into a single impulse. Strictly band-limited, so the only
// out-of-band energy an analysis sees is window leakage.
std::vector<float> band_noise(float low_hz, float high_hz, float amplitude, int frame_count) {
  constexpr int kPartialCount = 24;
  constexpr float kGoldenAngleRad = 2.39996323f;

  std::vector<float> samples(static_cast<std::size_t>(frame_count), 0.0f);
  for (int partial = 0; partial < kPartialCount; ++partial) {
    const float position = static_cast<float>(partial) / static_cast<float>(kPartialCount - 1);
    const float hz = low_hz + (high_hz - low_hz) * position;
    const float phase = kGoldenAngleRad * static_cast<float>(partial);
    for (int frame = 0; frame < frame_count; ++frame) {
      const float t = static_cast<float>(frame) / static_cast<float>(kTestSampleRate);
      samples[static_cast<std::size_t>(frame)] += std::sin(kTwoPi * hz * t + phase);
    }
  }
  const float scale = amplitude / static_cast<float>(kPartialCount);
  for (float& value : samples) value *= scale;
  return samples;
}

TrackInput make_input(const std::string& id, const std::vector<float>& samples) {
  TrackInput input;
  input.id = id;
  input.left = samples.data();
  input.frame_count = samples.size();
  input.sample_rate = kTestSampleRate;
  return input;
}

}  // namespace

TEST_CASE("analyze_band_dominance measures the energy share inside a shared band",
          "[mixing][assistant]") {
  constexpr int kFrames = 32;
  // 16:1 in power, i.e. the loud part carries 12 dB more energy in the band.
  constexpr float kLoudEnergy = 16.0f;
  constexpr float kQuietEnergy = 1.0f;

  std::vector<TrackProfile> profiles{make_track("loud", kFrames), make_track("quiet", kFrames)};
  set_band_energy(profiles[0], kMidBand, 0, kFrames - 1, kLoudEnergy);
  set_band_energy(profiles[1], kMidBand, 0, kFrames - 1, kQuietEnergy);

  const MixProfile mix = measure(profiles);
  const BandDominance forward = mix.dominance_at(0, 1, kMidBand);
  const BandDominance reverse = mix.dominance_at(1, 0, kMidBand);

  CHECK(forward.valid_frames == kFrames);
  CHECK(reverse.valid_frames == kFrames);
  CHECK_THAT(forward.ratio, WithinAbs(kLoudEnergy / (kLoudEnergy + kQuietEnergy), 1e-5));
  CHECK_THAT(reverse.ratio, WithinAbs(kQuietEnergy / (kLoudEnergy + kQuietEnergy), 1e-5));
  CHECK(forward.ratio > 0.5f);

  // Bands neither part occupies stay untouched.
  CHECK(mix.dominance_at(0, 1, kLowBand).valid_frames == 0);
  CHECK(mix.dominance_at(0, 1, kHighBand).valid_frames == 0);
}

TEST_CASE("analyze_band_dominance keeps the two directions of a pair asymmetric",
          "[mixing][assistant]") {
  constexpr int kFrames = 24;
  constexpr float kQuietEnergy = 1.0f;

  for (const float energy_ratio : {2.0f, 4.0f, 100.0f}) {
    INFO("energy ratio " << energy_ratio);
    std::vector<TrackProfile> profiles{make_track("loud", kFrames), make_track("quiet", kFrames)};
    set_band_energy(profiles[0], kMidBand, 0, kFrames - 1, kQuietEnergy * energy_ratio);
    set_band_energy(profiles[1], kMidBand, 0, kFrames - 1, kQuietEnergy);

    const MixProfile mix = measure(profiles);
    const BandDominance forward = mix.dominance_at(0, 1, kMidBand);
    const BandDominance reverse = mix.dominance_at(1, 0, kMidBand);

    CHECK(forward.ratio > 0.5f);
    CHECK(reverse.ratio < 0.5f);
    CHECK(forward.valid_frames == reverse.valid_frames);
    // The two shares are taken over one set of frames, so they partition it.
    CHECK_THAT(forward.ratio + reverse.ratio, WithinAbs(1.0, 1e-5));
  }
}

TEST_CASE("analyze_band_dominance reports no interference between distant bands",
          "[mixing][assistant]") {
  constexpr int kFrames = 32;
  constexpr float kEnergy = 4.0f;

  std::vector<TrackProfile> profiles{make_track("bass", kFrames), make_track("air", kFrames)};
  set_band_energy(profiles[0], kLowBand, 0, kFrames - 1, kEnergy);
  set_band_energy(profiles[1], kHighBand, 0, kFrames - 1, kEnergy);

  const MixProfile mix = measure(profiles);
  for (int band = 0; band < kBandCount; ++band) {
    INFO("band " << kBandNames[static_cast<std::size_t>(band)]);
    CHECK(mix.dominance_at(0, 1, band).valid_frames == 0);
    CHECK(mix.dominance_at(1, 0, band).valid_frames == 0);
    CHECK(mix.dominance_at(0, 1, band).ratio == 0.0f);
    CHECK(mix.dominance_at(1, 0, band).ratio == 0.0f);
  }
}

TEST_CASE("analyze_band_dominance excludes frames where the parts do not sound together",
          "[mixing][assistant]") {
  constexpr int kFrames = 40;
  constexpr int kHalf = kFrames / 2;
  constexpr float kEnergy = 2.0f;

  std::vector<TrackProfile> profiles{make_track("early", kFrames), make_track("late", kFrames)};
  set_band_energy(profiles[0], kMidBand, 0, kHalf - 1, kEnergy);
  set_band_energy(profiles[1], kMidBand, kHalf, kFrames - 1, kEnergy);

  // Both parts do occupy the band, and by the same total energy, so a measure
  // that collapsed the time axis before comparing would call this a complete
  // collision. Without this the zero below could come from an empty band.
  REQUIRE(band_energy_sum(profiles[0], kMidBand) > 0.0f);
  CHECK_THAT(band_energy_sum(profiles[0], kMidBand),
             WithinAbs(band_energy_sum(profiles[1], kMidBand), 1e-5));

  const MixProfile mix = measure(profiles);
  CHECK(mix.dominance_at(0, 1, kMidBand).valid_frames == 0);
  CHECK(mix.dominance_at(1, 0, kMidBand).valid_frames == 0);
  CHECK(mix.dominance_at(0, 1, kMidBand).ratio == 0.0f);

  SECTION("frames that do coincide are counted") {
    constexpr int kOverlap = 6;
    set_band_energy(profiles[1], kMidBand, kHalf - kOverlap, kHalf - 1, kEnergy);

    const MixProfile overlapped = measure(profiles);
    CHECK(overlapped.dominance_at(0, 1, kMidBand).valid_frames == kOverlap);
    CHECK_THAT(overlapped.dominance_at(0, 1, kMidBand).ratio, WithinAbs(0.5, 1e-5));
  }
}

TEST_CASE("analyze_band_dominance ignores frames far below a band's own peak",
          "[mixing][assistant]") {
  constexpr int kFrames = 10;
  constexpr int kPeakFrames = 2;
  constexpr int kNearFloorFrames = 2;
  constexpr float kReferenceEnergy = 1.0f;
  constexpr float kPeakEnergy = 1.0f;
  // 30 dB below the band peak: comfortably inside the floor and kept.
  constexpr float kNearFloorEnergy = 1e-3f;
  // 90 dB below the band peak: comfortably outside the floor and dropped.
  constexpr float kBelowFloorEnergy = 1e-9f;

  std::vector<TrackProfile> profiles{make_track("dynamic", kFrames), make_track("steady", kFrames)};
  set_band_energy(profiles[0], kMidBand, 0, kPeakFrames - 1, kPeakEnergy);
  set_band_energy(profiles[0], kMidBand, kPeakFrames, kPeakFrames + kNearFloorFrames - 1,
                  kNearFloorEnergy);
  set_band_energy(profiles[0], kMidBand, kPeakFrames + kNearFloorFrames, kFrames - 1,
                  kBelowFloorEnergy);
  set_band_energy(profiles[1], kMidBand, 0, kFrames - 1, kReferenceEnergy);

  const MixProfile mix = measure(profiles);
  const BandDominance forward = mix.dominance_at(0, 1, kMidBand);
  CHECK(forward.valid_frames == kPeakFrames + kNearFloorFrames);

  const double expected =
      (kPeakFrames * (kPeakEnergy / (kPeakEnergy + kReferenceEnergy)) +
       kNearFloorFrames * (kNearFloorEnergy / (kNearFloorEnergy + kReferenceEnergy))) /
      (kPeakFrames + kNearFloorFrames);
  CHECK_THAT(forward.ratio, WithinAbs(expected, 1e-5));

  SECTION("the floor follows the band's level rather than an absolute threshold") {
    // The same part rendered 60 dB quieter must keep the same frames. An
    // absolute floor would drop most of them and change the figure.
    constexpr float kQuieterRender = 1e-6f;
    scale_track_energy(profiles[0], kQuieterRender);

    const MixProfile quieter = measure(profiles);
    CHECK(quieter.dominance_at(0, 1, kMidBand).valid_frames == kPeakFrames + kNearFloorFrames);
  }
}

TEST_CASE("analyze_band_dominance compares tracks of different lengths", "[mixing][assistant]") {
  constexpr int kLongFrames = 48;
  constexpr int kShortFrames = 12;
  constexpr float kEnergy = 3.0f;

  std::vector<TrackProfile> profiles{make_track("long", kLongFrames),
                                     make_track("short", kShortFrames)};
  set_band_energy(profiles[0], kMidBand, 0, kLongFrames - 1, kEnergy);
  set_band_energy(profiles[1], kMidBand, 0, kShortFrames - 1, kEnergy);

  MixProfile mix;
  REQUIRE_NOTHROW(mix = measure(profiles));

  // Frames past the short track's end read as silence, so only its own span
  // counts, and the two carry the same energy where they do overlap.
  CHECK(mix.dominance_at(0, 1, kMidBand).valid_frames == kShortFrames);
  CHECK(mix.dominance_at(1, 0, kMidBand).valid_frames == kShortFrames);
  CHECK_THAT(mix.dominance_at(0, 1, kMidBand).ratio, WithinAbs(0.5, 1e-5));
}

TEST_CASE("analyze_band_dominance leaves the diagonal default-constructed", "[mixing][assistant]") {
  constexpr int kFrames = 16;
  constexpr float kEnergy = 5.0f;

  std::vector<TrackProfile> profiles{make_track("a", kFrames), make_track("b", kFrames)};
  set_band_energy(profiles[0], kMidBand, 0, kFrames - 1, kEnergy);
  set_band_energy(profiles[1], kMidBand, 0, kFrames - 1, kEnergy);

  const std::vector<BandDominance> dominance = analyze_band_dominance(profiles);
  REQUIRE(dominance.size() == profiles.size() * profiles.size() * kBandCount);

  // Read the matrix directly: dominance_at() answers the diagonal from its own
  // guard, which would hide a self-masking entry rather than expose it.
  for (std::size_t track = 0; track < profiles.size(); ++track) {
    for (int band = 0; band < kBandCount; ++band) {
      INFO("track " << track << " band " << kBandNames[static_cast<std::size_t>(band)]);
      const BandDominance self = flat_at(dominance, profiles.size(), track, track, band);
      CHECK(self.valid_frames == 0);
      CHECK(self.ratio == 0.0f);
    }
  }

  // Non-vacuity: the off-diagonal pair in the shared band was measured.
  CHECK(flat_at(dominance, profiles.size(), 0, 1, kMidBand).valid_frames == kFrames);
}

TEST_CASE("analyze_band_dominance leaves an unusable track's row and column empty",
          "[mixing][assistant]") {
  constexpr int kFrames = 20;
  constexpr float kEnergy = 2.0f;

  std::vector<TrackProfile> profiles{make_track("keeper", kFrames), make_track("dropped", kFrames),
                                     make_track("other", kFrames)};
  for (TrackProfile& profile : profiles) {
    set_band_energy(profile, kMidBand, 0, kFrames - 1, kEnergy);
  }
  profiles[1].usable = false;
  profiles[1].exclusion_reason = "silent";

  const MixProfile mix = measure(profiles);
  for (int band = 0; band < kBandCount; ++band) {
    INFO("band " << kBandNames[static_cast<std::size_t>(band)]);
    for (int other : {0, 2}) {
      CHECK(mix.dominance_at(1, other, band).valid_frames == 0);
      CHECK(mix.dominance_at(other, 1, band).valid_frames == 0);
      CHECK(mix.dominance_at(1, other, band).ratio == 0.0f);
      CHECK(mix.dominance_at(other, 1, band).ratio == 0.0f);
    }
  }

  // Non-vacuity: the two usable tracks were still measured against each other.
  CHECK(mix.dominance_at(0, 2, kMidBand).valid_frames == kFrames);
}

TEST_CASE("analyze_band_dominance accepts degenerate track counts", "[mixing][assistant]") {
  SECTION("no tracks") {
    std::vector<BandDominance> dominance;
    REQUIRE_NOTHROW(dominance = analyze_band_dominance({}));
    CHECK(dominance.empty());
  }

  SECTION("one track") {
    constexpr int kFrames = 8;
    std::vector<TrackProfile> profiles{make_track("alone", kFrames)};
    set_band_energy(profiles[0], kMidBand, 0, kFrames - 1, 1.0f);

    std::vector<BandDominance> dominance;
    REQUIRE_NOTHROW(dominance = analyze_band_dominance(profiles));
    REQUIRE(dominance.size() == kBandCount);
    for (const BandDominance& entry : dominance) {
      CHECK(entry.valid_frames == 0);
      CHECK(entry.ratio == 0.0f);
    }
  }
}

TEST_CASE("analyze_band_dominance finds a contested band in profiled audio",
          "[mixing][assistant]") {
  const int frame_count = static_cast<int>(static_cast<float>(kTestSampleRate) * kTestDurationSec);
  // Same band, 12 dB apart in level, which is 16:1 in energy.
  const std::vector<float> loud_samples = band_noise(600.0f, 1800.0f, 0.5f, frame_count);
  const std::vector<float> quiet_samples = band_noise(600.0f, 1800.0f, 0.125f, frame_count);

  const std::vector<TrackInput> inputs{make_input("loud", loud_samples),
                                       make_input("quiet", quiet_samples)};
  TrackProfileConfig config;
  config.n_fft = kTestNfft;
  config.hop_length = kTestHop;

  const std::vector<TrackProfile> profiles = analyze_track_profiles(inputs, config);
  REQUIRE(profiles.size() == 2);
  REQUIRE(profiles[0].usable);
  REQUIRE(profiles[1].usable);

  const MixProfile mix = measure(profiles);
  const BandDominance forward = mix.dominance_at(0, 1, kMidBand);
  const BandDominance reverse = mix.dominance_at(1, 0, kMidBand);

  CHECK(forward.valid_frames > 0);
  CHECK(forward.ratio > 0.8f);
  CHECK(reverse.ratio < 0.2f);
  CHECK_THAT(forward.ratio + reverse.ratio, WithinAbs(1.0, 1e-5));
}

TEST_CASE("analyze_band_dominance finds no contest between separated sources in profiled audio",
          "[mixing][assistant]") {
  const int frame_count = static_cast<int>(static_cast<float>(kTestSampleRate) * kTestDurationSec);
  const std::vector<float> low_samples = band_noise(80.0f, 220.0f, 0.5f, frame_count);
  const std::vector<float> high_samples = band_noise(7000.0f, 11000.0f, 0.5f, frame_count);

  const std::vector<TrackInput> inputs{make_input("low", low_samples),
                                       make_input("high", high_samples)};
  TrackProfileConfig config;
  config.n_fft = kTestNfft;
  config.hop_length = kTestHop;

  const std::vector<TrackProfile> profiles = analyze_track_profiles(inputs, config);
  REQUIRE(profiles.size() == 2);
  REQUIRE(profiles[0].usable);
  REQUIRE(profiles[1].usable);

  const MixProfile mix = measure(profiles);

  // In each source's own band the pair either never meets or is owned outright;
  // neither band settles anywhere near the 0.5 stand-off of a real collision.
  const BandDominance in_low = mix.dominance_at(0, 1, kLowBand);
  CHECK((in_low.valid_frames == 0 || in_low.ratio > 0.9f));
  const BandDominance in_high = mix.dominance_at(0, 1, kHighBand);
  CHECK((in_high.valid_frames == 0 || in_high.ratio < 0.1f));
}
