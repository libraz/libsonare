/// @file mixing_bus_control_test.cpp
/// @brief Mixing bus, VCA, controller, delay, width, and meter tests.

#include "mixing_test_helpers.h"

TEST_CASE("SendProcessor exposes pre and post fader timing", "[mixing]") {
  sonare::mixing::SendProcessor send({-3.0f, sonare::mixing::SendTiming::PreFader, 0.0f});

  REQUIRE(send.timing() == sonare::mixing::SendTiming::PreFader);
  send.set_timing(sonare::mixing::SendTiming::PostFader);
  REQUIRE(send.timing() == sonare::mixing::SendTiming::PostFader);
}

TEST_CASE("VcaGroup applies relative gain offset to members", "[mixing]") {
  sonare::mixing::GainProcessor first({0.0f, 0.0f});
  sonare::mixing::GainProcessor second({-6.0f, 0.0f});
  sonare::mixing::VcaGroup vca;

  REQUIRE(vca.add_member(&first));
  REQUIRE(vca.add_member(&second));
  vca.set_vca_gain_db(-3.0f);

  REQUIRE_THAT(first.vca_offset_db(), WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(second.vca_offset_db(), WithinAbs(-3.0f, 0.0001f));
}

TEST_CASE("Manual VCA trim and group membership accumulate independently", "[mixing]") {
  // Regression: a direct set_vca_offset_db() used to overwrite the strip's whole
  // offset, discarding every VCA group's accumulated contribution. The manual
  // trim and the group offset must now sum without either stomping the other.
  sonare::mixing::GainProcessor gain({0.0f, 0.0f});

  sonare::mixing::VcaGroup group_a;
  sonare::mixing::VcaGroup group_b;
  group_a.set_vca_gain_db(-3.0f);
  group_b.set_vca_gain_db(-2.0f);
  REQUIRE(group_a.add_member(&gain));
  REQUIRE(group_b.add_member(&gain));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-5.0f, 0.0001f));  // -3 + -2

  // A direct manual trim adds on top without disturbing the group total.
  gain.set_vca_offset_db(1.5f);
  REQUIRE_THAT(gain.vca_trim_offset_db(), WithinAbs(1.5f, 0.0001f));
  REQUIRE_THAT(gain.vca_group_offset_db(), WithinAbs(-5.0f, 0.0001f));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-3.5f, 0.0001f));  // 1.5 + (-5)

  // Removing one group only subtracts its own contribution; the manual trim and
  // the other group remain intact.
  REQUIRE(group_b.remove_member(&gain));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-1.5f, 0.0001f));  // 1.5 + (-3)
}

TEST_CASE("MixerController computes solo implied mute outside audio thread", "[mixing]") {
  sonare::mixing::ChannelStrip vocal;
  sonare::mixing::ChannelStrip drums;
  sonare::mixing::ChannelStrip reverb;
  sonare::mixing::MixerController controller;

  REQUIRE(controller.add_strip(&vocal));
  REQUIRE(controller.add_strip(&drums));
  REQUIRE(controller.add_strip(&reverb));
  controller.set_solo_safe(reverb, true);
  controller.set_solo(vocal, true);

  REQUIRE_FALSE(vocal.effectively_muted());
  REQUIRE(drums.effectively_muted());
  REQUIRE_FALSE(reverb.effectively_muted());
}

