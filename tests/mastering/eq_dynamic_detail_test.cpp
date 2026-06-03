/// @file eq_dynamic_detail_test.cpp
/// @brief Dynamic EQ and component detail tests.

#include "eq_test_helpers.h"

TEST_CASE("DynamicEq applies cut only above threshold", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -20.0f, 4.0f, -9.0f, true});

  auto quiet = sine(1000.0f, sample_rate, sample_rate, 0.02f);
  auto loud = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(eq, quiet);
  const float quiet_gain_db = eq.last_applied_gain_db(0);
  eq.reset();
  process(eq, loud);
  const float loud_gain_db = eq.last_applied_gain_db(0);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 0.95f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 0.55f);
  REQUIRE_THAT(quiet_gain_db, WithinAbs(0.0f, 0.001f));
  REQUIRE(loud_gain_db < -6.0f);
}

TEST_CASE("DynamicEq supports upward dynamic boost", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 3.0f, 6.0f, true});

  auto loud = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  const float before = rms_tail(loud, 4096);

  process(eq, loud);

  REQUIRE(eq.last_applied_gain_db(0) > 4.0f);
  REQUIRE(rms_tail(loud, 4096) / before > 1.45f);
}

TEST_CASE("DynamicEq detects each band from its own frequency region", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});
  eq.set_band(1, {EqBandType::Peak, 8000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});

  auto low = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  process(eq, low);

  REQUIRE(eq.last_band_detector_db(0) > -10.0f);
  REQUIRE(eq.last_band_detector_db(1) < -40.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
  REQUIRE_THAT(eq.last_applied_gain_db(1), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("DynamicEq supports external sidechain for dynamic bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});

  auto program = sine(1000.0f, sample_rate, sample_rate, 0.02f);
  auto sidechain = sine(1000.0f, sample_rate, sample_rate, 0.8f);
  const float before = rms_tail(program, 4096);
  const float* sidechain_channels[] = {sidechain.data()};

  eq.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(eq, program);

  REQUIRE(eq.last_band_detector_db(0) > -6.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
  REQUIRE(rms_tail(program, 4096) / before < 0.7f);
}

TEST_CASE("DynamicEq supports tunable sidechain frequency and timing", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true, 2.0f, 8000.0f,
                  0.1f, 20.0f, 0.5f});

  auto program = sine(1000.0f, sample_rate, sample_rate, 0.2f);
  auto sidechain = sine(8000.0f, sample_rate, sample_rate, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};

  eq.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(eq, program);

  REQUIRE(eq.last_band_detector_db(0) > -8.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
}

TEST_CASE("DynamicEq validates band parameters", "[mastering][eq]") {
  DynamicEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(DynamicEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, -20.0f, 2.0f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, -20.0f, 2.0f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, -20.0f, 0.5f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, -20.0f, 2.0f, -6.0f, true, 0.0f}));
  std::vector<float> sidechain(4, 0.0f);
  const float* sidechain_channels[] = {sidechain.data()};
  eq.set_sidechain(sidechain_channels, 1, 4);
  std::vector<float> program(3, 0.0f);
  float* program_channels[] = {program.data()};
  REQUIRE_THROWS(eq.process(program_channels, 1, 3));
}

