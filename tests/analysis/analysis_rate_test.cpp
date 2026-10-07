/// @file analysis_rate_test.cpp
/// @brief Tests for the analysis-rate window helper and the rate-invariance test material.

#include "analysis/analysis_rate.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "support/rate_material.h"
#include "util/exception.h"

using namespace sonare;
using sonare::test::make_rate_material;
using sonare::test::RateMaterial;

namespace {

bool only_235(int m) {
  for (int f : {2, 3, 5}) {
    while (m % f == 0) m /= f;
  }
  return m == 1;
}

}  // namespace

TEST_CASE("window_at_rate converts the window to the input rate", "[analysis_rate][helper]") {
  auto w = window_at_rate(2048, 22050);
  CHECK(w.win_length == 0);
  CHECK(w.n_fft == 2048);

  w = window_at_rate(2048, 48000);
  CHECK(w.win_length == 4458);
  CHECK(w.n_fft == 4608);

  w = window_at_rate(2048, 44100);
  CHECK(w.win_length == 4096);
  CHECK(w.n_fft == 4096);

  w = window_at_rate(2048, 32000);
  CHECK(w.win_length == 2972);
  CHECK(w.n_fft == fast_fft_length(2972));
  CHECK(w.n_fft >= w.win_length);
}

TEST_CASE("fast_fft_length returns the smallest fast length", "[analysis_rate][helper]") {
  for (int n = 1; n <= 6000; ++n) {
    const int m = fast_fft_length(n);
    REQUIRE(m >= n);
    REQUIRE(m % 32 == 0);
    REQUIRE(only_235(m));
    // Minimality by brute force.
    for (int k = ((n + 31) / 32) * 32; k < m; k += 32) {
      REQUIRE_FALSE(only_235(k));
    }
  }
}

TEST_CASE("window_at_rate rejects invalid and oversize requests", "[analysis_rate][helper]") {
  CHECK_THROWS_AS(window_at_rate(0, 48000), SonareException);
  CHECK_THROWS_AS(window_at_rate(-1, 48000), SonareException);
  CHECK_THROWS_AS(window_at_rate(2048, 0), SonareException);
  CHECK_THROWS_AS(window_at_rate(2048, -1), SonareException);
  CHECK_THROWS_AS(window_at_rate(kMaxStftNFft, 48000), SonareException);
  CHECK_THROWS_AS(fast_fft_length(0), SonareException);
}

TEST_CASE("stft_config_at_rate leaves the window unset at the analysis rate",
          "[analysis_rate][helper]") {
  const StftConfig base = stft_config_at_rate(2048, 512, 22050);
  CHECK(base.n_fft == 2048);
  CHECK(base.hop_length == 512);
  CHECK(base.win_length == 0);

  const StftConfig hi = stft_config_at_rate(2048, 512, 48000);
  CHECK(hi.n_fft == 4608);
  CHECK(hi.win_length == 4458);
  CHECK(hi.hop_length == 512);
}

TEST_CASE("window_at_rate converts from a caller-named reference rate", "[analysis_rate][helper]") {
  constexpr int kReference = 48000;
  const RateWindow same = window_at_rate(2048, kReference, kReference);
  CHECK(same.win_length == 0);
  CHECK(same.n_fft == 2048);

  const RateWindow low = window_at_rate(2048, 22050, kReference);
  CHECK(low.win_length == 941);
  CHECK(low.n_fft == 960);
  const RateWindow cd = window_at_rate(2048, 44100, kReference);
  CHECK(cd.win_length == 1882);
  CHECK(cd.n_fft == 1920);

  const StftConfig config = stft_config_at_rate(2048, 512, kReference, kReference);
  CHECK(config.n_fft == 2048);
  CHECK(config.win_length == 0);
  CHECK(config.hop_length == 512);
  CHECK_THROWS_AS(window_at_rate(2048, 48000, 0), SonareException);
}

TEST_CASE("rate material has the specified length at each rate", "[analysis_rate][helper]") {
  for (int sr : {22050, 48000}) {
    CAPTURE(sr);
    const double expect_t = 8.0 * sr;
    const double expect_k = 4.5 * sr;
    const double expect_c = 8.25 * sr;
    const double tol = 0.01 * sr;
    CHECK(
        std::abs(static_cast<double>(make_rate_material(RateMaterial::TriadTurnaround, sr).size()) -
                 expect_t) <= tol);
    CHECK(std::abs(static_cast<double>(make_rate_material(RateMaterial::Cadence, sr).size()) -
                   expect_k) <= tol);
    CHECK(std::abs(static_cast<double>(make_rate_material(RateMaterial::Clicks, sr).size()) -
                   expect_c) <= tol);
    CHECK(make_rate_material(RateMaterial::Clicks, sr).sample_rate() == sr);
  }
}

TEST_CASE("rate material reads as designed at 22050", "[analysis_rate][helper]") {
  SECTION("triad turnaround is C, Am, F, G") {
    ChordConfig config;
    config.use_triads_only = true;
    config.min_duration = 0.3f;
    config.use_beat_sync = false;
    const auto chords =
        detect_chords(make_rate_material(RateMaterial::TriadTurnaround, 22050), config);
    std::vector<std::string> names;
    for (const auto& c : chords) names.push_back(c.to_string());
    CAPTURE(names);
    CHECK(names == std::vector<std::string>{"C", "Am", "F", "G"});
  }
  SECTION("cadence is C major") {
    KeyAnalyzer key(make_rate_material(RateMaterial::Cadence, 22050));
    CAPTURE(static_cast<int>(key.root()), static_cast<int>(key.mode()));
    CHECK(key.root() == PitchClass::C);
    CHECK(key.mode() == Mode::Major);
  }
  SECTION("clicks give 16 onsets") {
    OnsetAnalyzer onsets(make_rate_material(RateMaterial::Clicks, 22050));
    CAPTURE(onsets.count());
    CHECK(onsets.count() == 16);
  }
}
