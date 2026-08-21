/// @file track_profile_test.cpp
/// @brief Mixing assistant per-track profiling tests.

#include "mixing/assistant/track_profile.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "util/constants.h"

namespace assistant = sonare::mixing::assistant;

namespace {

using sonare::constants::kTwoPi;

constexpr int kSampleRate = 48000;

std::vector<float> tone(float seconds, float frequency, float amplitude = 0.4f) {
  std::vector<float> samples(static_cast<std::size_t>(seconds * static_cast<float>(kSampleRate)),
                             0.0f);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = amplitude * std::sin(kTwoPi * frequency * t);
  }
  return samples;
}

// Sums a few partials inside one analysis band. Deterministic stand-in for
// band-limited noise: a test that asserts on band shares needs the excitation
// to be reproducible run to run.
std::vector<float> band_limited(float seconds, const std::vector<float>& frequencies,
                                float amplitude = 0.2f) {
  std::vector<float> samples(static_cast<std::size_t>(seconds * static_cast<float>(kSampleRate)),
                             0.0f);
  for (float frequency : frequencies) {
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
      samples[i] += amplitude * std::sin(kTwoPi * frequency * t);
    }
  }
  return samples;
}

std::vector<float> silence(float seconds) {
  return std::vector<float>(static_cast<std::size_t>(seconds * static_cast<float>(kSampleRate)),
                            0.0f);
}

assistant::TrackInput mono_track(const std::string& id, const std::vector<float>& samples) {
  assistant::TrackInput track;
  track.id = id;
  track.left = samples.data();
  track.frame_count = samples.size();
  track.sample_rate = kSampleRate;
  return track;
}

assistant::TrackInput stereo_track(const std::string& id, const std::vector<float>& left,
                                   const std::vector<float>& right) {
  assistant::TrackInput track = mono_track(id, left);
  track.right = right.data();
  return track;
}

// Resolves a band by its published identifier so the assertions below do not
// hardcode positions in kBands.
int band_index(const std::string& name) {
  for (int band = 0; band < assistant::kBandCount; ++band) {
    if (name == assistant::kBandNames[static_cast<std::size_t>(band)]) return band;
  }
  return -1;
}

int dominant_band(const assistant::TrackProfile& profile) {
  const auto largest =
      std::max_element(profile.band_occupancy.begin(), profile.band_occupancy.end());
  return static_cast<int>(std::distance(profile.band_occupancy.begin(), largest));
}

// The STFT is centre-padded, so a signal of n samples yields 1 + n / hop frames.
int expected_frames(std::size_t frame_count, int hop_length) {
  return 1 + static_cast<int>(frame_count / static_cast<std::size_t>(hop_length));
}

// STFT geometry the hand-built spectra below are written against: bins land
// every 23.4375 Hz, so bin 2 spans 35.15625 Hz to 58.59375 Hz.
constexpr int kSpectrumNFft = 2048;
constexpr float kBinHz = static_cast<float>(kSampleRate) / static_cast<float>(kSpectrumNFft);

// A spectrum assembled by hand, so a degenerate case can be built directly
// rather than searched for in some signal that happens to produce it.
assistant::MeanPowerSpectrum hand_spectrum(const std::vector<float>& power) {
  assistant::MeanPowerSpectrum spectrum;
  spectrum.n_bins = static_cast<int>(power.size());
  spectrum.n_fft = kSpectrumNFft;
  spectrum.sample_rate = kSampleRate;
  spectrum.power = power;
  return spectrum;
}

}  // namespace

TEST_CASE("Track profile band occupancy follows a band-limited source", "[mixing][assistant]") {
  const std::vector<float> low = tone(0.5f, 100.0f);
  const std::vector<float> high = band_limited(0.5f, {8000.0f, 9000.0f, 10000.0f, 11000.0f});

  const std::vector<assistant::TrackInput> tracks = {mono_track("bass", low),
                                                     mono_track("hat", high)};
  const std::vector<assistant::TrackProfile> profiles = assistant::analyze_track_profiles(tracks);

  REQUIRE(profiles.size() == 2);
  REQUIRE(profiles[0].usable);
  REQUIRE(profiles[1].usable);

  const int low_band = band_index("low");
  const int high_band = band_index("high");
  REQUIRE(low_band >= 0);
  REQUIRE(high_band >= 0);

  CHECK(dominant_band(profiles[0]) == low_band);
  CHECK(profiles[0].band_occupancy[static_cast<std::size_t>(low_band)] > 0.8f);
  CHECK(dominant_band(profiles[1]) == high_band);
  CHECK(profiles[1].band_occupancy[static_cast<std::size_t>(high_band)] > 0.8f);

  // Occupancy is a normalized share, so a non-silent track sums to 1.
  float sum = 0.0f;
  for (float share : profiles[0].band_occupancy) sum += share;
  CHECK(std::abs(sum - 1.0f) < 1e-4f);
}

