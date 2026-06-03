/// @file mixing_channel_strip_test.cpp
/// @brief Mixing channel strip tests.

#include "mixing_test_helpers.h"

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

TEST_CASE("BusProcessor publishes post-insert meter snapshot", "[mixing]") {
  std::array<float, 8> left{};
  std::array<float, 8> right{};
  left.fill(0.25f);
  right.fill(0.25f);
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.prepare(48000.0, static_cast<int>(left.size()));
  bus.process(channels, 2, static_cast<int>(left.size()));

  const auto snapshot = bus.meter_snapshot();
  REQUIRE(snapshot.seq == 1);
  REQUIRE_THAT(snapshot.correlation, WithinAbs(1.0f, 0.0001f));
  REQUIRE(snapshot.peak_db[0] > sonare::constants::kFloorDb);
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

TEST_CASE("GainProcessor zero smoothing applies target without a second ramp", "[mixing]") {
  std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {samples.data()};

  sonare::mixing::GainProcessor gain({0.0f, 0.0f});
  gain.prepare(48000.0, 4);
  gain.set_gain_db(-6.0206f);
  gain.process(channels, 1, 4);

  for (float sample : samples) {
    REQUIRE_THAT(sample, WithinAbs(0.5f, 0.0001f));
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

TEST_CASE("ChannelStrip input trim is independent from fader", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_input_trim_db(6.0206f);
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  REQUIRE_THAT(strip.input_trim_db(), WithinAbs(6.0206f, 0.0001f));
  REQUIRE_THAT(strip.fader_db(), WithinAbs(-6.0206f, 0.0001f));
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0002f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0002f));
  }
}

