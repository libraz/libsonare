/// @file eq_processor_test.cpp
/// @brief EqualizerProcessor routing and dynamic behavior tests.

#include "eq_test_helpers.h"

TEST_CASE("EqualizerProcessor matches ParametricEq for E0 stereo zero-latency bands",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};

  ParametricEq reference;
  reference.prepare(sample_rate, 1024);
  reference.set_band(0, band);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, band);

  auto ref_left = sine(1000.0f, sample_rate, 4096);
  auto ref_right = sine(4000.0f, sample_rate, 4096);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == 0);
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor realtime parameter updates preserve IIR history", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 512;
  const EqBand band{EqBandType::Peak, 700.0f, 6.0f, 0.6f, true};

  EqualizerProcessor reference({1});
  reference.prepare(sample_rate, block);
  reference.set_band(0, band);
  EqualizerProcessor updated({1});
  updated.prepare(sample_rate, block);
  updated.set_band(0, band);

  auto ref = sine(143.0f, sample_rate, block * 4, 0.3f);
  auto got = ref;
  for (int offset = 0; offset < static_cast<int>(ref.size()); offset += block) {
    float* ref_channels[] = {ref.data() + offset};
    reference.process(ref_channels, 1, block);

    float* got_channels[] = {got.data() + offset};
    updated.process(got_channels, 1, block);
    if (offset == block) {
      REQUIRE(updated.set_parameter(1, band.gain_db));
    }
  }

  for (size_t i = 0; i < got.size(); ++i) {
    REQUIRE_THAT(got[i], WithinAbs(ref[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor rejects realtime-unsafe LinearPhase dynamic bands",
          "[mastering][eq]") {
  EqualizerProcessor eq;
  eq.prepare(48000.0, 512);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};
  band.phase = PhaseMode::LinearPhase;
  band.dyn.enabled = true;
  band.dyn.threshold_db = -30.0f;
  REQUIRE_THROWS(eq.set_band(0, band));
}

TEST_CASE("EqualizerProcessor routes stereo LinearPhase bands before IIR and reports latency",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 4096;

  EqBand linear_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  linear_band.phase = PhaseMode::LinearPhase;
  EqBand iir_band{EqBandType::HighShelf, 6000.0f, -3.0f, 0.8f, true};

  LinearPhaseEq linear_reference;
  linear_reference.prepare(sample_rate, block_size);
  linear_reference.prepare_channels(2);
  linear_reference.set_band(0, linear_band);
  ParametricEq iir_reference;
  iir_reference.prepare(sample_rate, block_size);
  iir_reference.set_band(0, iir_band);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, linear_band);
  eq.set_band(1, iir_band);

  auto ref_left = sine(1000.0f, sample_rate, block_size);
  auto ref_right = sine(7000.0f, sample_rate, block_size, 0.18f);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(linear_reference, ref_left, ref_right);
  process_stereo(iir_reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == linear_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor routes Left and Right LinearPhase stages with matched delay",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 2048;

  EqBand left_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  left_band.phase = PhaseMode::LinearPhase;
  left_band.placement = StereoPlacement::Left;

  LinearPhaseEq left_reference;
  left_reference.prepare(sample_rate, block_size);
  left_reference.prepare_channels(1);
  left_reference.set_band(0, left_band);
  LinearPhaseEq right_reference;
  right_reference.prepare(sample_rate, block_size);
  right_reference.prepare_channels(1);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, left_band);

  auto ref_left = sine(1000.0f, sample_rate, block_size);
  auto ref_right = sine(1000.0f, sample_rate, block_size);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process(left_reference, ref_left);
  process(right_reference, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == left_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor routes Mid and Side LinearPhase stages with matched delay",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 2048;

  EqBand mid_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  mid_band.phase = PhaseMode::LinearPhase;
  mid_band.placement = StereoPlacement::Mid;

  LinearPhaseEq mid_reference;
  mid_reference.prepare(sample_rate, block_size);
  mid_reference.prepare_channels(1);
  mid_reference.set_band(0, mid_band);
  LinearPhaseEq side_reference;
  side_reference.prepare(sample_rate, block_size);
  side_reference.prepare_channels(1);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, mid_band);

  auto left = sine(1000.0f, sample_rate, block_size);
  auto right = sine(3000.0f, sample_rate, block_size, 0.18f);
  std::vector<float> ref_mid(left.size());
  std::vector<float> ref_side(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    ref_mid[i] = (left[i] + right[i]) * 0.5f;
    ref_side[i] = (left[i] - right[i]) * 0.5f;
  }

  auto got_left = left;
  auto got_right = right;
  process(mid_reference, ref_mid);
  process(side_reference, ref_side);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == mid_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_mid[i] + ref_side[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_mid[i] - ref_side[i], 0.000001f));
  }

  float* mono[] = {got_left.data()};
  REQUIRE_THROWS(eq.process(mono, 1, static_cast<int>(got_left.size())));
}