// ===================================================================
// Regression (Pultec corner frequency): the component-model one-pole
// charges now use a prewarped coefficient alpha = 1 - exp(-2*pi*f/fs)
// rather than the raw digital radian frequency 2*pi*f/fs, and the corner
// is clamped sub-Nyquist before the formula. At a high corner the old
// raw-omega form gave alpha = 2*pi*f/fs which can reach or exceed 2 (pole
// at 1-alpha <= -1), an unstable smoother; even between 1 and 2 it places
// the pole at the wrong (much higher) corner. The prewarped form keeps
// alpha in (0, 1) for any clamped corner so the one-pole stays a stable
// low-pass. Drive the Eqp1aWdf path hard at a high low-corner and assert
// the output stays finite and bounded (no divergence), and remains a
// bounded perturbation of the curve-only path.
// ===================================================================
TEST_CASE("PultecEq component one-pole corner stays stable at high frequencies",
          "[mastering][eq][pultec]") {
  constexpr int sample_rate = 48000;
  // 12 kHz low corner: raw-omega alpha = 2*pi*12000/48000 ~= 1.571; the
  // prewarped alpha = 1 - exp(-1.571) ~= 0.792 (and the corner is clamped to
  // 0.49*fs before the formula, guaranteeing alpha < 1).
  PultecEq curve;
  PultecEq component;
  curve.prepare(sample_rate, 1024);
  component.prepare(sample_rate, 1024);
  curve.set_low_frequency(12000.0f);
  component.set_low_frequency(12000.0f);
  curve.set_low_boost(6.0f);
  component.set_low_boost(6.0f);
  component.set_component_model(PultecComponentModel::Eqp1aWdf);
  component.set_output_drive(4.0f);

  auto curve_audio = sine(12000.0f, sample_rate, sample_rate, 0.7f);
  auto component_audio = curve_audio;
  process(curve, curve_audio);
  process(component, component_audio);

  // Stability: an unstable (alpha >= 2) one-pole would diverge unboundedly.
  // With the prewarped, clamped coefficient the component output stays finite
  // and bounded.
  for (float sample : component_audio) {
    REQUIRE(std::isfinite(sample));
  }
  REQUIRE(peak_abs(component_audio) <= 2.0f);
  // The component path remains a bounded perturbation of the curve path, not an
  // exploded signal: RMS ratio within a few dB of unity.
  const float curve_rms = rms_tail(curve_audio, 8192);
  const float component_rms = rms_tail(component_audio, 8192);
  REQUIRE(curve_rms > 0.0f);
  REQUIRE(component_rms > 0.0f);
  const float ratio = component_rms / curve_rms;
  REQUIRE(ratio > 0.25f);
  REQUIRE(ratio < 4.0f);
}

// ===================================================================
// Regression (Pultec corner frequency correctness): the component
// one-pole low charge is a low-pass with coefficient
// alpha = 1 - exp(-2*pi*f/fs), so its DC step response settles with time
// constant tau ~= fs / (2*pi*f) samples. The corner therefore controls
// the settling speed: a LOWER configured low-frequency must produce a
// SLOWER (longer) settle, scaling roughly inversely with frequency. We
// observe the smoother through the Eqp1aWdf reactive term (monotone in
// the low charge) by measuring the 63% settle index of the component
// output for a DC step at several corners. A miswired/wrong-corner
// coefficient (e.g. the old raw-omega form, or a fixed corner) would not
// reproduce this 1/f settle-time scaling.
// ===================================================================
TEST_CASE("PultecEq component low corner controls one-pole settle time",
          "[mastering][eq][pultec]") {
  constexpr int sample_rate = 48000;

  // Returns the 63% DC-step settle index (samples) of the Eqp1aWdf component
  // output for a given low corner frequency.
  auto settle_index = [&](float corner_hz) {
    PultecEq eq;
    eq.prepare(sample_rate, 8192);
    eq.set_component_model(PultecComponentModel::Eqp1aWdf);
    eq.set_low_frequency(corner_hz);
    eq.set_low_boost(8.0f);  // Maximise the low_reactive contribution.
    eq.set_output_drive(0.0f);

    const int total = 2048;
    std::vector<float> out(static_cast<size_t>(total), 0.1f);  // DC step
    process(eq, out);
    for (float sample : out) REQUIRE(std::isfinite(sample));
    const float base = out[0];
    const float steady = out[static_cast<size_t>(total - 1)];
    const float target = base + 0.632f * (steady - base);
    for (int i = 0; i < total; ++i) {
      const bool reached = (steady >= base) ? out[static_cast<size_t>(i)] >= target
                                            : out[static_cast<size_t>(i)] <= target;
      if (reached) return i;
    }
    return total;
  };

  const int s_low = settle_index(200.0f);
  const int s_mid = settle_index(400.0f);
  const int s_high = settle_index(2000.0f);

  // Settle time scales inversely with corner frequency: lower corner = slower.
  REQUIRE(s_low > s_mid);
  REQUIRE(s_mid > s_high);
  // Halving the corner (400 -> 200 Hz) should roughly double the settle index.
  // Allow a generous band to absorb the constant lag from the high one-pole and
  // the makeup-gain/tanh stage, while still rejecting a wrong (non-1/f) corner.
  const double ratio = static_cast<double>(s_low) / static_cast<double>(std::max(s_mid, 1));
  REQUIRE(ratio > 1.5);
  REQUIRE(ratio < 2.5);
  // The realised settle for the low corner must be in the neighbourhood of the
  // prewarped time constant tau = fs / (2*pi*f), not orders of magnitude off.
  const double tau_low = 1.0 / (2.0 * kPiD * 200.0 / sample_rate);
  REQUIRE(static_cast<double>(s_low) > tau_low);        // a real lag exists
  REQUIRE(static_cast<double>(s_low) < tau_low * 3.0);  // and is near tau
}

