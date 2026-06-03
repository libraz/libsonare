/// @file eq_specialized_test.cpp
/// @brief Specialized EQ processor tests.

#include "eq_test_helpers.h"

TEST_CASE("ShelvingEq boosts low and high shelves independently", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ShelvingEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_shelf(250.0f, 6.0f);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain > 1.7f);
  REQUIRE(high_gain < 1.1f);

  eq.clear();
  eq.set_high_shelf(4000.0f, 6.0f);
  low = sine(100.0f, sample_rate, sample_rate);
  high = sine(8000.0f, sample_rate, sample_rate);

  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 4096) / low_before < 1.1f);
  REQUIRE(rms_tail(high, 4096) / high_before > 1.7f);
}

TEST_CASE("CutFilter high-pass slope controls attenuation depth", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(4000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  CutFilter gentle;
  gentle.prepare(sample_rate, 1024);
  gentle.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db12PerOct);
  process(gentle, low);

  CutFilter steep;
  steep.prepare(sample_rate, 1024);
  steep.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db24PerOct);
  auto steep_low = sine(100.0f, sample_rate, sample_rate);
  process(steep, steep_low);

  CutFilter high_pass;
  high_pass.prepare(sample_rate, 1024);
  high_pass.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db24PerOct);
  process(high_pass, high);

  const float gentle_low_gain = rms_tail(low, 4096) / low_before;
  const float steep_low_gain = rms_tail(steep_low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(steep_low_gain < gentle_low_gain * 0.25f);
  REQUIRE(steep_low_gain < 0.01f);
  REQUIRE(high_gain > 0.9f);
}

TEST_CASE("CutFilter low-pass attenuates high frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_pass(1000.0f, kButterworthQ, CutFilterSlope::Db24PerOct);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 4096) / low_before > 0.95f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.02f);
}

TEST_CASE("CutFilter supports 6 through 96 dB/oct high-pass slopes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{
      CutFilterSlope::Db6PerOct,  CutFilterSlope::Db12PerOct, CutFilterSlope::Db24PerOct,
      CutFilterSlope::Db48PerOct, CutFilterSlope::Db72PerOct, CutFilterSlope::Db96PerOct,
  };

  float previous_stop_gain = 1.0f;
  for (CutFilterSlope slope : slopes) {
    CutFilter eq;
    eq.prepare(sample_rate, 1024);
    eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ, slope);

    auto stop = sine(500.0f, sample_rate, samples);
    auto pass = sine(8000.0f, sample_rate, samples);
    const float stop_before = rms_tail(stop, 4096);
    const float pass_before = rms_tail(pass, 4096);
    process(eq, stop);
    eq.reset();
    process(eq, pass);

    const float stop_gain = rms_tail(stop, 4096) / stop_before;
    const float pass_gain = rms_tail(pass, 4096) / pass_before;
    REQUIRE(stop_gain < previous_stop_gain);
    REQUIRE(pass_gain > 0.85f);
    previous_stop_gain = stop_gain;
  }
}

TEST_CASE("CutFilter supports 6 through 96 dB/oct low-pass slopes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{
      CutFilterSlope::Db6PerOct,  CutFilterSlope::Db12PerOct, CutFilterSlope::Db24PerOct,
      CutFilterSlope::Db48PerOct, CutFilterSlope::Db72PerOct, CutFilterSlope::Db96PerOct,
  };

  float previous_stop_gain = 1.0f;
  for (CutFilterSlope slope : slopes) {
    CutFilter eq;
    eq.prepare(sample_rate, 1024);
    eq.set_low_pass(1000.0f, sonare::constants::kButterworthQ, slope);

    auto pass = sine(100.0f, sample_rate, samples);
    auto stop = sine(2000.0f, sample_rate, samples);
    const float pass_before = rms_tail(pass, 4096);
    const float stop_before = rms_tail(stop, 4096);
    process(eq, pass);
    eq.reset();
    process(eq, stop);

    const float pass_gain = rms_tail(pass, 4096) / pass_before;
    const float stop_gain = rms_tail(stop, 4096) / stop_before;
    REQUIRE(stop_gain < previous_stop_gain);
    REQUIRE(pass_gain > 0.85f);
    previous_stop_gain = stop_gain;
  }
}