TEST_CASE("EqualizerProcessor applies Left and Right placement independently", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);

  EqBand left_boost{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  left_boost.placement = StereoPlacement::Left;
  EqBand right_cut{EqBandType::Peak, 1000.0f, -6.0f, 1.0f, true};
  right_cut.placement = StereoPlacement::Right;
  eq.set_band(0, left_boost);
  eq.set_band(1, right_cut);

  auto left = sine(1000.0f, sample_rate, 4096);
  auto right = left;
  const float before = rms_tail(left, 512);
  process_stereo(eq, left, right);

  REQUIRE(rms_tail(left, 512) / before > 1.7f);
  REQUIRE(rms_tail(right, 512) / before < 0.6f);
}

TEST_CASE("EqualizerProcessor Mid and Side placement matches MidSideEq convention",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand mid_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  mid_band.placement = StereoPlacement::Mid;
  EqBand side_band{EqBandType::Peak, 3000.0f, -4.0f, 1.0f, true};
  side_band.placement = StereoPlacement::Side;

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, mid_band);
  eq.set_band(1, side_band);

  MidSideEq reference;
  reference.prepare(sample_rate, 4096);
  reference.set_mid_band(0, mid_band);
  reference.set_side_band(1, side_band);

  auto ref_left = sine(1000.0f, sample_rate, 4096);
  auto ref_right = sine(3000.0f, sample_rate, 4096, 0.18f);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor rejects Mid and Side placement on mono input", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000.0, 512);
  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};
  band.placement = StereoPlacement::Mid;
  eq.set_band(0, band);

  auto mono = sine(1000.0f, 48000, 512);
  float* channels[] = {mono.data()};
  REQUIRE_THROWS(eq.process(channels, 1, static_cast<int>(mono.size())));
}

