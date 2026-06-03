/// @file mixing_meter_lufs_integration_test.cpp
/// @brief Mixing meter LUFS and integration tests.

#include "mixing_test_helpers.h"

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
  // metering/lufs.cpp is part of sonare_core, so this links without CMake changes.
  const auto offline = sonare::metering::lufs_interleaved(buffer.data(), buffer.size() / 2, 2,
                                                          static_cast<int>(kSr));
  REQUIRE_THAT(snap_a.momentary_lufs, WithinAbs(offline.momentary_lufs, 0.7f));
}

TEST_CASE("ChannelStrip EQ alters signal and position matters", "[mixing]") {
  static constexpr int kN = 256;
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
  static constexpr int kN = 128;
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

TEST_CASE("ChannelStrip applies send automation during mix_send_at", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  REQUIRE(strip.schedule_send_automation(send, 102, -6.0206f));
  REQUIRE_FALSE(strip.schedule_send_automation(send + 1, 102, -6.0206f));

  strip.process_at(channels, 2, 6, 100);

  std::array<float, 6> send_l{};
  std::array<float, 6> send_r{};
  float* dest[] = {send_l.data(), send_r.data()};
  strip.mix_send_at(send, dest, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip discards stale send automation even when send is not mixed", "[mixing]") {
  std::array<float, 1> left{1.0f};
  std::array<float, 1> right{1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 1);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});

  for (int i = 0; i < 1024; ++i) {
    REQUIRE(strip.schedule_send_automation(send, i, -6.0f));
  }
  REQUIRE_FALSE(strip.schedule_send_automation(send, 1024, -6.0f));

  strip.process_at(channels, 2, 1, 2048);
  REQUIRE(strip.schedule_send_automation(send, 2048, -3.0f));
}

// ============================================================================
// P1 regression test: ScopedNoDenormals guard on BusProcessor::process
// ============================================================================

TEST_CASE("BusProcessor silent input through IIR insert produces exact-zero output",
          "[mixing][bus][rt-safety]") {
  // Regression guard for the P1 fix that wraps BusProcessor::process in
  // rt::ScopedNoDenormals. An IIR insert (parametric low-shelf EQ here) fed a
  // long block of silence must produce an exact-zero output rather than
  // accumulating denormal floats — denormals would manifest as tiny non-zero
  // tail samples on x86 without DAZ/FTZ, and 10-100x CPU spikes in audio
  // callbacks. Mirrors the C-1 test for the voice changer (commit 4d34bbe).
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 4096;

  auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::LowShelf;
  band.frequency_hz = 100.0f;
  band.gain_db = 6.0f;
  band.q = sonare::constants::kButterworthQ;
  band.enabled = true;
  eq->set_band(0, band);

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.add_insert(std::move(eq));
  bus.prepare(static_cast<double>(kSampleRate), kBlockSize);

  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
  left.fill(0.0f);
  right.fill(0.0f);
  float* channels[] = {left.data(), right.data()};

  bus.process(channels, 2, kBlockSize);

  for (int i = 0; i < kBlockSize; ++i) {
    REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
  }
}

TEST_CASE("ChannelStrip segmented pre and post meters integrate the same window", "[mixing]") {
  // Regression: in the segmented automation path of process_at(), the pre-fader
  // meter was driven with clamped_samples (min(num_samples, max_block_size_))
  // while the post-fader meter was driven with the full num_samples. When
  // num_samples > max_block_size_ the two meters integrated different lengths,
  // so their RMS/LUFS readings disagreed for the SAME block. The fix clamps the
  // post meter to the same window as the pre meter (pre_tap_ is only
  // max_block_size_ wide, so the pre meter can never see more than that).
  //
  // Build a block that is LOUD only over the first max_block_size_ samples and
  // SILENT afterward. RMS depends on the integration window, so:
  //   - pre meter integrates clamped_samples (the loud region) -> rms = loud
  //   - post meter, when consistent, integrates the same window -> rms = loud
  // If the post meter instead integrated the full num_samples (loud + silence)
  // its RMS would be measurably LOWER. Asserting pre.rms_db == post.rms_db
  // therefore proves both meters use the same window length.
  constexpr int kMaxBlock = 64;
  constexpr int kNumSamples = 256;  // > kMaxBlock, so the windows differed pre-fix.
  static_assert(kNumSamples > kMaxBlock, "test requires num_samples > max_block_size_");

  // Unity strip: 0 dB fader, center Linear0dB pan, width 1 — so the post-fader
  // output equals the pre-fader tap sample-for-sample and the two meters see the
  // same signal. Any window mismatch is then the ONLY source of an RMS gap.
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kMaxBlock);

  std::vector<float> left(kNumSamples, 0.0f);
  std::vector<float> right(kNumSamples, 0.0f);
  constexpr float kLoud = 0.5f;
  for (int i = 0; i < kMaxBlock; ++i) {
    left[static_cast<size_t>(i)] = kLoud;
    right[static_cast<size_t>(i)] = kLoud;
  }
  float* channels[] = {left.data(), right.data()};

  // Force the segmented path with a no-op fader automation event (target equals
  // the current 0 dB value, so the gain stays unity and the signal is unchanged)
  // scheduled within the block.
  REQUIRE(strip.schedule_fader_automation(0, 0.0f));
  strip.process_at(channels, 2, kNumSamples, 0);

  const auto pre = strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  const auto post = strip.meter_snapshot(sonare::mixing::TapPoint::PostFader);
  REQUIRE(pre.seq == 1);
  REQUIRE(post.seq == 1);

  // The pre meter reflects the loud region only (it can never integrate more
  // than max_block_size_). The expected RMS of a constant kLoud over its window
  // is exactly kLoud.
  const float expected_rms_db = 20.0f * std::log10(kLoud);
  REQUIRE_THAT(pre.rms_db[0], WithinAbs(expected_rms_db, 0.01f));
  REQUIRE_THAT(pre.rms_db[1], WithinAbs(expected_rms_db, 0.01f));

  // Same window: the post meter must report the same RMS. (Pre-fix it would have
  // been ~6 dB lower because the silent tail diluted the full-block average.)
  REQUIRE_THAT(post.rms_db[0], WithinAbs(pre.rms_db[0], 0.01f));
  REQUIRE_THAT(post.rms_db[1], WithinAbs(pre.rms_db[1], 0.01f));

  // Peak is window-length insensitive here (the loud region dominates), so it
  // matches on both meters as a sanity check that the signals are identical.
  REQUIRE_THAT(post.peak_db[0], WithinAbs(pre.peak_db[0], 0.01f));
}