TEST_CASE("CutFilter applies resonance only to the final cut stage", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  CutFilter flat;
  flat.prepare(sample_rate, 1024);
  flat.set_high_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Db48PerOct);

  CutFilter resonant;
  resonant.prepare(sample_rate, 1024);
  resonant.set_high_pass(1000.0f, 2.0f, CutFilterSlope::Db48PerOct);

  auto flat_cutoff = sine(1000.0f, sample_rate, sample_rate);
  auto resonant_cutoff = flat_cutoff;
  const float before = rms_tail(flat_cutoff, 4096);
  process(flat, flat_cutoff);
  process(resonant, resonant_cutoff);

  REQUIRE(rms_tail(resonant_cutoff, 4096) / before > rms_tail(flat_cutoff, 4096) / before * 1.2f);
}

TEST_CASE("CutFilter brickwall high-pass uses linear-phase FIR latency and steep rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Brickwall);

  REQUIRE(eq.latency_samples() > 0);

  auto stop = sine(250.0f, sample_rate, sample_rate);
  auto pass = sine(8000.0f, sample_rate, sample_rate);
  const float stop_before = rms_tail(stop, 8192);
  const float pass_before = rms_tail(pass, 8192);

  process(eq, stop);
  eq.reset();
  process(eq, pass);

  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);

  eq.clear_high_pass();
  REQUIRE(eq.latency_samples() == 0);
}

TEST_CASE("CutFilter brickwall low-pass uses linear-phase FIR latency and steep rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Brickwall);

  REQUIRE(eq.latency_samples() > 0);

  auto pass = sine(100.0f, sample_rate, sample_rate);
  auto stop = sine(8000.0f, sample_rate, sample_rate);
  const float pass_before = rms_tail(pass, 8192);
  const float stop_before = rms_tail(stop, 8192);

  process(eq, pass);
  eq.reset();
  process(eq, stop);

  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);
  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
}

TEST_CASE("BandPassEq passes center frequency and rejects off-band tones", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  BandPassEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band_pass(1000.0f, 4.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, center);
  eq.reset();
  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(center, 4096) / center_before > 0.85f);
  REQUIRE(rms_tail(low, 4096) / low_before < 0.2f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.2f);
}

TEST_CASE("BandPassEq notch rejects center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  BandPassEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_notch(1000.0f, 8.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, center);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(center, 4096) / center_before < 0.1f);
  REQUIRE(rms_tail(high, 4096) / high_before > 0.9f);
}

TEST_CASE("MidSideEq mid band affects mono-compatible content", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MidSideEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_mid_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto left = sine(1000.0f, sample_rate, sample_rate);
  auto right = left;
  const float before = rms_tail(left, 4096);

  process_stereo(eq, left, right);

  const float left_gain = rms_tail(left, 4096) / before;
  const float right_gain = rms_tail(right, 4096) / before;
  REQUIRE(left_gain > 1.8f);
  REQUIRE_THAT(left_gain, WithinAbs(right_gain, 0.0001f));
}

TEST_CASE("MidSideEq side band affects side-only content", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MidSideEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_side_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto left = sine(1000.0f, sample_rate, sample_rate);
  auto right = left;
  for (auto& sample : right) {
    sample = -sample;
  }
  const float before = rms_tail(left, 4096);

  process_stereo(eq, left, right);

  const float left_gain = rms_tail(left, 4096) / before;
  const float right_gain = rms_tail(right, 4096) / before;
  REQUIRE(left_gain > 1.8f);
  REQUIRE_THAT(left_gain, WithinAbs(right_gain, 0.0001f));
  REQUIRE_THAT(left[5000], WithinAbs(-right[5000], 0.0001f));
}

