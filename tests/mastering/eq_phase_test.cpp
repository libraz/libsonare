/// @file eq_phase_test.cpp
/// @brief Linear and minimum phase EQ tests.

#include "eq_test_helpers.h"

TEST_CASE("TiltEq positive tilt brightens highs relative to lows", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  TiltEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_tilt_db(6.0f);
  eq.set_pivot_hz(1000.0f);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain < 0.9f);
  REQUIRE(high_gain > 1.1f);
  REQUIRE(high_gain > low_gain * 1.5f);
}

TEST_CASE("TiltEq zero tilt is bypassed", "[mastering][eq]") {
  TiltEq eq;
  eq.prepare(48000.0, 512);
  eq.set_tilt_db(0.0f);

  auto audio = sine(2000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("LinearPhaseEq stays click-free across mixed (ragged) block sizes", "[mastering][eq]") {
  // Regression: the partitioned-convolver fast path and the direct fallback kept
  // disjoint filter state, so a ragged (non-partition-aligned) block processed by
  // the direct path ran against empty history and lost the convolution tail. The
  // convolver-driven instance must now match an always-direct reference exactly,
  // for any sequence of block sizes.
  const EqBand band{EqBandType::HighShelf, 6000.0f, -4.0f, 0.8f, true};

  LinearPhaseEqConfig direct_config{1024, 257};
  direct_config.use_partitioned_convolution = false;  // always the direct path
  LinearPhaseEq reference(direct_config);
  reference.prepare(48000.0, 128);
  reference.set_band(0, band);

  LinearPhaseEqConfig conv_config{1024, 257};
  conv_config.partition_size = 128;  // convolver drives the aligned blocks
  LinearPhaseEq convolved(conv_config);
  convolved.prepare(48000.0, 128);
  convolved.set_band(0, band);

  // Aligned blocks (128) followed by a ragged tail (64), then more aligned and
  // another ragged block mid-stream — exercises both end-of-stream and
  // interior raggedness.
  const std::array<int, 6> block_sizes{128, 128, 64, 128, 96, 128};
  int total = 0;
  for (int b : block_sizes) total += b;

  auto input = sine(2000.0f, 48000, total);
  auto expected = input;
  auto actual = input;

  const auto run = [&](LinearPhaseEq& eq, std::vector<float>& buffer) {
    int offset = 0;
    for (int b : block_sizes) {
      float* channels[] = {buffer.data() + offset};
      eq.process(channels, 1, b);
      offset += b;
    }
  };
  run(reference, expected);
  run(convolved, actual);

  // FFT-domain convolver vs time-domain direct differ only by float rounding.
  REQUIRE(max_abs_difference(expected, actual) < 1.0e-3f);
}

TEST_CASE("LinearPhaseEq exposes symmetric FIR kernel and latency", "[mastering][eq]") {
  LinearPhaseEq eq({1024, 257});
  eq.prepare(48000.0, 512);

  REQUIRE(eq.latency_samples() == 128);
  REQUIRE(eq.kernel().size() == 257);
  for (size_t i = 0; i < eq.kernel().size() / 2; ++i) {
    REQUIRE_THAT(eq.kernel()[i], WithinAbs(eq.kernel()[eq.kernel().size() - 1 - i], 0.000001f));
  }
}

TEST_CASE("LinearPhaseEq resolution presets select deterministic FFT and FIR sizes",
          "[mastering][eq]") {
  LinearPhaseEqConfig low;
  low.resolution = LinearPhaseEqConfig::Resolution::Low;
  low.partition_size = 512;
  LinearPhaseEq low_eq(low);
  low_eq.prepare(48000.0, 512);

  LinearPhaseEqConfig maximum;
  maximum.resolution = LinearPhaseEqConfig::Resolution::Maximum;
  maximum.partition_size = 512;
  // Explicit sizes are ignored when a named resolution is selected.
  maximum.fft_size = 2048;
  maximum.kernel_size = 513;
  LinearPhaseEq max_eq(maximum);
  max_eq.prepare(48000.0, 512);

  REQUIRE(low_eq.kernel().size() == 1025);
  REQUIRE(low_eq.latency_samples() == 512);
  REQUIRE(max_eq.kernel().size() == 16385);
  REQUIRE(max_eq.latency_samples() == 8192);
}

TEST_CASE("EqualizerProcessor forwards LinearPhase resolution config to FIR stages",
          "[mastering][eq]") {
  LinearPhaseEqConfig linear_config;
  linear_config.resolution = LinearPhaseEqConfig::Resolution::Low;
  linear_config.partition_size = 512;

  EqualizerProcessorConfig eq_config;
  eq_config.max_channels = 2;
  eq_config.linear_phase_config = linear_config;
  EqualizerProcessor eq(eq_config);
  eq.prepare(48000.0, 512);
  eq.set_phase_mode(PhaseMode::LinearPhase);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});

  REQUIRE(eq.latency_samples() == 512);
  REQUIRE(eq.latency_samples_q8() == (512 << 8));
}

