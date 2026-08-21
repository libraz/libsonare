/// @file dynamics_compressor_limiter_test.cpp
/// @brief Compressor and limiter dynamics tests.

#include "dynamics_test_helpers.h"

TEST_CASE("Compressor reduces level above threshold", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
  compressor.prepare(48000.0, 1024);

  auto quiet = generate_sine_samples(1000.0f, 48000, 48000, 0.05f);
  auto loud = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(compressor, quiet);
  compressor.reset();
  process(compressor, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 0.95f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 0.55f);
  REQUIRE(compressor.last_gain_reduction_db() < -4.0f);
}

TEST_CASE("Compressor auto makeup increases compressed output", "[mastering][dynamics]") {
  auto input = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  auto no_makeup = input;
  auto with_makeup = input;

  Compressor dry({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
  Compressor makeup({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, true, DetectorMode::Rms});
  dry.prepare(48000.0, 1024);
  makeup.prepare(48000.0, 1024);

  process(dry, no_makeup);
  process(makeup, with_makeup);

  REQUIRE(rms_tail(with_makeup, 4096) > rms_tail(no_makeup, 4096) * 1.5f);
}

TEST_CASE("Compressor validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Compressor({-18.0f, 0.5f, 10.0f, 100.0f, 0.0f, 0.0f, false, DetectorMode::Rms}));
  REQUIRE_THROWS(Compressor({-18.0f, 2.0f, -1.0f, 100.0f, 0.0f, 0.0f, false, DetectorMode::Rms}));
}

TEST_CASE("Compressor LogRms detector compresses loud sustained tones", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 10.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::LogRms});
  compressor.prepare(48000.0, 1024);

  auto loud = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  const float before = rms_tail(loud, 4096);
  process(compressor, loud);

  REQUIRE(rms_tail(loud, 4096) / before < 0.55f);
  REQUIRE(compressor.last_gain_reduction_db() < -4.0f);
}

TEST_CASE("Compressor links detection across stereo channels", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 10.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::Peak});
  compressor.prepare(48000.0, 1024);

  auto left = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  auto right = generate_sine_samples(1000.0f, 48000, 48000, 0.02f);
  float* channels[] = {left.data(), right.data()};
  compressor.process(channels, 2, static_cast<int>(left.size()));

  // The quiet channel must be attenuated because the loud channel drove the
  // detector — this is the defining property of linked stereo detection.
  REQUIRE(rms_tail(right, 4096) < 0.018f);
}