TEST_CASE("MidSideEq requires stereo input", "[mastering][eq]") {
  MidSideEq eq;
  eq.prepare(48000.0, 512);

  auto mono = sine(1000.0f, 48000, 1024);
  float* channels[] = {mono.data()};
  REQUIRE_THROWS(eq.process(channels, 1, static_cast<int>(mono.size())));
}

TEST_CASE("MidSideEq processes blocks wider than prepared without reallocating",
          "[mastering][eq]") {
  // Prepared with a positive max block, a wider one-shot block is chunked
  // internally (no audio-thread reallocation) and yields the same result as
  // processing it in prepared-size pieces.
  auto make = []() {
    auto l = sine(1000.0f, 48000, 1024, 0.3f);
    auto r = sine(1500.0f, 48000, 1024, 0.2f);
    return std::pair{l, r};
  };

  MidSideEq whole;
  whole.prepare(48000.0, 256);
  whole.set_mid_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
  auto [wl, wr] = make();
  float* wc[] = {wl.data(), wr.data()};
  REQUIRE_NOTHROW(whole.process(wc, 2, 1024));  // 1024 > prepared 256 -> chunked

  MidSideEq piecewise;
  piecewise.prepare(48000.0, 256);
  piecewise.set_mid_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});
  auto [pl, pr] = make();
  for (int start = 0; start < 1024; start += 256) {
    float* pc[] = {pl.data() + start, pr.data() + start};
    piecewise.process(pc, 2, 256);
  }
  for (int i = 0; i < 1024; ++i) {
    REQUIRE_THAT(wl[static_cast<size_t>(i)], WithinAbs(pl[static_cast<size_t>(i)], 1e-5f));
    REQUIRE_THAT(wr[static_cast<size_t>(i)], WithinAbs(pr[static_cast<size_t>(i)], 1e-5f));
  }
}

TEST_CASE("GraphicEq exposes 31 bands and nearest band lookup", "[mastering][eq]") {
  GraphicEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE(GraphicEq::kNumBands == 31);
  REQUIRE_THAT(eq.center_frequency(0), WithinAbs(20.0f, 0.0001f));
  REQUIRE_THAT(eq.center_frequency(17), WithinAbs(1000.0f, 0.0001f));
  REQUIRE_THAT(eq.center_frequency(30), WithinAbs(20000.0f, 0.0001f));
  REQUIRE(eq.nearest_band(990.0f) == 17);
  REQUIRE(GraphicEq::band_q_for_gain_db(12.0f) > GraphicEq::band_q_for_gain_db(3.0f));
  REQUIRE_THAT(GraphicEq::band_q_for_gain_db(12.0f),
               WithinAbs(GraphicEq::band_q_for_gain_db(-12.0f), 0.0001f));
  REQUIRE_THROWS(eq.center_frequency(31));
  REQUIRE_THROWS(eq.nearest_band(0.0f));
}

TEST_CASE("GraphicEq boosts selected band more than distant bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  GraphicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_gain_for_frequency(1000.0f, 6.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto distant = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float distant_before = rms_tail(distant, 4096);

  process(eq, center);
  eq.reset();
  process(eq, distant);

  const float center_gain = rms_tail(center, 4096) / center_before;
  const float distant_gain = rms_tail(distant, 4096) / distant_before;

  REQUIRE(center_gain > 1.8f);
  REQUIRE(distant_gain < 1.15f);
}

TEST_CASE("PultecEq low boost and attenuation shape low end", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  PultecEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_frequency(60.0f);
  eq.set_low_boost(5.0f);
  eq.set_low_attenuation(4.0f);

  auto sub = sine(50.0f, sample_rate, sample_rate);
  auto low_mid = sine(160.0f, sample_rate, sample_rate);
  auto high = sine(5000.0f, sample_rate, sample_rate);
  const float sub_before = rms_tail(sub, 4096);
  const float low_mid_before = rms_tail(low_mid, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, sub);
  eq.reset();
  process(eq, low_mid);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(sub, 4096) / sub_before > 1.25f);
  REQUIRE(rms_tail(low_mid, 4096) / low_mid_before < 0.9f);
  REQUIRE(rms_tail(high, 4096) / high_before < 1.1f);
}