TEST_CASE("Track profile keeps each track's own frame count", "[mixing][assistant]") {
  const std::vector<float> quarter = tone(0.25f, 220.0f);
  const std::vector<float> half = tone(0.5f, 330.0f);
  const std::vector<float> longer = tone(0.75f, 440.0f);

  const std::vector<assistant::TrackInput> tracks = {
      mono_track("a", quarter), mono_track("b", half), mono_track("c", longer)};
  assistant::TrackProfileConfig config;
  const std::vector<assistant::TrackProfile> profiles =
      assistant::analyze_track_profiles(tracks, config);

  REQUIRE(profiles.size() == 3);
  CHECK(profiles[0].bands.n_frames == expected_frames(quarter.size(), config.hop_length));
  CHECK(profiles[1].bands.n_frames == expected_frames(half.size(), config.hop_length));
  CHECK(profiles[2].bands.n_frames == expected_frames(longer.size(), config.hop_length));
  CHECK(profiles[0].bands.n_frames < profiles[1].bands.n_frames);
  CHECK(profiles[1].bands.n_frames < profiles[2].bands.n_frames);

  // Reading past a shorter track's end is silence, not an error.
  CHECK(profiles[0].bands.at(0, profiles[2].bands.n_frames - 1) == 0.0f);
  CHECK(profiles[0].bands.at(-1, 0) == 0.0f);
}

TEST_CASE("Track profile shares one STFT geometry across every track", "[mixing][assistant]") {
  const std::vector<float> mono = tone(0.5f, 220.0f);
  const std::vector<float> left = tone(0.3f, 300.0f);
  const std::vector<float> right = tone(0.3f, 700.0f);

  assistant::TrackProfileConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  const std::vector<assistant::TrackInput> tracks = {mono_track("mono", mono),
                                                     stereo_track("stereo", left, right)};
  const std::vector<assistant::TrackProfile> profiles =
      assistant::analyze_track_profiles(tracks, config);

  REQUIRE(profiles.size() == 2);
  for (const assistant::TrackProfile& profile : profiles) {
    CHECK(profile.bands.n_fft == config.n_fft);
    CHECK(profile.bands.hop_length == config.hop_length);
    CHECK(profile.bands.sample_rate == kSampleRate);
  }
  CHECK(profiles[0].channel_count == 1);
  CHECK(profiles[1].channel_count == 2);
}

TEST_CASE("Track profile excludes a silent track", "[mixing][assistant]") {
  const std::vector<float> quiet = silence(0.5f);
  const assistant::TrackProfile profile =
      assistant::analyze_track_profile(mono_track("mute", quiet));

  CHECK_FALSE(profile.usable);
  CHECK_FALSE(profile.exclusion_reason.empty());
  CHECK(profile.strip_id == "mute");
  // A silent track has no energy to distribute, so every share stays zero.
  for (float share : profile.band_occupancy) CHECK(share == 0.0f);
}

TEST_CASE("Track profile excludes a track shorter than the measurable minimum",
          "[mixing][assistant]") {
  assistant::TrackProfileConfig config;
  const std::vector<float> brief = tone(0.1f, 440.0f);
  REQUIRE(static_cast<float>(brief.size()) / static_cast<float>(kSampleRate) <
          config.min_duration_sec);

  const assistant::TrackProfile profile =
      assistant::analyze_track_profile(mono_track("blip", brief), config);

  CHECK_FALSE(profile.usable);
  CHECK_FALSE(profile.exclusion_reason.empty());
  CHECK(profile.duration_sec > 0.0f);
}