TEST_CASE("EqualizerProcessor respects bypass and solo listen selection", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 1024);

  EqBand boost{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  boost.bypassed = true;
  eq.set_band(0, boost);

  auto bypassed = sine(1000.0f, sample_rate, 1024);
  const auto original = bypassed;
  process(eq, bypassed);
  for (size_t i = 0; i < bypassed.size(); ++i) {
    REQUIRE_THAT(bypassed[i], WithinAbs(original[i], 0.000001f));
  }

  eq.clear();
  EqBand selected{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  selected.soloed = true;
  EqBand ignored{EqBandType::Peak, 1000.0f, -12.0f, 1.0f, true};
  eq.set_band(0, selected);
  eq.set_band(1, ignored);

  ParametricEq reference;
  reference.prepare(sample_rate, 1024);
  reference.set_band(0, {EqBandType::BandPass, selected.frequency_hz, 0.0f, selected.q, true});

  auto expected = sine(1000.0f, sample_rate, 1024);
  auto actual = expected;
  process(reference, expected);
  process(eq, actual);
  for (size_t i = 0; i < actual.size(); ++i) {
    REQUIRE_THAT(actual[i], WithinAbs(expected[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor resolves inherited phase mode and enforces prepared channels",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand inherited{EqBandType::Peak, 9000.0f, 9.0f, 0.7f, true};

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 256);
  eq.set_phase_mode(PhaseMode::NaturalPhase);
  eq.set_band(0, inherited);

  ParametricEq reference;
  reference.prepare(sample_rate, 256);
  reference.set_band(0, {EqBandType::Peak, 9000.0f, 9.0f, 0.7f, true, BiquadCoeffMode::Vicanek});

  auto ref_left = sine(9000.0f, sample_rate, 256);
  auto ref_right = sine(1200.0f, sample_rate, 256);
  auto got_left = ref_left;
  auto got_right = ref_right;
  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }

  float* too_many[] = {got_left.data(), got_right.data(), got_left.data()};
  REQUIRE_THROWS(eq.process(too_many, 3, static_cast<int>(got_left.size())));
  float* stereo[] = {got_left.data(), got_right.data()};
  REQUIRE_THROWS(eq.process(stereo, 2, 257));
  REQUIRE_THROWS(eq.set_phase_mode(PhaseMode::Inherit));
  eq.clear();
  eq.set_phase_mode(PhaseMode::LinearPhase);
  eq.set_band(0, inherited);
  REQUIRE(eq.latency_samples() > 0);
}

TEST_CASE(
    "EqualizerProcessor per-band NaturalPhase uses Vicanek without changing the global default",
    "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand requested{EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Rbj};
  requested.phase = PhaseMode::NaturalPhase;

  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  REQUIRE(eq.phase_mode() == PhaseMode::ZeroLatency);
  eq.set_band(0, requested);
  REQUIRE(eq.band(0).coeff_mode == BiquadCoeffMode::Rbj);

  ParametricEq reference;
  reference.prepare(sample_rate, 512);
  reference.set_band(0, {EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Vicanek});

  auto expected = sine(12000.0f, sample_rate, 4096);
  auto actual = expected;
  process(reference, expected);
  process(eq, actual);

  double diff = 0.0;
  for (size_t i = 0; i < actual.size(); ++i) {
    diff += std::abs(static_cast<double>(actual[i] - expected[i]));
  }
  REQUIRE(diff < 1.0e-6);
}

TEST_CASE("EqualizerProcessor dynamic band attenuates above threshold on any of 24 bands",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  band.dyn.enabled = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(23, band);

  auto quiet = sine(1000.0f, sample_rate, 4096, 0.002f);
  auto loud = sine(1000.0f, sample_rate, 4096, 0.5f);
  const float quiet_before = rms_tail(quiet, 512);
  const float loud_before = rms_tail(loud, 512);

  process(eq, quiet);
  const float quiet_gain = rms_tail(quiet, 512) / quiet_before;
  const float quiet_applied = eq.last_applied_gain_db(23);
  eq.reset();
  process(eq, loud);
  const float loud_gain = rms_tail(loud, 512) / loud_before;

  REQUIRE(eq.last_band_detector_db(23) > -40.0f);
  REQUIRE(quiet_applied == 0.0f);
  REQUIRE(eq.last_applied_gain_db(23) < -3.0f);
  REQUIRE(loud_gain < quiet_gain * 0.8f);
}

TEST_CASE("EqualizerProcessor dynamic band can use an external sidechain", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  band.dyn.enabled = true;
  band.dyn.external_sidechain = true;
  band.dyn.threshold_db = -32.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(0, band);

  auto quiet = sine(1000.0f, sample_rate, 4096, 0.005f);
  auto internal = quiet;
  process(eq, internal);
  const float internal_gain = eq.last_applied_gain_db(0);

  eq.reset();
  quiet = sine(1000.0f, sample_rate, 4096, 0.005f);
  auto key = sine(1000.0f, sample_rate, 4096, 0.8f);
  const float* key_channels[] = {key.data()};
  eq.set_sidechain(key_channels, 1, static_cast<int>(key.size()));
  process(eq, quiet);

  REQUIRE_THAT(internal_gain, WithinAbs(0.0f, 0.0001f));
  REQUIRE(eq.last_band_detector_db(0) > -32.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -3.0f);
  REQUIRE(rms_tail(quiet, 512) < rms_tail(internal, 512) * 0.8f);
}

TEST_CASE("EqualizerProcessor validates and clears external sidechain buffers", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000, 128);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  const float* stereo_key[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(eq.set_sidechain(stereo_key, 2, 128));
  eq.clear_sidechain();
  REQUIRE_THROWS(eq.set_sidechain(nullptr, 1, 128));
  const float* bad_key[] = {left.data(), nullptr};
  REQUIRE_THROWS(eq.set_sidechain(bad_key, 2, 128));

  REQUIRE_NOTHROW(eq.set_sidechain(stereo_key, 2, 64));
  float* audio[] = {left.data(), right.data()};
  REQUIRE_THROWS(eq.process(audio, 2, 128));

  // A sidechain wider than the prepared max_channels (2) is rejected on the
  // control thread, so the audio-thread detector path never reallocates.
  std::array<float, 128> third{};
  const float* tri_key[] = {left.data(), right.data(), third.data()};
  REQUIRE_THROWS(eq.set_sidechain(tri_key, 3, 128));

  REQUIRE_NOTHROW(eq.set_sidechain(nullptr, 0, 0));
}

TEST_CASE("EqualizerProcessor dynamic auto-threshold is input-gain relative", "[mastering][eq]") {
  static constexpr int sample_rate = 48000;

  auto run = [](float amplitude) {
    EqualizerProcessor eq({1});
    eq.prepare(sample_rate, 4096);
    EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
    band.dyn.enabled = true;
    band.dyn.auto_threshold = true;
    band.dyn.ratio = 4.0f;
    band.dyn.range_db = -12.0f;
    band.dyn.attack_ms = 0.0f;
    band.dyn.release_ms = 10.0f;
    eq.set_band(0, band);
    auto audio = sine(1000.0f, sample_rate, 4096, amplitude);
    process(eq, audio);
    return eq.last_applied_gain_db(0);
  };

  const float quiet_gain = run(0.05f);
  const float loud_gain = run(0.5f);
  REQUIRE(quiet_gain < -2.0f);
  REQUIRE(loud_gain < -2.0f);
  REQUIRE_THAT(loud_gain, WithinAbs(quiet_gain, 0.2f));
}

TEST_CASE("EqualizerProcessor auto-gain compensates block RMS changes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand boost{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};

  EqualizerProcessor plain({1});
  plain.prepare(sample_rate, 4096);
  plain.set_band(0, boost);
  EqualizerProcessor compensated({1});
  compensated.prepare(sample_rate, 4096);
  compensated.set_band(0, boost);
  compensated.set_auto_gain_enabled(true);

  auto plain_audio = sine(1000.0f, sample_rate, 4096, 0.2f);
  auto compensated_audio = plain_audio;
  const float before = rms_tail(plain_audio, 512);
  process(plain, plain_audio);
  process(compensated, compensated_audio);

  REQUIRE(rms_tail(plain_audio, 512) > before * 3.0f);
  REQUIRE(rms_tail(compensated_audio, 512) < rms_tail(plain_audio, 512) * 0.55f);
  REQUIRE(compensated.last_auto_gain_db() < -6.0f);
}

TEST_CASE("EqualizerProcessor gain scale controls applied static and dynamic gain",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqualizerProcessor scaled({1});
  scaled.prepare(sample_rate, 4096);
  scaled.set_gain_scale(0.5f);
  EqBand peak{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  scaled.set_band(0, peak);

  ParametricEq reference;
  reference.prepare(sample_rate, 4096);
  reference.set_band(0, {EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});

  auto scaled_audio = sine(1000.0f, sample_rate, 4096, 0.1f);
  auto reference_audio = scaled_audio;
  process(scaled, scaled_audio);
  process(reference, reference_audio);
  REQUIRE_THAT(rms_tail(scaled_audio, 1024), WithinAbs(rms_tail(reference_audio, 1024), 0.002f));
  REQUIRE_THAT(scaled.spectrum_snapshot().band_gain_db[0], WithinAbs(3.0f, 0.0001f));

  EqualizerProcessor dynamic({1});
  dynamic.prepare(sample_rate, 4096);
  dynamic.set_gain_scale(0.5f);
  EqBand dyn{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  dyn.dyn.enabled = true;
  dyn.dyn.threshold_db = -40.0f;
  dyn.dyn.ratio = 4.0f;
  dyn.dyn.range_db = -12.0f;
  dyn.dyn.attack_ms = 0.0f;
  dyn.dyn.release_ms = 10.0f;
  dynamic.set_band(0, dyn);
  auto loud = sine(1000.0f, sample_rate, 4096, 0.5f);
  process(dynamic, loud);
  REQUIRE(dynamic.last_applied_gain_db(0) < -3.0f);
  REQUIRE(dynamic.last_applied_gain_db(0) > -6.1f);
  REQUIRE_THAT(dynamic.spectrum_snapshot().band_gain_db[0],
               WithinAbs(dynamic.last_applied_gain_db(0), 0.0001f));
}

TEST_CASE("EqualizerProcessor output gain and pan apply after EQ", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000, 128);
  eq.set_output_gain_db(6.0f);
  eq.set_output_pan(1.0f);

  std::vector<float> left(128, 0.25f);
  std::vector<float> right(128, 0.25f);
  process_stereo(eq, left, right);
  REQUIRE(peak_abs(left) < 0.000001f);
  REQUIRE_THAT(right[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));

  eq.reset();
  eq.set_output_pan(-1.0f);
  left.assign(128, 0.25f);
  right.assign(128, 0.25f);
  process_stereo(eq, left, right);
  REQUIRE_THAT(left[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));
  REQUIRE(peak_abs(right) < 0.000001f);

  eq.reset();
  eq.set_output_pan(1.0f);
  std::vector<float> mono(128, 0.25f);
  process(eq, mono);
  REQUIRE_THAT(mono[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));
}

TEST_CASE("EqualizerProcessor soloed band listens through a bandpass region", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand solo{EqBandType::Peak, 1000.0f, 12.0f, 4.0f, true};
  solo.soloed = true;

  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, solo);

  auto center = sine(1000.0f, sample_rate, 4096, 0.25f);
  auto distant = sine(8000.0f, sample_rate, 4096, 0.25f);
  const float center_before = rms_tail(center, 512);
  const float distant_before = rms_tail(distant, 512);
  process(eq, center);
  eq.reset();
  process(eq, distant);

  REQUIRE(rms_tail(center, 512) > center_before * 0.55f);
  REQUIRE(rms_tail(distant, 512) < distant_before * 0.2f);
}

TEST_CASE("EqualizerProcessor proportional Q narrows bell gain without mutating stored Q",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand regular{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  EqBand proportional = regular;
  proportional.proportional_q = true;
  proportional.proportional_q_strength = 0.04f;

  EqualizerProcessor wide({1});
  wide.prepare(sample_rate, 4096);
  wide.set_band(0, regular);
  EqualizerProcessor narrow({1});
  narrow.prepare(sample_rate, 4096);
  narrow.set_band(0, proportional);

  auto wide_edge = sine(1800.0f, sample_rate, 4096, 0.2f);
  auto narrow_edge = wide_edge;
  process(wide, wide_edge);
  process(narrow, narrow_edge);

  REQUIRE(rms_tail(narrow_edge, 512) < rms_tail(wide_edge, 512) * 0.85f);
  REQUIRE_THAT(narrow.band(0).q, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("EqualizerProcessor supports 30 kHz bands when sample rate allows it",
          "[mastering][eq]") {
  constexpr int sample_rate = 96000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, {EqBandType::Peak, 30000.0f, 9.0f, 2.0f, true, BiquadCoeffMode::Vicanek});

  auto center = sine(30000.0f, sample_rate, 4096, 0.2f);
  auto low = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float center_before = rms_tail(center, 512);
  const float low_before = rms_tail(low, 512);
  process(eq, center);
  eq.reset();
  process(eq, low);

  REQUIRE(rms_tail(center, 512) > center_before * 1.6f);
  REQUIRE(rms_tail(low, 512) < low_before * 1.2f);
}

TEST_CASE("EqualizerProcessor TiltShelf maps to opposite shelves", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(23, {EqBandType::TiltShelf, 1000.0f, 12.0f, 1.0f, true});

  auto low = sine(120.0f, sample_rate, 4096, 0.2f);
  auto high = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float low_before = rms_tail(low, 512);
  const float high_before = rms_tail(high, 512);
  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 512) < low_before * 0.65f);
  REQUIRE(rms_tail(high, 512) > high_before * 1.6f);
}

TEST_CASE("EqualizerProcessor FlatTilt tilts gently and differs from TiltShelf",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  // Correct tilt direction: lows cut, highs boosted around the pivot.
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, {EqBandType::FlatTilt, 1000.0f, 12.0f, 1.0f, true});
  auto low = sine(120.0f, sample_rate, 4096, 0.2f);
  auto high = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float low_before = rms_tail(low, 512);
  const float high_before = rms_tail(high, 512);
  process(eq, low);
  eq.reset();
  process(eq, high);
  REQUIRE(rms_tail(low, 512) < low_before);
  REQUIRE(rms_tail(high, 512) > high_before);

  // Near the pivot, FlatTilt's spread shelves cut less than TiltShelf's
  // Butterworth-Q shelf, proving the two band types are no longer identical.
  auto tilt_response = [&](EqBandType type, float frequency) {
    EqualizerProcessor probe({1});
    probe.prepare(sample_rate, 4096);
    probe.set_band(0, {type, 1000.0f, 12.0f, 1.0f, true});
    auto buffer = sine(frequency, sample_rate, 4096, 0.2f);
    process(probe, buffer);
    return rms_tail(buffer, 512);
  };
  const float flat_300 = tilt_response(EqBandType::FlatTilt, 300.0f);
  const float tilt_300 = tilt_response(EqBandType::TiltShelf, 300.0f);
  REQUIRE(flat_300 > tilt_300 * 1.05f);
}

TEST_CASE("EqualizerProcessor rejects internal backend overflow without partial commit",
          "[mastering][eq]") {
  EqualizerProcessor eq({1});
  eq.prepare(48000.0, 512);

  EqBand tilt{EqBandType::TiltShelf, 1000.0f, 6.0f, 1.0f, true};
  for (size_t i = 0; i < EqualizerProcessor::kMaxBands / 2; ++i) {
    tilt.frequency_hz = 200.0f + static_cast<float>(i) * 100.0f;
    eq.set_band(i, tilt);
  }

  tilt.frequency_hz = 3000.0f;
  REQUIRE_THROWS(eq.set_band(EqualizerProcessor::kMaxBands / 2, tilt));
  REQUIRE_FALSE(eq.band(EqualizerProcessor::kMaxBands / 2).enabled);

  auto audio = sine(1000.0f, 48000, 512);
  REQUIRE_NOTHROW(process(eq, audio));
}

TEST_CASE("EqualizerProcessor validates expanded backend capacity before prepare",
          "[mastering][eq]") {
  EqualizerProcessor eq({1});

  EqBand tilt{EqBandType::TiltShelf, 1000.0f, 6.0f, 1.0f, true};
  for (size_t i = 0; i < EqualizerProcessor::kMaxBands / 2; ++i) {
    tilt.frequency_hz = 200.0f + static_cast<float>(i) * 100.0f;
    eq.set_band(i, tilt);
  }

  tilt.frequency_hz = 3000.0f;
  REQUIRE_THROWS(eq.set_band(EqualizerProcessor::kMaxBands / 2, tilt));
  REQUIRE_FALSE(eq.band(EqualizerProcessor::kMaxBands / 2).enabled);

  REQUIRE_NOTHROW(eq.prepare(48000.0, 512));
}