TEST_CASE("Compressor set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  // Regression guard for the RtPublisher hand-off in Compressor::set_config:
  // a control thread races against an audio thread that is calling process()
  // back-to-back on small blocks. With the lock-free snapshot publisher the
  // audio thread must never observe a half-mutated configuration, must always
  // produce finite output, and the published configuration the control thread
  // last wrote must be visible to a subsequent block.
  Compressor compressor({-18.0f, 4.0f, 10.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::Peak});
  compressor.prepare(48000.0, 256);

  std::atomic<bool> stop{false};
  std::atomic<int> finite_blocks{0};
  std::atomic<int> nonfinite_blocks{0};

  std::thread audio_thread([&] {
    std::vector<float> block = generate_sine_samples(1000.0f, 48000, 256, 0.5f);
    float* channels[] = {block.data()};
    while (!stop.load(std::memory_order_acquire)) {
      // Refill the block each iteration so detector state has steady input;
      // the test verifies snapshot consistency, not detector behaviour.
      for (size_t i = 0; i < block.size(); ++i) {
        block[i] = 0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 1000.0 *
                                                      static_cast<double>(i) / 48000.0));
      }
      compressor.process(channels, 1, static_cast<int>(block.size()));
      bool all_finite = true;
      for (float s : block) {
        if (!std::isfinite(s)) {
          all_finite = false;
          break;
        }
      }
      (all_finite ? finite_blocks : nonfinite_blocks).fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Hammer set_config from the control thread with valid but varying parameter
  // sets. Each value is independently valid; together they exercise the full
  // recompute path (envelope follower, RMS, sidechain HPF, PDR).
  for (int i = 0; i < 2000; ++i) {
    CompressorConfig cfg{};
    cfg.threshold_db = -24.0f + static_cast<float>(i % 12);
    cfg.ratio = 2.0f + static_cast<float>(i % 5);
    cfg.attack_ms = 5.0f + static_cast<float>(i % 20);
    cfg.release_ms = 50.0f + static_cast<float>(i % 100);
    cfg.detector = (i & 1) ? DetectorMode::Rms : DetectorMode::Peak;
    cfg.sidechain_hpf_enabled = (i % 3) == 0;
    cfg.sidechain_hpf_hz = 80.0f + static_cast<float>(i % 200);
    compressor.set_config(cfg);
  }
  // On a heavily loaded CI runner the control loop above can finish before the
  // audio thread is scheduled even once; wait for at least one processed block
  // so the concurrency assertions below have something to observe.
  while (finite_blocks.load(std::memory_order_relaxed) +
             nonfinite_blocks.load(std::memory_order_relaxed) ==
         0) {
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  audio_thread.join();

  REQUIRE(finite_blocks.load() > 0);
  REQUIRE(nonfinite_blocks.load() == 0);

  // The audio thread must adopt the most recently published snapshot the next
  // time process() runs after the join. Re-publish a distinctive config then
  // drive one more block; round-trip via config().
  CompressorConfig final{};
  final.threshold_db = -12.0f;
  final.ratio = 3.0f;
  final.attack_ms = 7.0f;
  final.release_ms = 80.0f;
  compressor.set_config(final);
  std::vector<float> tail = generate_sine_samples(1000.0f, 48000, 256, 0.5f);
  float* tail_channels[] = {tail.data()};
  compressor.process(tail_channels, 1, static_cast<int>(tail.size()));
  REQUIRE(compressor.config().threshold_db == -12.0f);
  REQUIRE(compressor.config().ratio == 3.0f);
}

TEST_CASE("Compressor set_parameter exposes full insert parameter surface",
          "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 2.0f, 5.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
  compressor.prepare(48000.0, 256);

  REQUIRE(compressor.set_parameter(0, -30.0f));
  REQUIRE(compressor.set_parameter(1, 5.0f));
  REQUIRE(compressor.set_parameter(2, 1.0f));
  REQUIRE(compressor.set_parameter(3, 80.0f));
  REQUIRE(compressor.set_parameter(4, 2.0f));
  REQUIRE(compressor.set_parameter(5, 6.0f));
  REQUIRE(compressor.set_parameter(6, 1.0f));
  REQUIRE(compressor.set_parameter(7, 2.0f));
  REQUIRE(compressor.set_parameter(8, 1.0f));
  REQUIRE(compressor.set_parameter(9, 250.0f));
  REQUIRE(compressor.set_parameter(10, 30.0f));
  REQUIRE(compressor.set_parameter(11, 1.75f));
  REQUIRE_FALSE(compressor.set_parameter(12, 0.0f));

  std::vector<float> block = generate_sine_samples(1000.0f, 48000, 256, 0.8f);
  float* channels[] = {block.data()};
  compressor.process(channels, 1, static_cast<int>(block.size()));

  const auto& cfg = compressor.config();
  REQUIRE(cfg.threshold_db == -30.0f);
  REQUIRE(cfg.ratio == 5.0f);
  REQUIRE(cfg.attack_ms == 1.0f);
  REQUIRE(cfg.release_ms == 80.0f);
  REQUIRE(cfg.makeup_gain_db == 2.0f);
  REQUIRE(cfg.knee_db == 6.0f);
  REQUIRE(cfg.auto_makeup);
  REQUIRE(cfg.detector == DetectorMode::LogRms);
  REQUIRE(cfg.sidechain_hpf_enabled);
  REQUIRE(cfg.sidechain_hpf_hz == 250.0f);
  REQUIRE(cfg.pdr_time_ms == 30.0f);
  REQUIRE(cfg.pdr_release_scale == 1.75f);
  REQUIRE(compressor.last_gain_reduction_db() < 0.0f);
}

TEST_CASE("Compressor sidechain HPF ignores low-frequency detector energy",
          "[mastering][dynamics]") {
  auto input = generate_sine_samples(40.0f, 48000, 48000, 0.8f);
  auto full_band = input;
  auto hpf_keyed = input;

  Compressor full({-24.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Peak});
  Compressor hpf({-24.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Peak, true, 120.0f});
  full.prepare(48000.0, 1024);
  hpf.prepare(48000.0, 1024);

  process(full, full_band);
  process(hpf, hpf_keyed);

  REQUIRE(rms_tail(hpf_keyed, 4096) > rms_tail(full_band, 4096) * 1.5f);
}

TEST_CASE("Limiter delays audio and limits peaks", "[mastering][dynamics]") {
  Limiter limiter({-6.0f, 1.0f, 5.0f});
  limiter.prepare(1000.0, 128);

  std::vector<float> impulse(16, 0.0f);
  impulse[0] = 1.0f;

  process(limiter, impulse);

  REQUIRE(limiter.latency_samples() == 1);
  REQUIRE_THAT(impulse[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE(peak_abs(impulse, 1) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("Limiter passes signal below threshold after latency", "[mastering][dynamics]") {
  Limiter limiter({-1.0f, 1.0f, 0.0f});
  limiter.prepare(1000.0, 128);

  std::vector<float> signal = {0.25f, -0.25f, 0.2f, -0.2f};
  process(limiter, signal);

  REQUIRE_THAT(signal[1], WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(signal[2], WithinAbs(-0.25f, 0.0001f));
  REQUIRE_THAT(limiter.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("Limiter validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Limiter({-1.0f, -1.0f, 10.0f}));
  REQUIRE_THROWS(Limiter({-1.0f, 1.0f, -10.0f}));
}

TEST_CASE("BrickwallLimiter guarantees ceiling", "[mastering][dynamics]") {
  BrickwallLimiter limiter({-6.0f, 0.0f, 0.0f});
  limiter.prepare(48000.0, 128);

  std::vector<float> signal = {0.0f, 0.25f, -0.75f, 1.0f, -1.0f, 0.1f};
  process(limiter, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
  REQUIRE(limiter.latency_samples() == 0);
  REQUIRE(limiter.hard_clip_count() == 0);
}

TEST_CASE("BrickwallLimiter sanitizes non-finite output before the hard ceiling",
          "[mastering][dynamics]") {
  BrickwallLimiter limiter({-6.0f, 0.0f, 0.0f});
  limiter.prepare(48000.0, 128);

  std::vector<float> signal = {std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN(), 2.0f};
  process(limiter, signal);

  for (float sample : signal) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(std::abs(sample) <= 0.502f);
  }
  REQUIRE_THAT(signal[2], WithinAbs(0.0f, 0.0001f));
  REQUIRE(limiter.last_gain_reduction_db() <= -120.0f);
  REQUIRE(limiter.hard_clip_count() == 3);
}

TEST_CASE("BrickwallLimiter validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(BrickwallLimiter({-1.0f, -1.0f, 10.0f}));
  REQUIRE_THROWS(BrickwallLimiter({-1.0f, 1.0f, -10.0f}));
  REQUIRE_THROWS(BrickwallLimiter({std::numeric_limits<float>::infinity(), 1.0f, 10.0f}));
}

TEST_CASE("Compressor stays finite when the detector excludes its only channel",
          "[mastering][dynamics]") {
  // set_detector_excluded_channel is a public per-context host setting and
  // accepts channel 0 on a mono buffer, which leaves the linked detector with
  // no channels at all. The RMS detector divided the (zero) power sum by that
  // zero count, so power_lin was 0 * inf = NaN. That NaN reached the output
  // through a soft knee, and it also landed in rms_state_ -- state that
  // persists across blocks and that only reset() clears, so the detector never
  // recovered even after the exclusion was lifted.

  SECTION("a soft knee carries the detector NaN into the output") {
    // With knee_db > 0 the knee branch arithmetic propagates the NaN; a hard
    // knee happens to absorb it (every comparison against NaN is false), which
    // is why the finiteness of this configuration is the one worth pinning.
    Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 6.0f, 0.0f, false, DetectorMode::Rms});
    compressor.prepare(48000.0, 512);
    compressor.set_detector_excluded_channel(0);

    for (int block = 0; block < 4; ++block) {
      auto samples = generate_sine_samples(1000.0f, 512, 48000, 0.5f);
      float* channels[] = {samples.data()};
      compressor.process(channels, 1, 512);
      CAPTURE(block);
      for (float sample : samples) {
        REQUIRE(std::isfinite(sample));
      }
    }
  }

  SECTION("an empty detector degrades to unity gain, not to an arbitrary one") {
    Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
    compressor.prepare(48000.0, 512);
    compressor.set_detector_excluded_channel(0);

    auto reference = generate_sine_samples(1000.0f, 512, 48000, 0.5f);
    auto measured = generate_sine_samples(1000.0f, 512, 48000, 0.5f);
    float* channels[] = {measured.data()};
    compressor.process(channels, 1, 512);
    for (size_t i = 0; i < measured.size(); ++i) {
      CAPTURE(i);
      REQUIRE_THAT(measured[i], WithinAbs(reference[i], 1e-5f));
    }
  }

  SECTION("the detector recovers once the exclusion is lifted") {
    // The lasting damage: a single NaN in rms_state_ survives every later
    // block, so the level reads as NaN, the knee comparisons all fail, and the
    // compressor silently stops compressing until reset().
    Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
    compressor.prepare(48000.0, 512);
    compressor.set_detector_excluded_channel(0);

    auto excluded = generate_sine_samples(1000.0f, 512, 48000, 0.5f);
    float* excluded_channels[] = {excluded.data()};
    compressor.process(excluded_channels, 1, 512);

    compressor.set_detector_excluded_channel(-1);
    for (int block = 0; block < 4; ++block) {
      auto loud = generate_sine_samples(1000.0f, 512, 48000, 0.9f);
      float* channels[] = {loud.data()};
      compressor.process(channels, 1, 512);
      for (float sample : loud) {
        REQUIRE(std::isfinite(sample));
      }
    }
    // Well above the -18 dB threshold at a 4:1 ratio, so a working detector
    // must be reducing gain by now.
    REQUIRE(compressor.last_gain_reduction_db() < -1.0f);
  }

  SECTION("every detector mode takes the same divisor") {
    for (const DetectorMode detector :
         {DetectorMode::Peak, DetectorMode::Rms, DetectorMode::LogRms}) {
      Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 6.0f, 0.0f, false, detector});
      compressor.prepare(48000.0, 512);
      compressor.set_detector_excluded_channel(0);
      auto samples = generate_sine_samples(1000.0f, 512, 48000, 0.5f);
      float* channels[] = {samples.data()};
      compressor.process(channels, 1, 512);
      CAPTURE(static_cast<int>(detector));
      for (float sample : samples) {
        REQUIRE(std::isfinite(sample));
      }
    }
  }
}
