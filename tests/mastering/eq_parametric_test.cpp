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
    const float highest = sonare::rt::max_design_frequency_hz(rate);
    ParametricEq parametric;
    parametric.prepare(rate, 512);
    parametric.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
    REQUIRE(parametric.set_parameter(0, highest));
    REQUIRE(parametric.band(0).frequency_hz == highest);
    REQUIRE_FALSE(parametric.set_parameter(0, 1.0e9f));

    EqualizerProcessor equalizer;
    equalizer.prepare(rate, 512);
    equalizer.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
    REQUIRE(equalizer.set_parameter(0, highest));
    REQUIRE_FALSE(equalizer.set_parameter(0, 1.0e9f));

    // The tilt pivot's design keeps any positive corner below Nyquist.
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

namespace {

// A refused live value leaves the band and the rendered audio exactly as they were; the value
// just inside the bound is stored unclamped.
template <typename Eq>
void check_band_live_domain(const char* name, double sample_rate, unsigned int id, float refused,
                            float accepted, float EqBand::*field) {
  INFO(name << " id " << id << " refused " << refused);
  const EqBand kept{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  Eq eq;
  Eq pristine;
  eq.set_band(0, kept);
  pristine.set_band(0, kept);
  eq.prepare(sample_rate, 4096);
  pristine.prepare(sample_rate, 4096);

  REQUIRE_FALSE(eq.set_parameter(id, refused));
  REQUIRE(eq.band(0) == kept);
  auto left = sine(1000.0f, static_cast<int>(sample_rate), 4096);
  auto right = left;
  process(eq, left);
  process(pristine, right);
  REQUIRE(max_abs_difference(left, right) == 0.0f);

  REQUIRE(eq.set_parameter(id, accepted));
  REQUIRE(eq.band(0).*field == accepted);
}

template <typename Eq>
void check_band_domain_table(const char* name) {
  constexpr double kRate = 48000.0;
  const float just_below_nyquist = std::nextafter(static_cast<float>(kRate * 0.5), 0.0f);
  for (const float refused : {0.0f, -10.0f, static_cast<float>(kRate * 0.5), 30000.0f}) {
    check_band_live_domain<Eq>(name, kRate, 0u, refused, just_below_nyquist, &EqBand::frequency_hz);
  }
  for (const float refused : {0.0f, -1.0f}) {
    check_band_live_domain<Eq>(name, kRate, 2u, refused, 1.0e-6f, &EqBand::q);
  }
}

}  // namespace

TEST_CASE("EQ live frequency and Q refuse the values their band setter refuses",
          "[mastering][eq][live_domain]") {
  check_band_domain_table<ParametricEq>("ParametricEq");
  check_band_domain_table<EqualizerProcessor>("EqualizerProcessor");
  check_band_domain_table<LinearPhaseEq>("LinearPhaseEq");
}

TEST_CASE("EqualizerProcessor refuses a non-positive live frequency before prepare",
          "[mastering][eq][live_domain]") {
  EqualizerProcessor eq;
  const EqBand kept{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  eq.set_band(0, kept);
  REQUIRE_FALSE(eq.set_parameter(0, 0.0f));
  REQUIRE_FALSE(eq.set_parameter(0, -5.0f));
  REQUIRE(eq.band(0) == kept);
  // The rate is not known yet, so a high corner is judged at prepare().
  REQUIRE(eq.set_parameter(0, 30000.0f));
  REQUIRE(eq.band(0).frequency_hz == 30000.0f);
}

TEST_CASE("DynamicEq live parameters refuse the values its band validator refuses",
          "[mastering][eq][live_domain]") {
  struct Row {
    const char* name;
    unsigned int field;
    float refused;
    float accepted;
    float DynamicEqBand::*member;
  };
  const Row rows[] = {
      {"frequency zero", 0, 0.0f, 1.0e-3f, &DynamicEqBand::frequency_hz},
      {"frequency negative", 0, -10.0f, 1.0e-3f, &DynamicEqBand::frequency_hz},
      {"q zero", 2, 0.0f, 1.0e-3f, &DynamicEqBand::q},
      {"ratio below one", 4, 0.5f, 1.0f, &DynamicEqBand::ratio},
      {"sidechain q zero", 6, 0.0f, 1.0e-3f, &DynamicEqBand::sidechain_q},
      {"sidechain frequency zero", 7, 0.0f, 1.0e-3f, &DynamicEqBand::sidechain_freq_hz},
      {"sidechain frequency below the sentinel", 7, -2.0f, -1.0f,
       &DynamicEqBand::sidechain_freq_hz},
      {"sidechain frequency between sentinel and zero", 7, -0.5f, 20.0f,
       &DynamicEqBand::sidechain_freq_hz},
      {"attack negative", 8, -0.5f, 0.0f, &DynamicEqBand::attack_ms},
      {"release negative", 9, -0.5f, 0.0f, &DynamicEqBand::release_ms},
      {"detector delay negative", 10, -0.5f, 0.0f, &DynamicEqBand::detector_delay_ms},
  };
  const DynamicEqBand kept{EqBandType::Peak, 1000.0f, 3.0f, 1.0f, -24.0f, 2.0f, -6.0f, true};
  for (const bool prepared : {false, true}) {
    for (const Row& row : rows) {
      INFO(row.name << (prepared ? " (prepared)" : " (unprepared)"));
      DynamicEq eq;
      DynamicEq pristine;
      eq.set_band(0, kept);
      pristine.set_band(0, kept);
      if (prepared) {
        eq.prepare(48000.0, 1024);
        pristine.prepare(48000.0, 1024);
      }
      const unsigned int id = row.field;
      REQUIRE_FALSE(eq.set_parameter(id, row.refused));
      REQUIRE(eq.band(0) == kept);
      if (prepared) {
        auto left = sine(1000.0f, 48000, 4096);
        auto right = left;
        process(eq, left);
        process(pristine, right);
        REQUIRE(max_abs_difference(left, right) == 0.0f);
      }
      REQUIRE(eq.set_parameter(id, row.accepted));
      REQUIRE(eq.band(0).*row.member == row.accepted);
    }
  }
}

TEST_CASE("TiltEq live pivot refuses a non-positive frequency", "[mastering][eq][live_domain]") {
  TiltEq tilt;
  tilt.prepare(48000.0, 512);
  const float kept = tilt.pivot_hz();
  REQUIRE_FALSE(tilt.set_parameter(1, 0.0f));
  REQUIRE_FALSE(tilt.set_parameter(1, -1.0f));
  REQUIRE(tilt.pivot_hz() == kept);
  REQUIRE(tilt.set_parameter(1, 1.0e-3f));
  REQUIRE(tilt.pivot_hz() == 1.0e-3f);
}

TEST_CASE("PassiveTubeEq live frequencies and drive refuse what the setters refuse",
          "[mastering][eq][live_domain]") {
  PassiveTubeEq eq;
  eq.prepare(48000.0, 512);
  const float low = eq.low_frequency();
  const float high_boost = eq.high_boost_frequency();
  const float high_atten = eq.high_attenuation_frequency();
  for (const unsigned int id : {0u, 3u, 6u}) {
    INFO("id " << id);
    REQUIRE_FALSE(eq.set_parameter(id, 0.0f));
    REQUIRE_FALSE(eq.set_parameter(id, -100.0f));
  }
  REQUIRE(eq.low_frequency() == low);
  REQUIRE(eq.high_boost_frequency() == high_boost);
  REQUIRE(eq.high_attenuation_frequency() == high_atten);
  REQUIRE(eq.set_parameter(0, 1.0e-3f));
  REQUIRE(eq.low_frequency() == 1.0e-3f);
  REQUIRE(eq.set_parameter(3, 1.0e-3f));
  REQUIRE(eq.high_boost_frequency() == 1.0e-3f);
  REQUIRE(eq.set_parameter(6, 1.0e-3f));
  REQUIRE(eq.high_attenuation_frequency() == 1.0e-3f);
  // A corner above the design ceiling is kept inside it, as before.
  REQUIRE(eq.set_parameter(3, 1.0e9f));
  REQUIRE(eq.high_boost_frequency() < 24000.0f);

  eq.set_output_drive(2.0f);
  REQUIRE_FALSE(eq.set_parameter(8, -0.5f));
  REQUIRE(eq.output_drive() == 2.0f);
  REQUIRE(eq.set_parameter(8, 0.0f));
  REQUIRE(eq.output_drive() == 0.0f);
  REQUIRE(eq.set_parameter(8, 50.0f));
  REQUIRE(eq.output_drive() == 10.0f);
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

namespace {

/// @brief Drives @p processor with a loud 60 Hz tone, applies @p take_out, feeds silence,
///        applies @p put_back, and returns the peak of the next 256 silent samples.
template <typename Processor, typename TakeOut, typename PutBack>
float peak_after_reengage(Processor& processor, TakeOut take_out, PutBack put_back) {
  std::vector<float> left(2048);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] =
        0.8f * static_cast<float>(std::sin(2.0 * kPiD * 60.0 * static_cast<double>(i) / 48000.0));
  }
  std::vector<float> right = left;
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, 2048);
  take_out();
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  processor.process(channels, 2, 2048);
  put_back();
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  processor.process(channels, 2, 256);
  float peak = 0.0f;
  for (size_t i = 0; i < 256; ++i) peak = std::max({peak, std::fabs(left[i]), std::fabs(right[i])});
  return peak;
}

}  // namespace