TEST_CASE("PultecEq high boost and attenuation shape top end", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  PultecEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_high_boost(8000.0f, 5.0f, 0.8f);
  eq.set_high_attenuation(12000.0f, 4.0f);

  auto presence = sine(8000.0f, sample_rate, sample_rate);
  auto air = sine(16000.0f, sample_rate, sample_rate);
  const float presence_before = rms_tail(presence, 4096);
  const float air_before = rms_tail(air, 4096);

  process(eq, presence);
  eq.reset();
  process(eq, air);

  REQUIRE(rms_tail(presence, 4096) / presence_before > 1.3f);
  REQUIRE(rms_tail(air, 4096) / air_before < 1.0f);
}

TEST_CASE("PultecEq clear bypasses processing", "[mastering][eq]") {
  PultecEq eq;
  eq.prepare(48000.0, 512);
  eq.set_low_boost(8.0f);
  eq.set_high_boost(8000.0f, 8.0f);
  eq.clear();

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("PultecEq component model adds passive loss and output nonlinearity", "[mastering][eq]") {
  PultecEq curve;
  PultecEq component;
  curve.prepare(48000.0, 512);
  component.prepare(48000.0, 512);
  curve.set_low_boost(5.0f);
  component.set_low_boost(5.0f);
  component.set_component_model(PultecComponentModel::Eqp1aWdf);
  component.set_output_drive(6.0f);

  auto curve_audio = sine(60.0f, 48000, 4096, 0.8f);
  auto component_audio = curve_audio;
  process(curve, curve_audio);
  process(component, component_audio);

  REQUIRE(rms_tail(component_audio, 512) < rms_tail(curve_audio, 512));
  REQUIRE(peak_abs(component_audio) <= 1.1f);
  REQUIRE(component.component_model() == PultecComponentModel::Eqp1aWdf);
}

TEST_CASE("PultecEq component model preserves channel state when channel count grows",
          "[mastering][eq]") {
  PultecEq mono_path;
  PultecEq stereo_path;
  mono_path.prepare(48000.0, 512);
  stereo_path.prepare(48000.0, 512);
  mono_path.set_component_model(PultecComponentModel::Eqp1aWdf);
  stereo_path.set_component_model(PultecComponentModel::Eqp1aWdf);
  mono_path.set_low_boost(5.0f);
  stereo_path.set_low_boost(5.0f);

  auto warmup = sine(60.0f, 48000, 4096, 0.5f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  auto expected_left = sine(60.0f, 48000, 512, 0.5f);
  auto actual_left = expected_left;
  auto actual_right = expected_left;
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("ApiStyleEq snaps frequency and gain to stepped controls", "[mastering][eq]") {
  ApiStyleEq eq;
  eq.prepare(48000.0, 512);

  eq.set_band(ApiStyleEq::Band::LowMid, 520.0f, 5.1f);

  REQUIRE_THAT(eq.frequency(ApiStyleEq::Band::LowMid), WithinAbs(500.0f, 0.0001f));
  REQUIRE_THAT(eq.gain_db(ApiStyleEq::Band::LowMid), WithinAbs(6.0f, 0.0001f));
  REQUIRE_THAT(eq.snapped_frequency(ApiStyleEq::Band::High, 11000.0f),
               WithinAbs(10000.0f, 0.0001f));
  REQUIRE_THAT(eq.snapped_gain(-5.2f), WithinAbs(-6.0f, 0.0001f));
}

TEST_CASE("ApiStyleEq boosts selected band more than distant bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ApiStyleEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(ApiStyleEq::Band::LowMid, 500.0f, 6.0f);

  auto center = sine(500.0f, sample_rate, sample_rate);
  auto distant = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float distant_before = rms_tail(distant, 4096);

  process(eq, center);
  eq.reset();
  process(eq, distant);

  REQUIRE(rms_tail(center, 4096) / center_before > 1.8f);
  REQUIRE(rms_tail(distant, 4096) / distant_before < 1.15f);
}
