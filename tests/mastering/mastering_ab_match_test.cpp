#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/common/loudness_measure.h"
#include "mastering/match/ab_switcher.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::match;
using sonare::test::generate_sine_audio;

namespace {

// Full-scale-ish square wave: much higher BS.1770 loudness than a sine at the
// same peak, so it stands in for a heavily loudness-maximized reference.
Audio square_audio(float frequency_hz, float amplitude, int sample_rate = 48000,
                   float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> out(static_cast<std::size_t>(samples));
  const double period = static_cast<double>(sample_rate) / frequency_hz;
  for (int i = 0; i < samples; ++i) {
    const double phase = std::fmod(static_cast<double>(i), period) / period;
    out[static_cast<std::size_t>(i)] = phase < 0.5 ? amplitude : -amplitude;
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

}  // namespace

TEST_CASE("ABMatchLoudness matches a quiet b to a loud a", "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.5f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.05f);

  const auto matched = ab_match_loudness(a, b);

  // The output's measured loudness is what a stub returning gain 1 (unity)
  // cannot produce: it would leave b's LUFS exactly where it started, clearly
  // short of a's.
  const float a_lufs = mastering::common::measure_lufs(a);
  const float matched_b_lufs = mastering::common::measure_lufs(matched.b);
  REQUIRE_THAT(matched_b_lufs, WithinAbs(a_lufs, 0.2f));
  REQUIRE(matched.applied_gain_db > 5.0f);
}

TEST_CASE("ABMatchLoudness applies ~0 dB when a and b already match",
         "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);

  const auto matched = ab_match_loudness(a, b);

  REQUIRE_THAT(matched.applied_gain_db, WithinAbs(0.0f, 0.05f));
  REQUIRE_THAT(matched.b[0], WithinAbs(b[0], 1e-4f));
}

TEST_CASE("ABMatchLoudness leaves silent b unchanged instead of diverging",
         "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.5f);
  const std::vector<float> zeros(48000, 0.0f);
  const Audio silent_b = Audio::from_vector(zeros, 48000);

  const auto matched = ab_match_loudness(a, silent_b);

  REQUIRE(matched.applied_gain_db == 0.0f);
  REQUIRE(matched.b.size() == silent_b.size());
  for (std::size_t i = 0; i < matched.b.size(); ++i) {
    REQUIRE(matched.b[i] == 0.0f);
  }
  // Pins the finite silence sentinel true_peak_db() reports here instead of
  // -inf or NaN; no other case in this file is silent, so nothing else checks it.
  REQUIRE(matched.matched_true_peak_dbtp == sonare::constants::kFloorDb);
}

TEST_CASE("ABMatchLoudness leaves b unchanged when a is silent",
         "[mastering][match][ab-match]") {
  const std::vector<float> zeros(48000, 0.0f);
  const Audio silent_a = Audio::from_vector(zeros, 48000);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);

  const auto matched = ab_match_loudness(silent_a, b);

  REQUIRE(matched.applied_gain_db == 0.0f);
  for (std::size_t i = 0; i < matched.b.size(); ++i) {
    REQUIRE_THAT(matched.b[i], WithinAbs(b[i], 1e-6f));
  }
  REQUIRE(matched.matched_true_peak_dbtp > sonare::constants::kFloorDb);
}

TEST_CASE("ABMatchLoudness matches faithfully even when b is near full scale",
         "[mastering][match][ab-match]") {
  // b sits close to full scale already. A clamp keyed on b's own headroom
  // would leave b at its own loudness here; the match must still be applied
  // in full, with the resulting overshoot only reported, not prevented.
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.98f);
  const auto a = square_audio(1000.0f, 0.98f);

  const float a_lufs = mastering::common::measure_lufs(a);
  const float b_lufs = mastering::common::measure_lufs(b);
  REQUIRE(a_lufs > b_lufs + 1.0f);  // sanity: the square really is louder in LUFS

  const auto matched = ab_match_loudness(a, b);

  const float matched_b_lufs = mastering::common::measure_lufs(matched.b);
  REQUIRE_THAT(matched_b_lufs, WithinAbs(a_lufs, 0.2f));
  REQUIRE(matched.matched_true_peak_dbtp > 0.0f);
}

TEST_CASE("ABMatchLoudness validates like the existing A/B helpers",
         "[mastering][match][ab-match]") {
  const Audio empty;
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  REQUIRE_THROWS(ab_match_loudness(empty, b));
  REQUIRE_THROWS(ab_match_loudness(b, empty));

  const auto a_44k = generate_sine_audio(1000.0f, 44100, 1.0f, 0.2f);
  const auto b_48k = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  REQUIRE_THROWS(ab_match_loudness(a_44k, b_48k));
}

TEST_CASE("ABMatchLoudness returns a unchanged", "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.3f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.1f);

  const auto matched = ab_match_loudness(a, b);

  REQUIRE(matched.a.data() == a.data());
  REQUIRE(matched.a.size() == a.size());
}

TEST_CASE("ABMatchLoudness composes with the existing switch and crossfade",
         "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.3f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.05f);

  const auto matched = ab_match_loudness(a, b);
  const auto switched = ab_switch(matched.a, matched.b, ABSelection::B);
  const auto crossfaded = ab_crossfade(matched.a, matched.b, 0.5f);

  const float a_lufs = mastering::common::measure_lufs(a);
  REQUIRE_THAT(mastering::common::measure_lufs(switched), WithinAbs(a_lufs, 0.2f));
  REQUIRE(crossfaded.size() == std::min(matched.a.size(), matched.b.size()));
}