TEST_CASE("EQ stages taken out of the path re-enter from rest", "[mastering][eq]") {
  SECTION("parametric band disabled and re-enabled") {
    ParametricEq eq;
    const EqBand band{EqBandType::Peak, 60.0f, 12.0f, 1.0f, true};
    eq.set_band(0, band);
    eq.prepare(48000.0, 2048);
    EqBand off = band;
    off.enabled = false;
    REQUIRE(peak_after_reengage(
                eq, [&] { eq.set_band(0, off); }, [&] { eq.set_band(0, band); }) == 0.0f);
  }
  SECTION("graphic band brought to zero gain and back") {
    GraphicEq eq;
    eq.prepare(48000.0, 2048);
    eq.set_gain_for_frequency(60.0f, 12.0f);
    REQUIRE(peak_after_reengage(
                eq, [&] { eq.set_gain_for_frequency(60.0f, 0.0f); },
                [&] { eq.set_gain_for_frequency(60.0f, 12.0f); }) == 0.0f);
  }
  SECTION("cut filter stages cleared and set again") {
    CutFilter cut;
    cut.prepare(48000.0, 2048);
    cut.set_high_pass(80.0f);
    REQUIRE(peak_after_reengage(
                cut, [&] { cut.clear_high_pass(); }, [&] { cut.set_high_pass(80.0f); }) == 0.0f);
    cut.clear_high_pass();
    cut.set_low_pass(200.0f);
    REQUIRE(peak_after_reengage(
                cut, [&] { cut.clear_low_pass(); }, [&] { cut.set_low_pass(200.0f); }) == 0.0f);
  }
  SECTION("cut filter slope change rebuilds its cascade from rest") {
    CutFilter cut;
    cut.prepare(48000.0, 2048);
    cut.set_high_pass(80.0f, kButterworthQ, CutFilterSlope::Db12PerOct);
    REQUIRE(peak_after_reengage(
                cut, [&] { cut.set_high_pass(80.0f, kButterworthQ, CutFilterSlope::Db24PerOct); },
                [] {}) == 0.0f);
  }
}