// ===================================================================
// Regression (DynamicEq sample-rate smoothing): the per-band gain is now
// smoothed at SAMPLE rate (coefficients refreshed every 32 samples)
// instead of a single once-per-block step. Feed a level step that
// triggers a multi-dB dynamic change, process in consecutive blocks, and
// assert the OUTPUT has no single-sample discontinuity at the block
// boundary -- i.e. the per-sample change stays bounded. A once-per-block
// gain step would jump the gain by several dB exactly at the boundary.
// ===================================================================
TEST_CASE("DynamicEq gain change is smooth across block boundaries",
          "[mastering][eq][dynamic-smooth]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 256;
  constexpr int blocks = 24;
  constexpr int total = block * blocks;
  DynamicEq eq;
  eq.prepare(sample_rate, block);
  // Heavy downward band so the dynamic gain swings many dB once it engages.
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.5f, -36.0f, 8.0f, -18.0f, true});

  // A 1 kHz tone whose amplitude steps up partway through (mid-block) so the
  // detector crosses threshold and the gain must move several dB.
  std::vector<float> input(static_cast<size_t>(total), 0.0f);
  const int step_at = total / 2;
  for (int i = 0; i < total; ++i) {
    const float amp = i < step_at ? 0.01f : 0.6f;
    input[static_cast<size_t>(i)] =
        amp * static_cast<float>(std::sin(2.0 * kPiD * 1000.0 * i / sample_rate));
  }

  std::vector<float> output = input;
  for (int pos = 0; pos < total; pos += block) {
    float* channels[] = {output.data() + pos};
    eq.process(channels, 1, block);
  }
  for (float sample : output) REQUIRE(std::isfinite(sample));

  // The dynamic gain engaged (multi-dB cut) by the end.
  REQUIRE(eq.last_applied_gain_db(0) < -3.0f);

  // The natural per-sample slope of a 1 kHz tone at amplitude 0.6 is
  // 0.6 * sin(2*pi*1000/48000) ~= 0.078. A once-per-block gain jump of several
  // dB applied to a ~0.6 amplitude sine would produce per-sample deltas far
  // larger than this exactly at block boundaries. Check the boundary samples
  // do not spike relative to the natural slope. We restrict to the steady loud
  // region (after the step) where the gain is actively moving toward target.
  const float natural_step = 0.6f * std::sin(sonare::constants::kTwoPi * 1000.0f / sample_rate);
  const float tolerance = 3.0f * natural_step;
  for (int b = 1; b < blocks; ++b) {
    const int boundary = b * block;
    if (boundary <= step_at + block) continue;  // skip the transient at the step
    const float delta =
        std::abs(output[static_cast<size_t>(boundary)] - output[static_cast<size_t>(boundary - 1)]);
    INFO("boundary sample index: " << boundary);
    REQUIRE(delta < tolerance);
  }
}

// ===================================================================
// Regression (DynamicEq steady-state stability): a constant input must
// eventually yield a stable applied gain (the sample-rate smoother
// converges to the target rather than re-stepping every block).
// ===================================================================
TEST_CASE("DynamicEq converges to a stable applied gain on constant input",
          "[mastering][eq][dynamic-smooth]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 256;
  DynamicEq eq;
  eq.prepare(sample_rate, block);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.5f, -30.0f, 6.0f, -12.0f, true});

  auto block_buf = [&]() { return sine(1000.0f, sample_rate, block, 0.5f); };

  float prev_gain = 0.0f;
  float last_gain = 0.0f;
  // Drive many identical blocks; the applied gain must settle.
  for (int b = 0; b < 80; ++b) {
    auto buffer = block_buf();
    float* channels[] = {buffer.data()};
    eq.process(channels, 1, block);
    prev_gain = last_gain;
    last_gain = eq.last_applied_gain_db(0);
    for (float sample : buffer) REQUIRE(std::isfinite(sample));
  }
  // Engaged and converged: the gain moved a meaningful amount and the last two
  // blocks agree closely (steady state, no per-block re-stepping).
  REQUIRE(last_gain < -3.0f);
  REQUIRE_THAT(last_gain, WithinAbs(prev_gain, 0.25f));
}

