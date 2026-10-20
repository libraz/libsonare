/// @file masking_outlier_reference_test.cpp
/// @brief One loud event in another band must not move a track's band floors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <vector>

#include "mixing/assistant/masking.h"
#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::assistant::analyze_band_dominance;
using sonare::mixing::assistant::BandDominance;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::TrackProfile;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kNfft = 2048;
constexpr int kHop = 512;
// About 14 s of frames: long enough for a robust reference to set one event aside.
constexpr int kFrames = 1200;
constexpr int kSharedBand = 3;  // 500-2000 Hz
constexpr int kBurstBand = 5;   // 6000-12000 Hz
constexpr int kBurstFrame = 600;
// +40 dB in power.
constexpr float kBurstGain = 1.0e4f;

TrackProfile steady_track(bool with_burst) {
  TrackProfile profile;
  profile.usable = true;
  profile.bands.n_frames = kFrames;
  profile.bands.n_fft = kNfft;
  profile.bands.hop_length = kHop;
  profile.bands.sample_rate = kSampleRate;
  profile.duration_sec = static_cast<float>(kFrames) * kHop / kSampleRate;
  profile.bands.energy.assign(static_cast<std::size_t>(kBandCount) * kFrames, 0.0f);
  // A small deterministic ripple, so the band has local peaks like real material.
  for (int frame = 0; frame < kFrames; ++frame) {
    const float level = (frame % 3 == 0) ? 1.2f : 1.0f;
    profile.bands.energy[static_cast<std::size_t>(kSharedBand) * kFrames + frame] = level;
  }
  if (with_burst) {
    profile.bands.energy[static_cast<std::size_t>(kBurstBand) * kFrames + kBurstFrame] = kBurstGain;
  }
  return profile;
}

BandDominance shared_band_dominance(bool with_burst) {
  const std::vector<TrackProfile> profiles = {steady_track(with_burst), steady_track(false)};
  const std::vector<BandDominance> dominance = analyze_band_dominance(profiles, {});
  const std::size_t index = (0 * 2 + 1) * static_cast<std::size_t>(kBandCount) + kSharedBand;
  REQUIRE(index < dominance.size());
  return dominance[index];
}

}  // namespace

TEST_CASE("masking positive control: equal steady parts share the band evenly",
          "[mixing][assistant]") {
  const BandDominance control = shared_band_dominance(false);
  REQUIRE(control.valid_frames == kFrames);
  REQUIRE_THAT(control.ratio, WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("masking keeps a shared band's overlap when one track has a loud burst in another band",
          "[mixing][assistant]") {
  const BandDominance control = shared_band_dominance(false);
  const BandDominance with_burst = shared_band_dominance(true);

  // The burst sets the track-wide occupancy floor 35 dB under itself, which sits
  // above the steady band, so every frame of the shared band is dropped.
  REQUIRE(with_burst.valid_frames == control.valid_frames);
  REQUIRE_THAT(with_burst.ratio, WithinAbs(control.ratio, 1e-4f));
}