TEST_CASE("AlignmentDelay reports and applies integer latency", "[mixing]") {
  std::array<float, 4> mono{1.0f, 2.0f, 3.0f, 4.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::AlignmentDelay delay(2);

  delay.prepare(48000.0, 4);
  delay.process(channels, 1, 4);

  REQUIRE(delay.latency_samples() == 2);
  REQUIRE_THAT(mono[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(mono[1], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(mono[2], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("AlignmentDelay clamps pathological delays so the Q8 shift never overflows", "[mixing]") {
  // delay_samples_q8_ is delay_samples << 8; an unclamped huge request would
  // overflow signed int and wrap to a negative Q8 latency. The clamp keeps both
  // the integer and Q8 reports non-negative and monotonic.
  sonare::mixing::AlignmentDelay delay(std::numeric_limits<int>::max());
  REQUIRE(delay.delay_samples() >= 0);
  REQUIRE(delay.delay_samples_q8() >= 0);
  REQUIRE(delay.delay_samples_q8() == (delay.delay_samples() << 8));

  delay.set_delay_samples(std::numeric_limits<int>::max());
  REQUIRE(delay.delay_samples() >= 0);
  REQUIRE(delay.delay_samples_q8() >= 0);
  REQUIRE(delay.delay_samples_q8() == (delay.delay_samples() << 8));
}

TEST_CASE("AlignmentDelay reports Q8 fractional latency and interpolates impulse", "[mixing]") {
  std::array<float, 6> mono{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::AlignmentDelay delay;

  delay.set_delay_samples_q8((1 << 8) + 128);
  delay.prepare(48000.0, 6);
  delay.process(channels, 1, 6);

  REQUIRE(delay.delay_samples() == 1);
  REQUIRE(delay.delay_samples_q8() == ((1 << 8) + 128));
  REQUIRE(delay.latency_samples_q8() == ((1 << 8) + 128));
  REQUIRE(delay.fractional_mode() == sonare::mixing::FractionalDelayMode::Lagrange3);

  float abs_sum = 0.0f;
  int nonzero = 0;
  for (float sample : mono) {
    abs_sum += std::abs(sample);
    if (std::abs(sample) > 0.001f) {
      ++nonzero;
    }
  }
  REQUIRE(abs_sum > 0.5f);
  REQUIRE(nonzero >= 2);
}

TEST_CASE("AlignmentDelay Lagrange3 magnitude droop is documented by fixture values", "[mixing]") {
  for (double fractional_delay : {0.25, 0.5, 0.75}) {
    REQUIRE(lagrange3_magnitude_db(fractional_delay, 0.25) > -0.1);
  }

  REQUIRE_THAT(lagrange3_magnitude_db(0.25, 0.8), WithinAbs(-3.4543, 0.01));
  REQUIRE_THAT(lagrange3_magnitude_db(0.5, 0.8), WithinAbs(-6.9595, 0.01));
  REQUIRE_THAT(lagrange3_magnitude_db(0.75, 0.8), WithinAbs(-3.4543, 0.01));

  REQUIRE(lagrange3_magnitude_db(0.5, 1.0) < -200.0);
}

TEST_CASE("StereoWidthProcessor collapses to mono at zero width", "[mixing]") {
  std::array<float, 2> left{1.0f, 0.0f};
  std::array<float, 2> right{0.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};
  sonare::mixing::StereoWidthProcessor width(0.0f);

  width.process(channels, 2, 2);

  REQUIRE_THAT(left[0], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[1], WithinAbs(0.5f, 0.0001f));
}

TEST_CASE("FxBus owns ordered inserts and sums latency", "[mixing]") {
  sonare::mixing::FxBus fx_bus;

  fx_bus.add_insert(std::make_unique<TestLatencyProcessor>(3));
  fx_bus.add_insert(std::make_unique<TestLatencyProcessor>(5));

  REQUIRE(fx_bus.num_inserts() == 2);
  REQUIRE(fx_bus.latency_samples() == 8);
  REQUIRE(fx_bus.latency_samples_q8() == (8 << 8));
}

TEST_CASE("BusProcessor applies post-sum inserts", "[mixing]") {
  std::array<float, 4> a_l{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> a_r{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> b_l{2.0f, 2.0f, 2.0f, 2.0f};
  std::array<float, 4> b_r{2.0f, 2.0f, 2.0f, 2.0f};
  float* input_a[] = {a_l.data(), a_r.data()};
  float* input_b[] = {b_l.data(), b_r.data()};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};
  float* output[] = {out_l.data(), out_r.data()};

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.add_insert(std::make_unique<ScaleProcessor>(2.0f));
  bus.prepare(48000.0, 4);
  bus.sum_inputs({input_a, input_b}, output, 2, 4);
  bus.process(output, 2, 4);

  REQUIRE(bus.num_inserts() == 1);
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(out_l[static_cast<size_t>(i)], WithinAbs(6.0f, 0.0001f));
    REQUIRE_THAT(out_r[static_cast<size_t>(i)], WithinAbs(6.0f, 0.0001f));
  }
}

TEST_CASE("MeterProcessor publishes peak rms and correlation snapshot", "[mixing]") {
  std::array<float, 4> left{0.5f, -0.5f, 0.5f, -0.5f};
  std::array<float, 4> right{0.5f, -0.5f, 0.5f, -0.5f};
  float* channels[] = {left.data(), right.data()};
  sonare::mixing::MeterProcessor meter;

  meter.prepare(48000.0, 4);
  meter.process(channels, 2, 4);
  const auto snapshot = meter.snapshot();

  REQUIRE(snapshot.seq == 1);
  REQUIRE_THAT(snapshot.peak_db[0], WithinAbs(-6.0206f, 0.001f));
  REQUIRE_THAT(snapshot.rms_db[1], WithinAbs(-6.0206f, 0.001f));
  REQUIRE_THAT(snapshot.correlation, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(snapshot.gain_reduction_db, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(snapshot.max_true_peak_db, WithinAbs(sonare::constants::kFloorDb, 0.001f));
}

TEST_CASE("MeterProcessor optionally publishes true peak", "[mixing]") {
  std::array<float, 3> mono{0.0f, 0.8f, 0.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::MeterConfig config;
  config.measure_lufs = false;
  config.measure_true_peak = true;
  config.true_peak_oversample = 4;
  sonare::mixing::MeterProcessor meter(config);

  meter.prepare(48000.0, 3);
  meter.process(channels, 1, 3);
  const auto snapshot = meter.snapshot();

  REQUIRE(snapshot.max_true_peak_db >= snapshot.peak_db[0]);
  REQUIRE_THAT(snapshot.true_peak_db[0], WithinAbs(snapshot.max_true_peak_db, 0.0001f));
  REQUIRE_THAT(snapshot.true_peak_db[1], WithinAbs(sonare::constants::kFloorDb, 0.001f));
  REQUIRE(snapshot.max_true_peak_db > -3.0f);
}

TEST_CASE("MeterProcessor true peak is consistent across block sizes (boundary history)",
          "[mixing]") {
  // Regression for the stateless true-peak path: it reconstructed ~half-a-tap
  // worth of samples against zeros at the start AND end of every block and kept
  // no cross-block history, so the measured true peak depended on the block
  // size and phase. The history-preserving path must measure (nearly) the same
  // true peak whether the signal is fed in one block or in many small blocks,
  // and must still see the inter-sample over.
  constexpr int kSampleRate = 48000;
  constexpr int kN = 1020;  // multiple of the 6-sample pattern below
  // A repeating half-band pattern near full scale has a strong inter-sample
  // peak: its bandlimited reconstruction overshoots well above the sample
  // values (same family as the existing "bandlimited inter-sample overs" test).
  const std::array<float, 6> pattern{0.0f, 0.95f, 0.95f, 0.0f, -0.95f, -0.95f};
  std::vector<float> signal(static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    signal[static_cast<size_t>(i)] = pattern[static_cast<size_t>(i % 6)];
  }
  const float sample_peak = 0.95f;

  auto measure = [&](int block) {
    sonare::mixing::MeterConfig config;
    config.measure_lufs = false;
    config.measure_true_peak = true;
    config.true_peak_oversample = 4;
    sonare::mixing::MeterProcessor meter(config);
    meter.prepare(static_cast<double>(kSampleRate), block);

    float max_tp_db = sonare::constants::kFloorDb;
    for (int offset = 0; offset < kN; offset += block) {
      const int n = std::min(block, kN - offset);
      float* chan = signal.data() + offset;
      float* channels[] = {chan};
      meter.process(channels, 1, n);
      max_tp_db = std::max(max_tp_db, meter.snapshot().max_true_peak_db);
    }
    return max_tp_db;
  };

  const float tp_one_block = measure(kN);
  const float tp_small_blocks = measure(64);  // 15 full + a 60-sample tail block
  const float tp_tiny_blocks = measure(13);   // ragged blocks, many boundaries

  // The inter-sample over must be detected (true peak strictly above the sample
  // peak) regardless of block size.
  const float sample_peak_db = sonare::linear_to_db(sample_peak);
  REQUIRE(tp_one_block > sample_peak_db);
  REQUIRE(tp_small_blocks > sample_peak_db);
  REQUIRE(tp_tiny_blocks > sample_peak_db);

  // And the result must be block-size independent within a tight tolerance. The
  // old stateless path zero-padded the FIR at every block edge and kept no
  // history, so this measurement drifted with the block size; the cross-block
  // history path makes the small/tiny-block runs track the single-block
  // reference closely.
  REQUIRE_THAT(tp_small_blocks, WithinAbs(tp_one_block, 0.3f));
  REQUIRE_THAT(tp_tiny_blocks, WithinAbs(tp_one_block, 0.3f));
}

TEST_CASE("GoniometerBuffer returns latest scope points", "[mixing]") {
  sonare::mixing::GoniometerBuffer<3> buffer;
  std::array<sonare::mixing::GoniometerPoint, 3> points{};

  buffer.push(1.0f, 0.0f);
  buffer.push(0.0f, 1.0f);
  buffer.push(-1.0f, 0.0f);
  buffer.push(0.0f, -1.0f);
  const size_t count = buffer.read_latest(points.data(), points.size());

  REQUIRE(count == 3);
  REQUIRE_THAT(points[0].left, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(points[0].right, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(points[2].right, WithinAbs(-1.0f, 0.0001f));
}

TEST_CASE("ChannelStrip publishes post-fader goniometer points", "[mixing]") {
  std::array<float, 4> left{1.0f, 0.5f, -0.5f, -1.0f};
  std::array<float, 4> right{-1.0f, -0.5f, 0.5f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  std::array<sonare::mixing::GoniometerPoint, 4> points{};
  const size_t count = strip.read_goniometer_latest(points.data(), points.size());

  REQUIRE(count == 4);
  for (size_t i = 0; i < points.size(); ++i) {
    REQUIRE_THAT(points[i].left, WithinAbs(left[i], 0.0001f));
    REQUIRE_THAT(points[i].right, WithinAbs(right[i], 0.0001f));
  }
  REQUIRE(strip.meter_snapshot().max_true_peak_db > sonare::constants::kFloorDb);

  strip.reset();
  REQUIRE(strip.read_goniometer_latest(points.data(), points.size()) == 0);
}

TEST_CASE("StereoWidthProcessor converges to steady-state mid/side", "[mixing]") {
  constexpr int kN = 4096;
  constexpr float kL0 = 0.8f;
  constexpr float kR0 = 0.2f;
  const float mid0 = 0.5f * (kL0 + kR0);
  const float side0 = 0.5f * (kL0 - kR0);

  SECTION("width=0 collapses to mid after convergence") {
    std::vector<float> left(kN, kL0);
    std::vector<float> right(kN, kR0);
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(0.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    REQUIRE_THAT(left[kN - 1], WithinAbs(mid0, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(mid0, 0.001f));
  }

  SECTION("width=2 doubles the side and preserves the mid after convergence") {
    std::vector<float> left(kN, kL0);
    std::vector<float> right(kN, kR0);
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // Standard M/S width law: mid is left untouched, only the side scales with width.
    REQUIRE_THAT(left[kN - 1], WithinAbs(mid0 + 2.0f * side0, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(mid0 - 2.0f * side0, 0.001f));

    // The recovered mid (channel sum / 2) must equal the input mid: widening must
    // not attenuate the center/mono component.
    const float out_mid = 0.5f * (left[kN - 1] + right[kN - 1]);
    REQUIRE_THAT(out_mid, WithinAbs(mid0, 0.001f));
  }

  SECTION("width does not attenuate a centered/mono source") {
    constexpr float kCenter = 0.7f;
    std::vector<float> left(kN, kCenter);
    std::vector<float> right(kN, kCenter);  // mono: side == 0
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // With side == 0, raising width must leave a mono source completely unchanged.
    REQUIRE_THAT(left[kN - 1], WithinAbs(kCenter, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(kCenter, 0.001f));
  }

  SECTION("width=2 doubles a pure side signal") {
    std::vector<float> left(kN, 1.0f);
    std::vector<float> right(kN, -1.0f);  // mid == 0, side == 1
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // Pure side scales directly with width: |L| = |R| = side * w = 1 * 2 = 2.
    REQUIRE_THAT(std::abs(left[kN - 1]), WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(std::abs(right[kN - 1]), WithinAbs(2.0f, 0.001f));
  }
}

TEST_CASE("StereoWidthProcessor smooths width changes (no zipper)", "[mixing]") {
  constexpr int kN = 64;
  std::vector<float> left(kN, 1.0f);
  std::vector<float> right(kN, -1.0f);  // pure side signal
  float* channels[] = {left.data(), right.data()};

  // Default smoothing (5 ms) and starting width=1.
  sonare::mixing::StereoWidthProcessor width(1.0f);
  width.prepare(48000.0, kN);

  // Request a collapse to mono, then process a single small block.
  width.set_width(0.0f);
  width.process(channels, 2, kN);

  // The side must not collapse instantaneously: the first sample should still
  // retain a substantial fraction of the original side (|L - R| was 2.0).
  const float side_diff = std::abs(left[0] - right[0]);
  REQUIRE(side_diff > 0.1f);
  REQUIRE(side_diff < 2.0f);
}

TEST_CASE("MeterProcessor seqlock increments per block and stays consistent", "[mixing]") {
  constexpr int kN = 256;
  std::vector<float> left(kN);
  std::vector<float> right(kN);
  for (int i = 0; i < kN; ++i) {
    const float s =
        0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    left[i] = s;
    right[i] = s;
  }
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::MeterProcessor meter;
  meter.prepare(48000.0, kN);
  meter.process(channels, 2, kN);
  meter.process(channels, 2, kN);
  meter.process(channels, 2, kN);

  const auto snapshot = meter.snapshot();
  REQUIRE(snapshot.seq == 3);
  REQUIRE(std::isfinite(snapshot.peak_db[0]));
  REQUIRE(std::isfinite(snapshot.rms_db[0]));
  REQUIRE(snapshot.peak_db[0] > sonare::constants::kFloorDb);
  REQUIRE(snapshot.rms_db[0] > sonare::constants::kFloorDb);
}
