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

namespace {

/// Output of @p eq over a fixed two-tone signal, after prepare at @p rate.
template <typename Eq>
std::vector<float> render_at(Eq& eq, int rate) {
  eq.prepare(rate, 512);
  auto audio = sine(3000.0f, rate, 4096);
  const auto high = sine(9000.0f, rate, 4096);
  for (size_t i = 0; i < audio.size(); ++i) audio[i] += high[i];
  process(eq, audio);
  return audio;
}

bool all_finite(const std::vector<float>& audio) {
  return std::all_of(audio.begin(), audio.end(), [](float x) { return std::isfinite(x); });
}

}  // namespace

TEST_CASE("an EQ re-prepared below a band's frequency designs it at the rate's ceiling",
          "[mastering][eq]") {
  constexpr int kLowRate = 22050;
  const float ceiling = sonare::rt::max_design_frequency_hz(kLowRate);
  const EqBand high{EqBandType::HighShelf, 12000.0f, 6.0f, 0.7f, true};
  const EqBand low{EqBandType::Peak, 2000.0f, -3.0f, 1.0f, true};

  ParametricEq moved;
  moved.prepare(48000.0, 512);
  moved.set_band(0, low);
  moved.set_band(1, high);
  std::vector<float> got;
  REQUIRE_NOTHROW(got = render_at(moved, kLowRate));
  // The band keeps the frequency it was given; only its design follows the rate.
  REQUIRE(moved.band(1).frequency_hz == 12000.0f);

  ParametricEq direct;
  direct.set_band(0, low);
  EqBand clamped = high;
  clamped.frequency_hz = ceiling;
  direct.set_band(1, clamped);
  const auto expected = render_at(direct, kLowRate);
  REQUIRE(all_finite(got));
  REQUIRE(max_abs_difference(got, expected) == 0.0f);

  // Gain automation on that band reaches the same design rather than throwing.
  REQUIRE(moved.set_parameter(4, 3.0f));
  clamped.gain_db = 3.0f;
  direct.set_band(1, clamped);
  auto after_moved = sine(5000.0f, kLowRate, 2048);
  auto after_direct = after_moved;
  moved.reset();
  direct.reset();
  process(moved, after_moved);
  process(direct, after_direct);
  REQUIRE(max_abs_difference(after_moved, after_direct) == 0.0f);

  DynamicEq dynamic;
  dynamic.prepare(48000.0, 512);
  DynamicEqBand dynamic_band;
  dynamic_band.type = EqBandType::HighShelf;
  dynamic_band.frequency_hz = 12000.0f;
  dynamic_band.static_gain_db = 6.0f;
  dynamic_band.enabled = true;
  dynamic.set_band(0, dynamic_band);
  std::vector<float> dynamic_out;
  REQUIRE_NOTHROW(dynamic_out = render_at(dynamic, kLowRate));
  REQUIRE(all_finite(dynamic_out));
}

TEST_CASE("a band set before any rate is known is refused by the first prepare it exceeds",
          "[mastering][eq]") {
  // The first prepare is where such a band meets its rate, so it is refused there
  // exactly as set_band refuses it at a known rate; only a band already accepted
  // at a higher rate is carried to a lower one at its ceiling.
  const EqBand high{EqBandType::HighShelf, 12000.0f, 6.0f, 0.7f, true};
  ParametricEq fresh;
  fresh.set_band(0, high);
  REQUIRE_THROWS_AS(fresh.prepare(22050.0, 512), sonare::SonareException);
  REQUIRE_NOTHROW(fresh.prepare(48000.0, 512));
  REQUIRE_NOTHROW(fresh.prepare(22050.0, 512));

  EqualizerProcessor equalizer;
  equalizer.set_band(0, high);
  REQUIRE_THROWS_AS(equalizer.prepare(22050.0, 512), sonare::SonareException);
  REQUIRE_NOTHROW(equalizer.prepare(48000.0, 512));
  REQUIRE_NOTHROW(equalizer.prepare(22050.0, 512));
}

TEST_CASE("frequency automation at its maximum never throws at high rates", "[mastering][eq]") {
  for (const int rate : {44100, 88200, 96000, 192000}) {
    INFO("rate " << rate);
    ParametricEq parametric;
    parametric.prepare(rate, 512);
    parametric.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
    REQUIRE(parametric.set_parameter(0, 1.0e9f));
    REQUIRE(parametric.band(0).frequency_hz < static_cast<float>(rate * 0.5));

    EqualizerProcessor equalizer;
    equalizer.prepare(rate, 512);
    equalizer.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
    REQUIRE(equalizer.set_parameter(0, 1.0e9f));

    TiltEq tilt;
    tilt.prepare(rate, 512);
    tilt.set_tilt_db(3.0f);
    REQUIRE(tilt.set_parameter(1, 1.0e9f));

    auto audio = sine(1000.0f, rate, 2048);
    process(parametric, audio);
    process(tilt, audio);
    REQUIRE(all_finite(audio));
  }
}

TEST_CASE("a refused band leaves an EQ's bands and design untouched", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);
  const EqBand kept{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  eq.set_band(0, kept);
  auto before = sine(1000.0f, 48000, 2048);
  auto after = before;
  process(eq, before);
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 30000.0f, 6.0f, 1.0f, true}));
  REQUIRE(eq.band(0).frequency_hz == kept.frequency_hz);
  eq.reset();
  process(eq, after);
  REQUIRE(max_abs_difference(before, after) == 0.0f);
}