TEST_CASE("Track profile reports degenerate input without throwing", "[mixing][assistant]") {
  const std::vector<float> samples = tone(0.5f, 440.0f);

  SECTION("null buffer") {
    assistant::TrackInput track;
    track.id = "null";
    track.name = "no buffer";
    track.frame_count = samples.size();
    track.sample_rate = kSampleRate;

    assistant::TrackProfile profile;
    REQUIRE_NOTHROW(profile = assistant::analyze_track_profile(track));
    CHECK_FALSE(profile.usable);
    CHECK_FALSE(profile.exclusion_reason.empty());
    CHECK(profile.strip_id == "null");
    CHECK(profile.name == "no buffer");
    CHECK(profile.bands.n_frames == 0);
  }

  SECTION("zero frame count") {
    assistant::TrackInput track = mono_track("empty", samples);
    track.frame_count = 0;

    assistant::TrackProfile profile;
    REQUIRE_NOTHROW(profile = assistant::analyze_track_profile(track));
    CHECK_FALSE(profile.usable);
    CHECK_FALSE(profile.exclusion_reason.empty());
  }

  SECTION("non-positive sample rate") {
    assistant::TrackInput track = mono_track("no-rate", samples);
    track.sample_rate = 0;

    assistant::TrackProfile profile;
    REQUIRE_NOTHROW(profile = assistant::analyze_track_profile(track));
    CHECK_FALSE(profile.usable);
    CHECK_FALSE(profile.exclusion_reason.empty());
    CHECK(profile.strip_id == "no-rate");
  }

  SECTION("degenerate track in a batch leaves the others alone") {
    assistant::TrackInput broken;
    broken.id = "broken";
    broken.sample_rate = -1;

    const std::vector<assistant::TrackInput> tracks = {broken, mono_track("good", samples)};
    std::vector<assistant::TrackProfile> profiles;
    REQUIRE_NOTHROW(profiles = assistant::analyze_track_profiles(tracks));
    REQUIRE(profiles.size() == 2);
    CHECK_FALSE(profiles[0].usable);
    CHECK(profiles[1].usable);
  }
}

TEST_CASE("Track profile keeps a time-averaged spectrum finer than the bands",
          "[mixing][assistant]") {
  // 150 Hz sits inside the low band, which spans 60 Hz to 250 Hz. The bands
  // cannot say whether the track has anything under 80 Hz; the spectrum can.
  const std::vector<float> low = tone(0.5f, 150.0f);
  assistant::TrackProfileConfig config;
  const assistant::TrackProfile profile =
      assistant::analyze_track_profile(mono_track("bass", low), config);

  REQUIRE(profile.usable);
  REQUIRE(profile.spectrum.n_bins == config.n_fft / 2 + 1);
  CHECK(profile.spectrum.power.size() == static_cast<std::size_t>(profile.spectrum.n_bins));
  CHECK(profile.spectrum.n_fft == config.n_fft);
  CHECK(profile.spectrum.sample_rate == kSampleRate);

  // The tone is above 80 Hz and below 250 Hz, so those two questions have
  // opposite answers even though both corners fall inside one analysis band.
  CHECK(profile.spectrum.energy_share_below(80.0f) < 0.05f);
  CHECK(profile.spectrum.energy_share_below(250.0f) > 0.9f);
  // Everything is below Nyquist.
  CHECK(profile.spectrum.energy_share_below(0.5f * static_cast<float>(kSampleRate)) > 0.99f);
}

TEST_CASE("Energy share below a frequency splits the bin the frequency falls in",
          "[mixing][assistant]") {
  // All the energy in bin 2, which spans 1.5 to 2.5 bin widths.
  const assistant::MeanPowerSpectrum spectrum = hand_spectrum({0.0f, 0.0f, 1.0f, 0.0f, 0.0f});

  CHECK(spectrum.energy_share_below(1.5f * kBinHz) == 0.0f);
  CHECK(spectrum.energy_share_below(2.5f * kBinHz) == 1.0f);
  // The bin's own centre halves it, and the share moves smoothly rather than in
  // bin-sized steps: every corner the assistant asks about falls inside a bin.
  CHECK(std::abs(spectrum.energy_share_below(2.0f * kBinHz) - 0.5f) < 1e-5f);
  CHECK(std::abs(spectrum.energy_share_below(1.75f * kBinHz) - 0.25f) < 1e-5f);
  CHECK(spectrum.energy_share_below(1.9f * kBinHz) < spectrum.energy_share_below(2.1f * kBinHz));
}

