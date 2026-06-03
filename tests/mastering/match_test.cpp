#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/eq/equalizer.h"
#include "mastering/match/ab_switcher.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/match/tonal_balance.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::match;
using sonare::constants::kPi;

namespace {
using sonare::test::rms;

Audio sine_audio(float frequency_hz, float amplitude, int sample_rate = 48000,
                 float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz * i / sample_rate));
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

TEST_CASE("MatchEq live bands are placed at curve extrema and can configure EqualizerProcessor",
          "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f},
                           {-20.0f, -20.0f, -20.0f, -20.0f, -20.0f},
                           48000};
  ReferenceSpectrum reference{{100.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f},
                              {-20.0f, -10.0f, -20.0f, -30.0f, -20.0f},
                              48000};

  const auto bands = match_eq_bands(source, reference, {2, 12.0f, 100.0f, 12000.0f, 1.4f, 0});

  REQUIRE(bands.size() == 2);
  REQUIRE_THAT(bands[0].frequency_hz, WithinAbs(250.0f, 0.001f));
  REQUIRE(bands[0].gain_db > 9.0f);
  REQUIRE_THAT(bands[1].frequency_hz, WithinAbs(4000.0f, 0.001f));
  REQUIRE(bands[1].gain_db < -9.0f);

  sonare::mastering::eq::EqualizerProcessor eq({1});
  eq.prepare(48000.0, 512);
  configure_equalizer_from_match(eq, source, reference, {2, 12.0f, 100.0f, 12000.0f, 1.4f, 0});

  REQUIRE(eq.band(0).enabled);
  REQUIRE_THAT(eq.band(0).frequency_hz, WithinAbs(250.0f, 0.001f));
  REQUIRE(eq.band(1).enabled);
  REQUIRE_THAT(eq.band(1).frequency_hz, WithinAbs(4000.0f, 0.001f));
  REQUIRE_FALSE(eq.band(2).enabled);
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

TEST_CASE("MatchEq FIR kernel is linear phase and follows curve gain", "[mastering][match]") {
  MatchEqCurve curve{{100.0f, 1000.0f, 10000.0f}, {0.0f, 6.0f, 0.0f}};

  const auto kernel = match_eq_fir_kernel(curve, 48000, {1024, 257});

  REQUIRE(kernel.size() == 257);
  REQUIRE_THAT(kernel.front(), WithinAbs(kernel.back(), 0.0001f));
  REQUIRE_THAT(kernel[10], WithinAbs(kernel[kernel.size() - 11], 0.0001f));
}

TEST_CASE("MatchEq FIR kernel supports minimum-phase mode", "[mastering][match]") {
  MatchEqCurve curve{{100.0f, 1000.0f, 10000.0f}, {0.0f, 6.0f, 0.0f}};

  const auto linear = match_eq_fir_kernel(curve, 48000, {1024, 257});
  const auto minimum =
      match_eq_fir_kernel(curve, 48000, {1024, 257, MatchEqFirPhase::MinimumPhase});

  REQUIRE(minimum.size() == 257);
  REQUIRE(std::abs(minimum.front()) > std::abs(linear.front()) * 10.0f);
  REQUIRE(std::abs(minimum.front() - minimum.back()) > 0.0001f);
}

TEST_CASE("ApplyMatchEq boosts material toward the reference spectrum", "[mastering][match]") {
  const Audio input = sine_audio(1000.0f, 0.1f);
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -14.0f, -20.0f}, 48000};

  const auto output =
      apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0}, {1024, 257});

  REQUIRE(output.size() == input.size());
  REQUIRE(rms(output, 512) > rms(input, 512) * 1.5f);
}

TEST_CASE("ApplyMatchEq uses partitioned FIR and minimum-phase options", "[mastering][match]") {
  const Audio input = sine_audio(1000.0f, 0.1f);
  ReferenceSpectrum source{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -20.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 1000.0f, 10000.0f}, {-20.0f, -14.0f, -20.0f}, 48000};

  const auto linear = apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0},
                                     {1024, 257, MatchEqFirPhase::LinearPhase, 128});
  const auto minimum =
      apply_match_eq(input, source, reference, {8, 6.0f, 100.0f, 10000.0f, 1.0f, 0},
                     {1024, 257, MatchEqFirPhase::MinimumPhase, 128});

  REQUIRE(linear.size() == input.size());
  REQUIRE(minimum.size() == input.size());
  REQUIRE(rms(linear, 512) > rms(input, 512) * 1.5f);
  REQUIRE(rms(minimum, 512) > rms(input, 512) * 1.5f);
}

TEST_CASE("MatchEq can time-align reference audio by cross-correlation", "[mastering][match]") {
  std::vector<float> source_samples(512, 0.0f);
  for (size_t i = 0; i < source_samples.size(); ++i) {
    source_samples[i] = static_cast<float>(std::sin(0.13 * static_cast<double>(i)) +
                                           0.2 * std::sin(0.017 * static_cast<double>(i * i)));
  }
  std::vector<float> reference_samples(512, 0.0f);
  for (size_t i = 0; i + 5 < reference_samples.size(); ++i) {
    reference_samples[i + 5] = source_samples[i];
  }

  const auto source = Audio::from_vector(source_samples, 48000);
  const auto reference = Audio::from_vector(reference_samples, 48000);

  REQUIRE_THAT(estimate_reference_delay_samples(source, reference, 16), WithinAbs(5.0f, 0.01f));
  const auto aligned = align_reference_to_source(source, reference, 16);

  for (size_t i = 0; i + 5 < aligned.size(); ++i) {
    REQUIRE_THAT(aligned[i], WithinAbs(source[i], 0.00001f));
  }
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

TEST_CASE("TonalBalance can summarize log-frequency bands", "[mastering][match]") {
  ReferenceSpectrum source{{100.0f, 200.0f}, {-10.0f, -20.0f}, 48000};
  ReferenceSpectrum reference{{100.0f, 200.0f}, {-15.0f, -18.0f}, 48000};

  const auto balance = tonal_balance_log_bands(source, reference, 1, 100.0f, 400.0f);

  REQUIRE(balance.size() == 2);
  REQUIRE_THAT(balance[0].deviation_db, WithinAbs(5.0f, 0.001f));
  REQUIRE_THAT(balance[1].deviation_db, WithinAbs(-2.0f, 0.001f));
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
