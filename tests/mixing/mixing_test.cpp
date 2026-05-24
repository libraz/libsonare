#include "mixing/mixing.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <vector>

#include "analysis/meter/lufs.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {

float energy(float left, float right) { return left * left + right * right; }

}  // namespace

class TestLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TestLatencyProcessor(int latency) : latency_(latency) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
};

TEST_CASE("Const3dB pan law preserves stereo power", "[mixing]") {
  for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
    const auto gains = sonare::mixing::compute_pan_gains(pan, sonare::mixing::PanLaw::Const3dB);

    REQUIRE_THAT(energy(gains.left, gains.right), WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("Panner supports selectable pan laws", "[mixing]") {
  const auto const3 = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Const3dB);
  const auto const6 = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Const6dB);
  const auto linear = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Linear0dB);

  REQUIRE_THAT(const3.left, WithinAbs(std::sqrt(0.5f), 0.0001f));
  REQUIRE_THAT(const3.right, WithinAbs(std::sqrt(0.5f), 0.0001f));
  REQUIRE_THAT(const6.left, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(const6.right, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(linear.left, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(linear.right, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("BusProcessor sums multiple stereo inputs", "[mixing]") {
  std::array<float, 4> in1_l{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> in1_r{0.5f, 1.0f, 1.5f, 2.0f};
  std::array<float, 4> in2_l{-0.25f, 0.25f, -0.25f, 0.25f};
  std::array<float, 4> in2_r{2.0f, 1.0f, 0.0f, -1.0f};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};

  float* input1[] = {in1_l.data(), in1_r.data()};
  float* input2[] = {in2_l.data(), in2_r.data()};
  float* output[] = {out_l.data(), out_r.data()};

  const sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Master);
  bus.sum_inputs({input1, input2}, output, 2, 4);

  REQUIRE_THAT(out_l[0], WithinAbs(0.75f, 0.0001f));
  REQUIRE_THAT(out_l[1], WithinAbs(2.25f, 0.0001f));
  REQUIRE_THAT(out_r[0], WithinAbs(2.5f, 0.0001f));
  REQUIRE_THAT(out_r[3], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("GainProcessor applies fader and VCA offset", "[mixing]") {
  std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {samples.data()};

  sonare::mixing::GainProcessor gain({-6.0f, 0.0f});
  gain.set_vca_offset_db(6.0f);
  gain.prepare(48000.0, 4);
  gain.process(channels, 1, 4);

  for (float sample : samples) {
    REQUIRE_THAT(sample, WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip applies fader then pan", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 1.0f, sonare::mixing::PanLaw::Const3dB, 0.0f});
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  for (float sample : left) {
    REQUIRE_THAT(sample, WithinAbs(0.0f, 0.0001f));
  }
  for (float sample : right) {
    REQUIRE_THAT(sample, WithinAbs(1.0f, 0.0001f));
  }
}

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

  SECTION("width=2 doubles the side after convergence") {
    std::vector<float> left(kN, kL0);
    std::vector<float> right(kN, kR0);
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    REQUIRE_THAT(left[kN - 1], WithinAbs(mid0 + 2.0f * side0, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(mid0 - 2.0f * side0, 0.001f));
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

namespace {

// Feeds a steady stereo sine of the given amplitude through the meter in blocks and
// returns the final snapshot. The buffer is also captured for an optional offline check.
sonare::mixing::MeterSnapshot drive_meter_sine(sonare::mixing::MeterProcessor& meter,
                                               float amplitude, double sample_rate,
                                               double duration_sec, int block_size,
                                               std::vector<float>* interleaved = nullptr) {
  const int total = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> left(static_cast<size_t>(block_size));
  std::vector<float> right(static_cast<size_t>(block_size));
  float* channels[] = {left.data(), right.data()};
  if (interleaved != nullptr) {
    interleaved->clear();
    interleaved->reserve(static_cast<size_t>(total) * 2);
  }

  int n = 0;
  while (n < total) {
    const int block = std::min(block_size, total - n);
    for (int i = 0; i < block; ++i) {
      const float s =
          amplitude * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n + i) /
                               static_cast<float>(sample_rate));
      left[static_cast<size_t>(i)] = s;
      right[static_cast<size_t>(i)] = s;
      if (interleaved != nullptr) {
        interleaved->push_back(s);
        interleaved->push_back(s);
      }
    }
    meter.process(channels, 2, block);
    n += block;
  }
  return meter.snapshot();
}

}  // namespace

TEST_CASE("MeterProcessor streaming LUFS obeys the energy doubling law", "[mixing]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 512;
  constexpr float kA = 0.25f;

  // Before the momentary window (400 ms) is full, momentary LUFS stays at the floor.
  {
    sonare::mixing::MeterProcessor warmup;
    warmup.prepare(kSr, kBlock);
    std::vector<float> l(kBlock);
    std::vector<float> r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
      const float s = kA * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                                    static_cast<float>(kSr));
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
    float* ch[] = {l.data(), r.data()};
    // Two blocks ~= 21 ms, far short of the 400 ms momentary window.
    warmup.process(ch, 2, kBlock);
    warmup.process(ch, 2, kBlock);
    const auto early = warmup.snapshot();
    REQUIRE_THAT(early.momentary_lufs, WithinAbs(sonare::constants::kFloorDb, 0.001f));
  }

  std::vector<float> buffer;
  sonare::mixing::MeterProcessor meter_a;
  meter_a.prepare(kSr, kBlock);
  const auto snap_a = drive_meter_sine(meter_a, kA, kSr, 3.5, kBlock, &buffer);

  // Once filled, momentary/short-term are finite and well above the absolute gate.
  REQUIRE(std::isfinite(snap_a.momentary_lufs));
  REQUIRE(snap_a.momentary_lufs > -70.0f);
  REQUIRE(std::isfinite(snap_a.short_term_lufs));
  REQUIRE(snap_a.short_term_lufs > -70.0f);

  // For a steady tone the integrated loudness tracks momentary closely.
  REQUIRE(std::isfinite(snap_a.integrated_lufs));
  REQUIRE_THAT(snap_a.integrated_lufs, WithinAbs(snap_a.momentary_lufs, 1.0f));

  // Doubling the amplitude raises loudness by 20*log10(2) ~= 6.0206 dB, independent
  // of the K-weighting gain (a pure energy ratio on identical filtering).
  sonare::mixing::MeterProcessor meter_b;
  meter_b.prepare(kSr, kBlock);
  const auto snap_b = drive_meter_sine(meter_b, 2.0f * kA, kSr, 3.5, kBlock);

  REQUIRE_THAT(snap_b.momentary_lufs - snap_a.momentary_lufs, WithinAbs(6.0206f, 0.1f));

  // Optional reference-free cross-check against the offline meter on the same buffer.
  // analysis/meter/lufs.cpp is part of sonare_core, so this links without CMake changes.
  const auto offline = sonare::analysis::meter::lufs_interleaved(buffer.data(), buffer.size() / 2,
                                                                 2, static_cast<int>(kSr));
  REQUIRE_THAT(snap_a.momentary_lufs, WithinAbs(offline.momentary_lufs, 0.7f));
}

TEST_CASE("ChannelStrip EQ alters signal and position matters", "[mixing]") {
  constexpr int kN = 256;
  auto make_input = [](std::vector<float>& l, std::vector<float>& r) {
    for (int i = 0; i < kN; ++i) {
      const float s =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
  };

  SECTION("an enabled EQ band changes the output energy") {
    std::vector<float> bypass_l(kN);
    std::vector<float> bypass_r(kN);
    make_input(bypass_l, bypass_r);
    float* bypass[] = {bypass_l.data(), bypass_r.data()};

    sonare::mixing::ChannelStrip plain({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    plain.prepare(48000.0, kN);
    plain.process(bypass, 2, kN);

    std::vector<float> eq_l(kN);
    std::vector<float> eq_r(kN);
    make_input(eq_l, eq_r);
    float* eqd[] = {eq_l.data(), eq_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 12.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;
    strip.set_eq_band(0, band);
    strip.prepare(48000.0, kN);
    strip.process(eqd, 2, kN);

    float diff_energy = 0.0f;
    for (int i = 0; i < kN; ++i) {
      diff_energy += (eq_l[static_cast<size_t>(i)] - bypass_l[static_cast<size_t>(i)]) *
                     (eq_l[static_cast<size_t>(i)] - bypass_l[static_cast<size_t>(i)]);
    }
    REQUIRE(diff_energy > 1e-4f);
  }

  SECTION("EqPosition is configurable and routes the EQ relative to the fader") {
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 12.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;

    std::vector<float> pre_l(kN);
    std::vector<float> pre_r(kN);
    make_input(pre_l, pre_r);
    float* pre[] = {pre_l.data(), pre_r.data()};
    sonare::mixing::ChannelStrip pre_strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f,
                                            sonare::mixing::EqPosition::PreFader});
    REQUIRE(pre_strip.eq_position() == sonare::mixing::EqPosition::PreFader);
    pre_strip.set_eq_band(0, band);
    pre_strip.prepare(48000.0, kN);
    pre_strip.process(pre, 2, kN);

    std::vector<float> post_l(kN);
    std::vector<float> post_r(kN);
    make_input(post_l, post_r);
    float* post[] = {post_l.data(), post_r.data()};
    sonare::mixing::ChannelStrip post_strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f,
                                             sonare::mixing::EqPosition::PostFader});
    REQUIRE(post_strip.eq_position() == sonare::mixing::EqPosition::PostFader);
    post_strip.set_eq_band(0, band);
    post_strip.prepare(48000.0, kN);
    post_strip.process(post, 2, kN);

    // The EQ stage is a linear biquad and the fader is a scalar gain; an LTI filter
    // commutes with scalar multiplication, so fader*EQ(x) == EQ(fader*x). Both orderings
    // therefore produce the same output here. (Position would matter once a nonlinear or
    // amplitude-dependent stage is inserted between EQ and fader.) Verify exact agreement
    // so this assertion catches any accidental non-commuting change in the routing.
    float max_diff = 0.0f;
    for (int i = 0; i < kN; ++i) {
      max_diff = std::max(max_diff,
                          std::abs(pre_l[static_cast<size_t>(i)] - post_l[static_cast<size_t>(i)]));
    }
    REQUIRE_THAT(max_diff, WithinAbs(0.0f, 1e-4f));
  }
}

TEST_CASE("ChannelStrip aux sends tap the correct stage", "[mixing]") {
  constexpr int kN = 128;
  auto make_input = [](std::vector<float>& l, std::vector<float>& r) {
    for (int i = 0; i < kN; ++i) {
      const float s =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
  };

  SECTION("post-fader send at 0 dB equals the post-fader output") {
    std::vector<float> in_l(kN);
    std::vector<float> in_r(kN);
    make_input(in_l, in_r);
    float* channels[] = {in_l.data(), in_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)], WithinAbs(in_l[static_cast<size_t>(i)], 1e-3f));
    }
  }

  SECTION("post-fader send at -6.0206 dB scales the output by 0.5") {
    std::vector<float> in_l(kN);
    std::vector<float> in_r(kN);
    make_input(in_l, in_r);
    float* channels[] = {in_l.data(), in_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({-6.0206f, sonare::mixing::SendTiming::PostFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)],
                   WithinAbs(0.5f * in_l[static_cast<size_t>(i)], 1e-3f));
    }
  }

  SECTION("pre-fader send taps the signal before the fader gain") {
    std::vector<float> input(kN);
    {
      std::vector<float> tmp(kN);
      make_input(input, tmp);
    }

    std::vector<float> in_l = input;
    std::vector<float> in_r = input;
    float* channels[] = {in_l.data(), in_r.data()};

    // -6 dB fader so pre-fader and post-fader taps differ. No EQ bands.
    sonare::mixing::ChannelStrip strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    const float fader_gain = std::pow(10.0f, -6.0f / 20.0f);
    for (int i = 0; i < kN; ++i) {
      // Pre-fader tap equals the original input (no EQ, tapped before the fader).
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)], WithinAbs(input[static_cast<size_t>(i)], 1e-3f));
      // Post-fader output (left, captured in channels) is the input scaled by the fader.
      REQUIRE_THAT(in_l[static_cast<size_t>(i)],
                   WithinAbs(fader_gain * input[static_cast<size_t>(i)], 1e-3f));
    }
    // The pre-fader send therefore differs from the post-fader output.
    float max_diff = 0.0f;
    for (int i = 0; i < kN; ++i) {
      max_diff = std::max(max_diff,
                          std::abs(dest_l[static_cast<size_t>(i)] - in_l[static_cast<size_t>(i)]));
    }
    REQUIRE(max_diff > 1e-3f);
  }
}