TEST_CASE("EqualizerProcessor high-pass slope controls attenuation depth", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{6, 12, 24, 48, 72, 96};

  float previous_stop_gain = 1.0f;
  for (int slope : slopes) {
    EqualizerProcessor eq({1});
    eq.prepare(sample_rate, samples);
    EqBand band{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
    band.slope_db_oct = slope;
    eq.set_band(0, band);

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

TEST_CASE("EqualizerProcessor applies cut resonance consistently in cascades", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqBand flat_band{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  flat_band.slope_db_oct = 48;
  EqualizerProcessor flat({1});
  flat.prepare(sample_rate, sample_rate);
  flat.set_band(0, flat_band);

  EqBand resonant_band = flat_band;
  resonant_band.q = 2.0f;
  EqualizerProcessor resonant({1});
  resonant.prepare(sample_rate, sample_rate);
  resonant.set_band(0, resonant_band);

  auto flat_cutoff = sine(1000.0f, sample_rate, sample_rate);
  auto resonant_cutoff = flat_cutoff;
  const float before = rms_tail(flat_cutoff, 4096);
  process(flat, flat_cutoff);
  process(resonant, resonant_cutoff);

  REQUIRE(rms_tail(resonant_cutoff, 4096) / before > rms_tail(flat_cutoff, 4096) / before * 1.2f);
}

TEST_CASE("EqualizerProcessor brickwall cuts route through FIR latency and rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqualizerProcessorConfig config;
  config.max_channels = 1;
  config.linear_phase_config.resolution = LinearPhaseEqConfig::Resolution::Low;
  config.linear_phase_config.partition_size = 512;
  EqualizerProcessor eq(config);
  eq.prepare(sample_rate, sample_rate);

  EqBand high_pass{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  high_pass.slope_db_oct = 0;
  eq.set_band(0, high_pass);

  REQUIRE(eq.latency_samples() == 512);
  REQUIRE(eq.latency_samples_q8() == (512 << 8));

  auto stop = sine(250.0f, sample_rate, sample_rate);
  auto pass = sine(8000.0f, sample_rate, sample_rate);
  const float stop_before = rms_tail(stop, 8192);
  const float pass_before = rms_tail(pass, 8192);

  process(eq, stop);
  eq.reset();
  process(eq, pass);

  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);

  EqBand low_pass{EqBandType::LowPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  low_pass.slope_db_oct = 0;
  eq.set_band(0, low_pass);

  pass = sine(100.0f, sample_rate, sample_rate);
  stop = sine(8000.0f, sample_rate, sample_rate);
  const float low_pass_before = rms_tail(pass, 8192);
  const float high_stop_before = rms_tail(stop, 8192);

  process(eq, pass);
  eq.reset();
  process(eq, stop);

  REQUIRE(rms_tail(pass, 8192) / low_pass_before > 0.85f);
  REQUIRE(rms_tail(stop, 8192) / high_stop_before < 0.002f);
}

TEST_CASE("EQ phase modes expose expected impulse timing", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};

  ParametricEq zero;
  zero.prepare(sample_rate, 512);
  zero.set_band(0, band);
  MinimumPhaseEq natural;
  natural.prepare(sample_rate, 512);
  natural.set_band(0, band);
  LinearPhaseEq linear({1024, 257});
  linear.prepare(sample_rate, 512);
  linear.set_band(0, band);

  auto zero_impulse = std::vector<float>(512, 0.0f);
  auto natural_impulse = zero_impulse;
  auto linear_impulse = zero_impulse;
  zero_impulse[0] = 1.0f;
  natural_impulse[0] = 1.0f;
  linear_impulse[0] = 1.0f;
  process(zero, zero_impulse);
  process(natural, natural_impulse);
  process(linear, linear_impulse);

  const auto peak_index = [](const std::vector<float>& samples) {
    return static_cast<size_t>(
        std::max_element(samples.begin(), samples.end(),
                         [](float a, float b) { return std::abs(a) < std::abs(b); }) -
        samples.begin());
  };

  REQUIRE(zero.latency_samples() == 0);
  REQUIRE(natural.latency_samples() == 0);
  REQUIRE(linear.latency_samples() == 128);
  REQUIRE(peak_index(zero_impulse) == 0);
  REQUIRE(peak_index(natural_impulse) == 0);
  REQUIRE(peak_index(linear_impulse) == static_cast<size_t>(linear.latency_samples()));
}

TEST_CASE("LinearPhaseEq flat response is delayed but otherwise transparent", "[mastering][eq]") {
  LinearPhaseEq eq({1024, 129});
  eq.prepare(48000.0, 512);

  std::vector<float> impulse(256, 0.0f);
  impulse[0] = 1.0f;

  process(eq, impulse);

  for (int i = 0; i < eq.latency_samples(); ++i) {
    REQUIRE_THAT(impulse[static_cast<size_t>(i)], WithinAbs(0.0f, 0.000001f));
  }
  REQUIRE_THAT(impulse[static_cast<size_t>(eq.latency_samples())], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("LinearPhaseEq high-pass attenuates lows while preserving highs", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  LinearPhaseEq eq({2048, 513});
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, kButterworthQ, true});

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(4000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 8192);
  const float high_before = rms_tail(high, 8192);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 8192) / low_before;
  const float high_gain = rms_tail(high, 8192) / high_before;

  REQUIRE(low_gain < 0.35f);
  REQUIRE(high_gain > 0.85f);
}

TEST_CASE("LinearPhaseEq kernel follows RBJ biquad magnitude", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  LinearPhaseEq eq({8192, 2049, false});
  eq.prepare(sample_rate, 1024);
  const EqBand band{EqBandType::Peak, 1000.0f, 6.0f, kButterworthQ, true};
  eq.set_band(0, band);

  const auto coeffs = sonare::rt::rbj_peak(
      static_cast<float>(2.0 * kPiD * band.frequency_hz / sample_rate), band.q, band.gain_db);

  for (double frequency_hz : {250.0, 1000.0, 4000.0}) {
    const float expected = sonare::rt::biquad_magnitude(
        coeffs, static_cast<float>(2.0 * kPiD * frequency_hz / sample_rate));
    const float actual = kernel_magnitude_at(eq.kernel(), frequency_hz, sample_rate);
    INFO("frequency: " << frequency_hz);
    REQUIRE_THAT(actual, WithinAbs(expected, 0.08f));
  }
}

TEST_CASE("LinearPhaseEq partitioned convolution matches direct convolution", "[mastering][eq]") {
  LinearPhaseEq direct({1024, 257, false});
  LinearPhaseEq partitioned({1024, 257, true, 128});
  direct.prepare(48000.0, 128);
  partitioned.prepare(48000.0, 128);

  const EqBand band{EqBandType::Peak, 2500.0f, 4.0f, 1.2f, true};
  direct.set_band(0, band);
  partitioned.set_band(0, band);

  auto direct_signal = sine(700.0f, 48000, 1024, 0.25f);
  auto partitioned_signal = direct_signal;
  process(direct, direct_signal);
  process(partitioned, partitioned_signal);

  for (size_t i = 0; i < direct_signal.size(); ++i) {
    REQUIRE_THAT(partitioned_signal[i], WithinAbs(direct_signal[i], 0.00001f));
  }
}

TEST_CASE("LinearPhaseEq validates configuration and bands", "[mastering][eq]") {
  REQUIRE_THROWS(LinearPhaseEq({1000, 257}));
  REQUIRE_THROWS(LinearPhaseEq({1024, 258}));
  REQUIRE_THROWS(LinearPhaseEq({1024, 2049}));

  LinearPhaseEq eq({1024, 257});
  eq.prepare(48000.0, 512);
  REQUIRE_THROWS(eq.set_band(LinearPhaseEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}

TEST_CASE("MinimumPhaseEq reports zero latency and processes immediately", "[mastering][eq]") {
  MinimumPhaseEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE(eq.latency_samples() == 0);

  std::vector<float> impulse(32, 0.0f);
  impulse[0] = 1.0f;

  process(eq, impulse);

  REQUIRE_THAT(impulse[0], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("MinimumPhaseEq high-pass attenuates low frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MinimumPhaseEq eq;
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

TEST_CASE("MinimumPhaseEq forces Vicanek natural-phase coefficients", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand requested{EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Rbj};

  MinimumPhaseEq natural;
  natural.prepare(sample_rate, 1024);
  natural.set_band(0, requested);

  REQUIRE(natural.band(0).coeff_mode == BiquadCoeffMode::Vicanek);
  REQUIRE(natural.band(0).phase == PhaseMode::NaturalPhase);

  ParametricEq vicanek;
  vicanek.prepare(sample_rate, 1024);
  vicanek.set_band(0, {EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Vicanek});

  ParametricEq rbj;
  rbj.prepare(sample_rate, 1024);
  rbj.set_band(0, requested);

  auto natural_audio = sine(12000.0f, sample_rate, 4096);
  auto vicanek_audio = natural_audio;
  auto rbj_audio = natural_audio;
  process(natural, natural_audio);
  process(vicanek, vicanek_audio);
  process(rbj, rbj_audio);

  double natural_vs_vicanek = 0.0;
  double natural_vs_rbj = 0.0;
  for (size_t i = 0; i < natural_audio.size(); ++i) {
    natural_vs_vicanek += std::abs(static_cast<double>(natural_audio[i] - vicanek_audio[i]));
    natural_vs_rbj += std::abs(static_cast<double>(natural_audio[i] - rbj_audio[i]));
  }

  REQUIRE(natural_vs_vicanek < 1.0e-6);
  REQUIRE(natural_vs_rbj > 0.01);
}

TEST_CASE("MinimumPhaseEq validates bands through its own API", "[mastering][eq]") {
  MinimumPhaseEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(MinimumPhaseEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}
