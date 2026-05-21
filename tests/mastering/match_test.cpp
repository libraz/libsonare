#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/match/ab_switcher.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/match/tonal_balance.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::match;

namespace {

constexpr double kPi = 3.14159265358979323846;

Audio sine_audio(float frequency_hz, float amplitude, int sample_rate = 48000,
                 float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency_hz * i / sample_rate));
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

}  // namespace

TEST_CASE("ReferenceSpectrum extracts smoothed bins", "[mastering][match]") {
  const auto spectrum = reference_spectrum(sine_audio(1000.0f, 0.5f), {1024, 256, true, 3});

  REQUIRE(spectrum.sample_rate == 48000);
  REQUIRE(spectrum.frequencies.size() == 513);
  REQUIRE(spectrum.db.size() == spectrum.frequencies.size());
}

TEST_CASE("ReferenceSpectrum averages across the full signal", "[mastering][match]") {
  std::vector<float> samples(48000, 0.0f);
  for (size_t i = 0; i < samples.size() / 2; ++i) {
    samples[i] =
        0.7f * static_cast<float>(std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / 48000.0));
  }
  for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
    samples[i] =
        0.7f * static_cast<float>(std::sin(2.0 * kPi * 4000.0 * static_cast<double>(i) / 48000.0));
  }

  const auto spectrum =
      reference_spectrum(Audio::from_vector(std::move(samples), 48000), {2048, 512, false, 3});

  const auto near_500 = static_cast<size_t>(std::round(500.0 / (48000.0 / 2048.0)));
  const auto near_4000 = static_cast<size_t>(std::round(4000.0 / (48000.0 / 2048.0)));
  REQUIRE(spectrum.db[near_500] > -20.0f);
  REQUIRE(spectrum.db[near_4000] > -20.0f);
}

TEST_CASE("MatchEq bands follow reference minus source difference", "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-14.0f, -8.0f, -26.0f}, 48000};

  const auto bands = match_eq_bands(source, reference, {3, 6.0f, 100.0f, 20000.0f, 1.2f, 0});

  REQUIRE(bands.size() == 3);
  REQUIRE(bands[0].enabled);
  REQUIRE(bands[0].gain_db > 0.0f);
  REQUIRE(bands[1].gain_db <= 6.0f);
  REQUIRE(bands[2].gain_db < 0.0f);
}

TEST_CASE("MatchEq curve keeps dense smoothed correction data", "[mastering][match]") {
  ReferenceSpectrum source{
      {100.0f, 200.0f, 400.0f, 800.0f}, {-20.0f, -20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{
      {100.0f, 200.0f, 400.0f, 800.0f}, {-14.0f, -14.0f, -26.0f, -26.0f}, 48000};

  const auto curve = match_eq_curve(source, reference, {4, 12.0f, 100.0f, 800.0f, 1.0f, 1});

  REQUIRE(curve.frequencies.size() == 4);
  REQUIRE(curve.gain_db.size() == curve.frequencies.size());
  REQUIRE(curve.gain_db.front() > 0.0f);
  REQUIRE(curve.gain_db.back() < 0.0f);
}

TEST_CASE("TonalBalance summarizes broad band deviations", "[mastering][match]") {
  ReferenceSpectrum source{
      {100.0f, 500.0f, 3000.0f, 12000.0f}, {-10.0f, -20.0f, -30.0f, -40.0f}, 48000};
  ReferenceSpectrum reference{
      {100.0f, 500.0f, 3000.0f, 12000.0f}, {-12.0f, -18.0f, -35.0f, -35.0f}, 48000};

  const auto balance = tonal_balance(source, reference);

  REQUIRE(balance.size() == 4);
  REQUIRE_THAT(balance[0].deviation_db, WithinAbs(2.0f, 0.001f));
  REQUIRE_THAT(balance[1].deviation_db, WithinAbs(-2.0f, 0.001f));
  REQUIRE_THAT(balance[2].deviation_db, WithinAbs(5.0f, 0.001f));
  REQUIRE_THAT(balance[3].deviation_db, WithinAbs(-5.0f, 0.001f));
}

TEST_CASE("ReferenceLoudness reports gain required to match reference", "[mastering][match]") {
  const auto quiet = sine_audio(1000.0f, 0.05f);
  const auto loud = sine_audio(1000.0f, 0.2f);

  const auto result = reference_loudness(quiet, loud);

  REQUIRE(result.gain_to_match_db > 10.0f);
  REQUIRE(result.reference_lufs > result.source_lufs);
}

TEST_CASE("ABSwitcher selects and crossfades audio", "[mastering][match]") {
  const std::vector<float> a = {0.0f, 0.25f, 0.5f};
  const std::vector<float> b = {1.0f, 0.75f, 0.5f};
  const Audio aa = Audio::from_buffer(a.data(), a.size(), 48000);
  const Audio bb = Audio::from_buffer(b.data(), b.size(), 48000);

  const auto selected = ab_switch(aa, bb, ABSelection::B);
  const auto mixed = ab_crossfade(aa, bb, 0.25f);

  REQUIRE_THAT(selected[0], WithinAbs(1.0f, 0.001f));
  REQUIRE(mixed.size() == 3);
  REQUIRE_THAT(mixed[0], WithinAbs(0.25f, 0.001f));
  REQUIRE_THAT(mixed[1], WithinAbs(0.375f, 0.001f));
}

TEST_CASE("Match helpers validate inputs", "[mastering][match]") {
  const Audio empty;
  REQUIRE_THROWS(reference_spectrum(empty));

  ReferenceSpectrum source{{100.0f}, {-10.0f}, 48000};
  ReferenceSpectrum reference{{100.0f}, {-10.0f}, 44100};
  REQUIRE_THROWS(match_eq_bands(source, reference));
  REQUIRE_THROWS(tonal_balance(source, reference));

  const auto audio = sine_audio(1000.0f, 0.1f);
  REQUIRE_THROWS(ab_crossfade(audio, audio, 1.5f));
}