TEST_CASE("ChannelStrip applies polarity delay width and dual meter taps", "[mixing]") {
  std::array<float, 4> left{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> right{10.0f, 20.0f, 30.0f, 40.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_polarity_invert(true, false);
  strip.set_width(0.0f);
  strip.set_channel_delay_samples(1);
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  REQUIRE(strip.polarity_invert_left());
  REQUIRE_FALSE(strip.polarity_invert_right());
  REQUIRE(strip.channel_delay_samples() == 1);
  REQUIRE(strip.latency_samples() == 1);

  // After polarity and one-sample delay, the first nonzero stereo frame is (-1, 10).
  // Width 0 collapses the post-fader signal to mono, so both channels become 4.5.
  REQUIRE_THAT(left[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(4.5f, 0.0001f));
  REQUIRE_THAT(right[1], WithinAbs(4.5f, 0.0001f));

  const auto pre = strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  const auto post = strip.meter_snapshot(sonare::mixing::TapPoint::PostFader);
  REQUIRE(pre.seq == 1);
  REQUIRE(post.seq == 1);
  REQUIRE(pre.peak_db[1] > post.peak_db[1]);
}

TEST_CASE("ChannelStrip pre and post inserts wrap fader pan and width", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.add_post_insert(std::make_unique<ScaleProcessor>(3.0f));
  strip.prepare(48000.0, 4);
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  strip.process(channels, 2, 4);

  std::array<float, 4> send_l{};
  std::array<float, 4> send_r{};
  float* send[] = {send_l.data(), send_r.data()};
  strip.mix_send(pre_send, send, 2, 4);

  const float fader_gain = std::pow(10.0f, -6.0f / 20.0f);
  REQUIRE(strip.num_pre_inserts() == 1);
  REQUIRE(strip.num_post_inserts() == 1);
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(2.0f * fader_gain * 3.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(2.0f * fader_gain * 3.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip input trim starts the fixed strip order", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_input_trim_db(6.0206f);
  strip.add_pre_insert(std::make_unique<AddProcessor>(3.0f));
  strip.add_post_insert(std::make_unique<ScaleProcessor>(5.0f));
  strip.prepare(48000.0, 4);
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  strip.process(channels, 2, 4);

  std::array<float, 4> send_l{};
  std::array<float, 4> send_r{};
  float* send[] = {send_l.data(), send_r.data()};
  strip.mix_send(pre_send, send, 2, 4);

  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(5.0f, 0.0002f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(5.0f, 0.0002f));
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(12.5f, 0.001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(12.5f, 0.001f));
  }
  REQUIRE(strip.meter_snapshot(sonare::mixing::TapPoint::PreFader).seq == 1);
  REQUIRE(strip.meter_snapshot(sonare::mixing::TapPoint::PostFader).seq == 1);
}

TEST_CASE("ChannelStrip aggregates Q8 latency across delay and inserts", "[mixing]") {
  sonare::mixing::ChannelStrip strip;

  strip.set_channel_delay_samples(2);
  strip.add_pre_insert(std::make_unique<TestQ8LatencyProcessor>(3 << 8));
  strip.add_post_insert(std::make_unique<TestQ8LatencyProcessor>((5 << 8) + 128));

  REQUIRE(strip.latency_samples_q8() == ((10 << 8) + 128));
  REQUIRE(strip.latency_samples() == 10);
}

TEST_CASE("ChannelStrip applies fader automation at block sample offsets", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_fader_automation(102, -6.0206f));
  strip.process_at(channels, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
  }
  REQUIRE_THAT(strip.fader_db(), WithinAbs(-6.0206f, 0.0001f));
  REQUIRE(strip.meter_snapshot().seq == 1);
}

TEST_CASE("ChannelStrip segmented path reports block-max gain reduction", "[mixing]") {
  // With sample-accurate automation, process_at() splits the block into
  // segments. The pre-insert's last_gain_reduction_db() reflects only the most
  // recently processed segment, so the reported aggregate must track the
  // block-representative (most-negative) GR across all segments, not just the
  // final one. Here the loud leading segment (GR -10 dB) precedes a quiet
  // trailing segment (GR -1 dB); the snapshot must report -10 dB.
  std::array<float, 6> left{1.0f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<PeakGainReductionProcessor>());
  strip.prepare(48000.0, 6);
  // A width automation event at offset 2 forces a segment boundary there.
  REQUIRE(strip.schedule_width_automation(102, 1.0f));
  strip.process_at(channels, 2, 6, 100);

  // First segment peak 1.0 -> GR -10 dB; final segment peak 0.1 -> GR -1 dB.
  // The reported GR must be the block max (-10), not the last segment (-1).
  REQUIRE_THAT(strip.meter_snapshot().gain_reduction_db, WithinAbs(-10.0f, 0.0001f));
}

TEST_CASE("ChannelStrip reset clears pending automation lanes", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 4);
  REQUIRE(strip.schedule_fader_automation(100, -6.0206f));
  strip.reset();
  strip.process_at(channels, 2, 4, 100);

  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip applies pan and width automation in sample order", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_width_automation(102, 0.0f));
  REQUIRE(strip.schedule_pan_automation(104, 1.0f));
  strip.process_at(channels, 2, 6, 100);

  REQUIRE_THAT(left[0], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(left[2], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[2], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(left[4], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(right[4], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(strip.width(), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(strip.pan(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("ChannelStrip drives insert parameter automation at sample offsets", "[mixing]") {
  // schedule_insert_automation must dispatch to the insert's set_parameter at the
  // scheduled sample. ScaleProcessor::set_parameter(0, v) sets its linear gain, so
  // a single step event flips the gain mid-block at a known boundary.
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_insert_automation(0, 0, 102, 4.0f));
  strip.process_at(channels, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip keeps independent insert automation lanes sample-ordered", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(3.0f));
  strip.prepare(48000.0, 4);

  REQUIRE(strip.schedule_insert_automation(1, 0, 101, 5.0f));
  REQUIRE(strip.schedule_insert_automation(0, 0, 102, 4.0f));
  strip.process_at(channels, 2, 4, 100);

  REQUIRE_THAT(left[0], WithinAbs(6.0f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(10.0f, 0.0001f));
  REQUIRE_THAT(left[2], WithinAbs(20.0f, 0.0001f));
  REQUIRE_THAT(left[3], WithinAbs(20.0f, 0.0001f));
}

TEST_CASE("ChannelStrip insert automation boosts a parametric EQ band", "[mixing][eq]") {
  // Proves set_parameter reaches a real mastering insert (ParametricEq): band 0
  // gain lives at param id 1 (block-of-3 layout). Automating it from 0 dB to
  // +12 dB must lift the band's output energy, which a no-op set_parameter could
  // not do.
  static constexpr int kN = 4096;
  auto make_sine = [] {
    std::vector<float> out(kN);
    for (int i = 0; i < kN; ++i) {
      out[static_cast<size_t>(i)] =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    }
    return out;
  };

  auto make_band0_eq = [] {
    auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 0.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;
    eq->set_band(0, band);
    return eq;
  };

  std::vector<float> flat_l = make_sine();
  std::vector<float> flat_r = flat_l;
  float* flat[] = {flat_l.data(), flat_r.data()};
  sonare::mixing::ChannelStrip flat_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  flat_strip.add_pre_insert(make_band0_eq());
  flat_strip.prepare(48000.0, kN);
  flat_strip.process(flat, 2, kN);

  std::vector<float> auto_l = make_sine();
  std::vector<float> auto_r = auto_l;
  float* automated[] = {auto_l.data(), auto_r.data()};
  sonare::mixing::ChannelStrip auto_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto_strip.add_pre_insert(make_band0_eq());
  auto_strip.prepare(48000.0, kN);
  REQUIRE(auto_strip.schedule_insert_automation(0, 1, 0, 12.0f));
  auto_strip.process_at(automated, 2, kN, 0);

  REQUIRE(rms_tail(auto_l, 512) > rms_tail(flat_l, 512) * 1.5f);
}

TEST_CASE("ChannelStrip rejects non-RT-safe insert automation", "[mixing]") {
  sonare::mixing::ChannelStrip linear_phase_strip;
  linear_phase_strip.add_pre_insert(std::make_unique<sonare::mastering::eq::LinearPhaseEq>());
  linear_phase_strip.prepare(48000.0, 128);

  REQUIRE_FALSE(linear_phase_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  auto equalizer = std::make_unique<sonare::mastering::eq::EqualizerProcessor>();
  sonare::mastering::eq::EqBand linear_band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f,
                                            1.0f, true};
  linear_band.phase = sonare::mastering::eq::PhaseMode::LinearPhase;
  equalizer->set_band(0, linear_band);
  sonare::mixing::ChannelStrip equalizer_strip;
  equalizer_strip.add_pre_insert(std::move(equalizer));
  equalizer_strip.prepare(48000.0, 128);

  REQUIRE_FALSE(equalizer_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  sonare::mixing::ChannelStrip maximizer_strip;
  maximizer_strip.add_pre_insert(std::make_unique<sonare::mastering::maximizer::Maximizer>());
  maximizer_strip.prepare(48000.0, 128);

  REQUIRE(maximizer_strip.schedule_insert_automation(0, 0, 0, 3.0f));
  REQUIRE_FALSE(maximizer_strip.schedule_insert_automation(0, 1, 0, -2.0f));
  REQUIRE(maximizer_strip.schedule_insert_automation(0, 2, 0, 20.0f));
}

TEST_CASE("ChannelStrip reuses mastering EQ as a pre-fader insert", "[mixing]") {
  static constexpr int kN = 512;
  auto make_input = [] {
    std::vector<float> out(kN);
    for (int i = 0; i < kN; ++i) {
      out[static_cast<size_t>(i)] =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    }
    return out;
  };

  std::vector<float> plain_l = make_input();
  std::vector<float> plain_r = plain_l;
  float* plain[] = {plain_l.data(), plain_r.data()};
  sonare::mixing::ChannelStrip plain_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  plain_strip.prepare(48000.0, kN);
  plain_strip.process(plain, 2, kN);

  auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::Peak;
  band.frequency_hz = 1000.0f;
  band.gain_db = 12.0f;
  band.q = sonare::constants::kButterworthQ;
  band.enabled = true;
  eq->set_band(0, band);

  std::vector<float> eq_l = make_input();
  std::vector<float> eq_r = eq_l;
  float* eq_channels[] = {eq_l.data(), eq_r.data()};
  sonare::mixing::ChannelStrip eq_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  eq_strip.add_pre_insert(std::move(eq));
  eq_strip.prepare(48000.0, kN);
  eq_strip.process(eq_channels, 2, kN);

  REQUIRE(rms_tail(eq_l, 128) > rms_tail(plain_l, 128) * 1.5f);
}

TEST_CASE("ChannelStrip reuses mastering compressor as a post-fader insert", "[mixing]") {
  constexpr int kN = 48000;
  std::vector<float> left(kN, 0.8f);
  std::vector<float> right(kN, 0.8f);
  float* channels[] = {left.data(), right.data()};

  sonare::mastering::dynamics::CompressorConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 8.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 20.0f;
  config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  auto compressor = std::make_unique<sonare::mastering::dynamics::Compressor>(config);
  auto* compressor_ptr = compressor.get();

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_post_insert(std::move(compressor));
  strip.prepare(48000.0, 1024);
  strip.process(channels, 2, kN);

  REQUIRE(rms_tail(left, 4096) < 0.25f);
  REQUIRE(compressor_ptr->last_gain_reduction_db() < -10.0f);
  REQUIRE(strip.meter_snapshot().gain_reduction_db < -10.0f);
}

TEST_CASE("ChannelStrip accepts mastering SidechainRouter insert with external key", "[mixing]") {
  constexpr int kN = 48000;
  std::vector<float> left(kN, 0.01f);
  std::vector<float> right(kN, 0.01f);
  float* channels[] = {left.data(), right.data()};

  sonare::mastering::dynamics::SidechainRouterConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 4.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 20.0f;
  config.range_db = 18.0f;
  auto router = std::make_unique<sonare::mastering::dynamics::SidechainRouter>(config);
  auto* router_ptr = router.get();

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_post_insert(std::move(router));
  strip.prepare(48000.0, 1024);

  std::vector<float> sidechain(kN, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};
  router_ptr->set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  strip.process(channels, 2, kN);

  REQUIRE(rms_tail(left, 4096) < 0.0025f);
  REQUIRE(router_ptr->last_gain_reduction_db() < -10.0f);
}