// ===================================================================
// Regression (DynamicEq detector continuity): the sidechain detector
// keeps persistent state (filters, envelope, lookahead ring) across
// blocks, so a transient straddling a block boundary is detected
// correctly and is not missed by an old within-block reset/clamp. Place
// a burst near the end of one block extending into the next and assert
// the per-band detector responds. Detection may LAG by up to the
// lookahead, never lead.
// ===================================================================
TEST_CASE("DynamicEq detector responds to a transient crossing a block boundary",
          "[mastering][eq][dynamic-detector]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 256;
  DynamicEq eq;
  eq.prepare(sample_rate, block);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.5f, -30.0f, 4.0f, -9.0f, true});

  // First block: quiet up to the last few samples, then a loud burst that
  // continues into the second block (the transient straddles the boundary).
  std::vector<float> blk0(static_cast<size_t>(block), 0.0f);
  std::vector<float> blk1(static_cast<size_t>(block), 0.0f);
  const int burst_start = block - 16;  // last 16 samples of block 0
  for (int i = 0; i < block; ++i) {
    const float t0 = static_cast<float>(i) / sample_rate;
    const float t1 = static_cast<float>(i + block) / sample_rate;
    if (i >= burst_start) {
      blk0[static_cast<size_t>(i)] = 0.7f * std::sin(sonare::constants::kTwoPi * 1000.0f * t0);
    }
    blk1[static_cast<size_t>(i)] = 0.7f * std::sin(sonare::constants::kTwoPi * 1000.0f * t1);
  }

  float* ch0[] = {blk0.data()};
  eq.process(ch0, 1, block);
  // The detector envelope is continuous across blocks; processing block 1
  // (still loud) must show the detector well above the quiet floor, proving
  // the burst that began at the boundary was not dropped.
  float* ch1[] = {blk1.data()};
  eq.process(ch1, 1, block);

  REQUIRE(eq.last_band_detector_db(0) > -20.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -2.0f);
  for (float sample : blk0) REQUIRE(std::isfinite(sample));
  for (float sample : blk1) REQUIRE(std::isfinite(sample));
}

// ===================================================================
// Regression (LinearPhaseEq no silence gap on reconfigure): a band /
// parameter change now rebuilds the FIR taps WITHOUT zeroing the
// convolver history, so changing a band mid-stream no longer produces a
// ~kernel-length silence gap. Fill the FIR history with a steady tone,
// change a band, process the next block, and assert the output right
// after the change is NOT silenced for kernel-length samples.
// ===================================================================
TEST_CASE("LinearPhaseEq band change does not silence the convolver history",
          "[mastering][eq][linear-phase]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 256;
  LinearPhaseEq eq({1024, 257});
  eq.prepare(sample_rate, block);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});

  // Fill the FIR history with a steady tone past the latency region.
  const int warmup_blocks = 8;
  float warm_rms = 0.0f;
  for (int b = 0; b < warmup_blocks; ++b) {
    auto buffer = sine(1000.0f, sample_rate, block, 0.5f);
    process(eq, buffer);
    warm_rms = rms_tail(buffer, 0);
    for (float sample : buffer) REQUIRE(std::isfinite(sample));
  }
  REQUIRE(warm_rms > 0.0f);

  // Change a band parameter mid-stream. The reconfigure() path must refresh
  // the FIR taps without clearing the convolver history.
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  // Process the very next block: with the old (history-zeroing) behaviour the
  // output would be near-silent for ~kernel_size samples. With the fix the
  // output stays comparable to the warmed-up level immediately.
  auto next = sine(1000.0f, sample_rate, block, 0.5f);
  process(eq, next);
  for (float sample : next) REQUIRE(std::isfinite(sample));

  const float next_rms = rms_tail(next, 0);
  INFO("warm_rms=" << warm_rms << " next_rms=" << next_rms);
  // No silence gap: the block right after the change must retain most of its
  // energy (>= 50% of the warmed-up RMS). A kernel-length zeroing would push
  // this near zero (this 256-sample block is shorter than the 257-tap kernel,
  // so a history reset would silence essentially the whole block).
  REQUIRE(next_rms > warm_rms * 0.5f);
}