TEST_CASE("Energy share below a frequency weighs the bins by their energy", "[mixing][assistant]") {
  // Bin 1 carries a quarter of the energy and bin 4 the rest.
  const assistant::MeanPowerSpectrum spectrum = hand_spectrum({0.0f, 1.0f, 0.0f, 0.0f, 3.0f});

  CHECK(std::abs(spectrum.energy_share_below(3.0f * kBinHz) - 0.25f) < 1e-5f);
  CHECK(spectrum.energy_share_below(10.0f * kBinHz) == 1.0f);
}

TEST_CASE("Energy share below a frequency reports the degenerate cases as zero",
          "[mixing][assistant]") {
  const assistant::MeanPowerSpectrum measured = hand_spectrum({1.0f, 1.0f, 1.0f});

  SECTION("no spectrum at all") {
    const assistant::MeanPowerSpectrum empty;
    CHECK(empty.energy_share_below(80.0f) == 0.0f);
  }

  SECTION("a spectrum carrying no energy") {
    CHECK(hand_spectrum({0.0f, 0.0f, 0.0f}).energy_share_below(80.0f) == 0.0f);
  }

  SECTION("a frequency that is not positive") {
    CHECK(measured.energy_share_below(0.0f) == 0.0f);
    CHECK(measured.energy_share_below(-80.0f) == 0.0f);
  }

  SECTION("a frequency that is not a real number") {
    CHECK(measured.energy_share_below(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(measured.energy_share_below(std::numeric_limits<float>::infinity()) == 0.0f);
    CHECK(measured.energy_share_below(-std::numeric_limits<float>::infinity()) == 0.0f);
  }

  SECTION("geometry that cannot map a bin to a frequency") {
    assistant::MeanPowerSpectrum broken = measured;
    broken.n_fft = 0;
    CHECK(broken.energy_share_below(80.0f) == 0.0f);
    broken = measured;
    broken.sample_rate = 0;
    CHECK(broken.energy_share_below(80.0f) == 0.0f);
  }

  SECTION("a declared bin count larger than the spectrum holds") {
    assistant::MeanPowerSpectrum overclaimed = measured;
    overclaimed.n_bins = 4096;
    float share = 0.0f;
    REQUIRE_NOTHROW(share = overclaimed.energy_share_below(3.0f * kBinHz));
    CHECK(share == 1.0f);
  }
}

TEST_CASE("Track profile leaves no spectrum on a track it could not measure",
          "[mixing][assistant]") {
  assistant::TrackInput track;
  track.id = "null";
  track.sample_rate = kSampleRate;

  const assistant::TrackProfile profile = assistant::analyze_track_profile(track);
  CHECK_FALSE(profile.usable);
  CHECK(profile.spectrum.n_bins == 0);
  CHECK(profile.spectrum.power.empty());
  CHECK(profile.spectrum.energy_share_below(80.0f) == 0.0f);
}

TEST_CASE("Track profile measures stereo loudness with channel summing", "[mixing][assistant]") {
  // Two unrelated tones: a mono downmix halves each one's amplitude, so a
  // downmix-based measurement reads the pair roughly 6 dB low.
  const std::vector<float> left = tone(0.5f, 200.0f);
  const std::vector<float> right = tone(0.5f, 1103.0f);
  std::vector<float> downmix(left.size(), 0.0f);
  for (std::size_t i = 0; i < downmix.size(); ++i) downmix[i] = 0.5f * (left[i] + right[i]);

  const assistant::TrackProfile stereo =
      assistant::analyze_track_profile(stereo_track("stereo", left, right));
  const assistant::TrackProfile mono =
      assistant::analyze_track_profile(mono_track("mono", downmix));

  REQUIRE(stereo.usable);
  REQUIRE(mono.usable);
  REQUIRE(stereo.channel_count == 2);
  REQUIRE(mono.channel_count == 1);
  REQUIRE(std::isfinite(stereo.base.loudness.integrated_lufs));
  REQUIRE(std::isfinite(mono.base.loudness.integrated_lufs));
  CHECK(stereo.base.loudness.integrated_lufs > mono.base.loudness.integrated_lufs + 3.0f);
}
