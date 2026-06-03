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
