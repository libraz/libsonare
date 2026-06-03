/// @file eq_parametric_test.cpp
/// @brief Parametric EQ tests.

#include "eq_test_helpers.h"

TEST_CASE("ParametricEq with no enabled bands preserves audio", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  REQUIRE(audio.size() == original.size());
  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("ParametricEq peak band boosts its center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto off_center = sine(4000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float off_before = rms_tail(off_center, 4096);

  process(eq, center);
  eq.reset();
  process(eq, off_center);

  const float center_gain = rms_tail(center, 4096) / center_before;
  const float off_gain = rms_tail(off_center, 4096) / off_before;

  REQUIRE(center_gain > 1.85f);
  REQUIRE(off_gain < center_gain * 0.75f);
}

TEST_CASE("ParametricEq high-pass attenuates low frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, kButterworthQ, true});

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(2000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain < 0.08f);
  REQUIRE(high_gain > 0.9f);
}

TEST_CASE("ParametricEq Vicanek mode filters common band types", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  ParametricEq lowpass;
  lowpass.prepare(sample_rate, 1024);
  lowpass.set_band(
      0, {EqBandType::LowPass, 1000.0f, 0.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
  auto low = sine(200.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(lowpass, low);
  lowpass.reset();
  process(lowpass, high);
  REQUIRE(rms_tail(low, 4096) / low_before > 0.75f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.35f);

  ParametricEq highpass;
  highpass.prepare(sample_rate, 1024);
  highpass.set_band(
      0, {EqBandType::HighPass, 1000.0f, 0.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
  low = sine(200.0f, sample_rate, sample_rate);
  high = sine(8000.0f, sample_rate, sample_rate);
  process(highpass, low);
  highpass.reset();
  process(highpass, high);
  REQUIRE(rms_tail(low, 4096) / low_before < 0.35f);
  REQUIRE(rms_tail(high, 4096) / high_before > 0.75f);
}

TEST_CASE("ParametricEq Vicanek peak boosts its center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 6000.0f, 6.0f, 1.0f, true, BiquadCoeffMode::Vicanek});

  auto center = sine(6000.0f, sample_rate, sample_rate);
  auto off_center = sine(500.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float off_before = rms_tail(off_center, 4096);

  process(eq, center);
  eq.reset();
  process(eq, off_center);

  REQUIRE(rms_tail(center, 4096) / center_before > 1.5f);
  REQUIRE(rms_tail(off_center, 4096) / off_before < 1.2f);
}

TEST_CASE("ParametricEq Vicanek shelves match low and high shelf intent", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  ParametricEq low_shelf;
  low_shelf.prepare(sample_rate, 1024);
  low_shelf.set_band(
      0, {EqBandType::LowShelf, 1000.0f, 6.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(low_shelf, low);
  low_shelf.reset();
  process(low_shelf, high);
  REQUIRE(rms_tail(low, 4096) / low_before > 1.7f);
  REQUIRE(rms_tail(high, 4096) / high_before < 1.15f);

  ParametricEq high_shelf;
  high_shelf.prepare(sample_rate, 1024);
  high_shelf.set_band(
      0, {EqBandType::HighShelf, 6000.0f, 6.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
  low = sine(100.0f, sample_rate, sample_rate);
  high = sine(12000.0f, sample_rate, sample_rate);
  const float low2_before = rms_tail(low, 4096);
  const float high2_before = rms_tail(high, 4096);
  process(high_shelf, low);
  high_shelf.reset();
  process(high_shelf, high);
  REQUIRE(rms_tail(low, 4096) / low2_before < 1.15f);
  REQUIRE(rms_tail(high, 4096) / high2_before > 1.6f);
}

TEST_CASE("ParametricEq Vicanek high shelf falls back when endpoint error is excessive",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr float gain_db = 24.0f;
  const float w0 = static_cast<float>(2.0 * kPiD * 18000.0 / sample_rate);
  const auto coeffs = sonare::rt::vicanek_high_shelf(w0, gain_db);

  const float nyquist_mag =
      sonare::rt::biquad_magnitude(coeffs, static_cast<float>(sonare::constants::kPiD * 0.999));
  REQUIRE_THAT(20.0f * std::log10(nyquist_mag), WithinAbs(gain_db, 0.05f));

  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(
      0, {EqBandType::HighShelf, 18000.0f, gain_db, kButterworthQ, true, BiquadCoeffMode::Vicanek});

  auto high = sine(23000.0f, sample_rate, sample_rate);
  const float before = rms_tail(high, 4096);
  process(eq, high);
  REQUIRE(20.0f * std::log10(rms_tail(high, 4096) / before) > 22.0f);
}

TEST_CASE("ParametricEq disabled band is bypassed", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 12.0f, 1.0f, false});

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("ParametricEq validates band configuration", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(ParametricEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 24000.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}

TEST_CASE("ParametricEq supports 24 bands while preserving old aggregate initialization",
          "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);
  eq.set_band(23, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true});

  REQUIRE(eq.band(23).enabled);
  REQUIRE(eq.band(23).type == EqBandType::Peak);
  REQUIRE(eq.band(23).coeff_mode == BiquadCoeffMode::Rbj);
  REQUIRE(eq.band(23).phase == PhaseMode::Inherit);
}
