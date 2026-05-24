#include "mixing/mixing.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <vector>

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
