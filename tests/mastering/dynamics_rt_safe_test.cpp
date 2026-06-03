/// @file dynamics_rt_safe_test.cpp
/// @brief RT-safe set_config dynamics tests.

#include "dynamics_test_helpers.h"

TEST_CASE("Gate set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  Gate gate({});
  gate.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        GateConfig cfg{};
        cfg.threshold_db = -50.0f + static_cast<float>(i % 20);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 10);
        cfg.release_ms = 50.0f + static_cast<float>(i % 100);
        cfg.range_db = -80.0f;
        cfg.hold_ms = static_cast<float>(i % 5);
        cfg.close_threshold_db = -52.0f + static_cast<float>(i % 20);
        cfg.key_hpf_hz = static_cast<float>(i % 200);
        gate.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        gate.process(channels, 1, 256);
        return all_finite(block);
      });
  GateConfig final{};
  final.threshold_db = -33.0f;
  gate.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  gate.process(channels, 1, 256);
  REQUIRE(gate.config().threshold_db == -33.0f);
}

TEST_CASE("Limiter set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  Limiter limiter({});
  limiter.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  // lookahead_ms is held constant because changing it would resize buffers.
  run_rt_safe_race(
      [&](int i) {
        LimiterConfig cfg{};
        cfg.threshold_db = -6.0f + static_cast<float>(i % 5);
        cfg.lookahead_ms = 1.0f;
        cfg.release_ms = 20.0f + static_cast<float>(i % 100);
        limiter.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        limiter.process(channels, 1, 256);
        return all_finite(block);
      });
  LimiterConfig final{};
  final.threshold_db = -3.0f;
  final.lookahead_ms = 1.0f;
  final.release_ms = 25.0f;
  limiter.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  limiter.process(channels, 1, 256);
  REQUIRE(limiter.config().threshold_db == -3.0f);
}

TEST_CASE("Expander set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  Expander expander({});
  expander.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        ExpanderConfig cfg{};
        cfg.threshold_db = -40.0f + static_cast<float>(i % 20);
        cfg.ratio = 1.5f + static_cast<float>(i % 4);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 10);
        cfg.release_ms = 50.0f + static_cast<float>(i % 100);
        cfg.range_db = -60.0f;
        expander.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        expander.process(channels, 1, 256);
        return all_finite(block);
      });
  ExpanderConfig final{};
  final.threshold_db = -30.0f;
  final.ratio = 2.5f;
  expander.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  expander.process(channels, 1, 256);
  REQUIRE(expander.config().threshold_db == -30.0f);
  REQUIRE(expander.config().ratio == 2.5f);
}

TEST_CASE("DeEsser set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  DeEsser deesser({});
  deesser.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        DeEsserConfig cfg{};
        cfg.frequency_hz = 4000.0f + static_cast<float>(i % 4000);
        cfg.threshold_db = -30.0f + static_cast<float>(i % 15);
        cfg.ratio = 2.0f + static_cast<float>(i % 8);
        cfg.attack_ms = 0.5f + static_cast<float>(i % 5);
        cfg.release_ms = 30.0f + static_cast<float>(i % 100);
        cfg.range_db = 6.0f + static_cast<float>(i % 12);
        cfg.bandpass_q = 0.5f + static_cast<float>(i % 4);
        deesser.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 6000.0f, 48000);
        deesser.process(channels, 1, 256);
        return all_finite(block);
      });
  DeEsserConfig final{};
  final.frequency_hz = 7500.0f;
  deesser.set_config(final);
  fill_sine_block(block, 6000.0f, 48000);
  deesser.process(channels, 1, 256);
  REQUIRE(deesser.config().frequency_hz == 7500.0f);
}

TEST_CASE("ParallelComp set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  ParallelComp pc({});
  pc.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        ParallelCompConfig cfg{};
        cfg.threshold_db = -24.0f + static_cast<float>(i % 12);
        cfg.ratio = 2.0f + static_cast<float>(i % 6);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 20);
        cfg.release_ms = 50.0f + static_cast<float>(i % 100);
        cfg.makeup_gain_db = -3.0f + static_cast<float>(i % 6);
        cfg.mix = 0.1f + 0.05f * static_cast<float>(i % 10);
        cfg.linked_detection = (i & 1) != 0;
        cfg.output_limiter = (i & 2) != 0;
        cfg.output_ceiling_db = -1.0f;
        pc.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        pc.process(channels, 1, 256);
        return all_finite(block);
      });
  ParallelCompConfig final{};
  final.mix = 0.7f;
  pc.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  pc.process(channels, 1, 256);
  REQUIRE(pc.config().mix == 0.7f);
}

TEST_CASE("VocalRider set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  VocalRider rider({});
  rider.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        VocalRiderConfig cfg{};
        cfg.target_db = -24.0f + static_cast<float>(i % 12);
        cfg.max_boost_db = 3.0f + static_cast<float>(i % 6);
        cfg.max_cut_db = 3.0f + static_cast<float>(i % 6);
        cfg.attack_ms = 20.0f + static_cast<float>(i % 60);
        cfg.release_ms = 200.0f + static_cast<float>(i % 500);
        cfg.output_gain_db = -3.0f + static_cast<float>(i % 6);
        cfg.gain_smoothing_ms = 50.0f + static_cast<float>(i % 100);
        cfg.noise_floor_db = -70.0f + static_cast<float>(i % 20);
        cfg.linked_detection = (i & 1) != 0;
        rider.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        rider.process(channels, 1, 256);
        return all_finite(block);
      });
  VocalRiderConfig final{};
  final.target_db = -15.0f;
  rider.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  rider.process(channels, 1, 256);
  REQUIRE(rider.config().target_db == -15.0f);
}

TEST_CASE("UpwardCompressor set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  UpwardCompressor uc({});
  uc.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        UpwardCompressorConfig cfg{};
        cfg.threshold_db = -40.0f + static_cast<float>(i % 20);
        cfg.ratio = 1.5f + static_cast<float>(i % 4);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 20);
        cfg.release_ms = 50.0f + static_cast<float>(i % 100);
        cfg.range_db = 6.0f + static_cast<float>(i % 12);
        uc.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        uc.process(channels, 1, 256);
        return all_finite(block);
      });
  UpwardCompressorConfig final{};
  final.ratio = 3.0f;
  uc.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  uc.process(channels, 1, 256);
  REQUIRE(uc.config().ratio == 3.0f);
}

TEST_CASE("UpwardExpander set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  UpwardExpander ue({});
  ue.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  run_rt_safe_race(
      [&](int i) {
        UpwardExpanderConfig cfg{};
        cfg.threshold_db = -30.0f + static_cast<float>(i % 15);
        cfg.ratio = 1.2f + 0.1f * static_cast<float>(i % 20);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 10);
        cfg.release_ms = 50.0f + static_cast<float>(i % 100);
        cfg.range_db = 6.0f + static_cast<float>(i % 12);
        ue.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        ue.process(channels, 1, 256);
        return all_finite(block);
      });
  UpwardExpanderConfig final{};
  final.ratio = 2.0f;
  ue.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  ue.process(channels, 1, 256);
  REQUIRE(ue.config().ratio == 2.0f);
}

TEST_CASE("BrickwallLimiter set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  BrickwallLimiter bw({});
  bw.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  // lookahead_ms is held constant — buffers are sized in prepare().
  run_rt_safe_race(
      [&](int i) {
        BrickwallLimiterConfig cfg{};
        cfg.ceiling_db = -3.0f + 0.1f * static_cast<float>(i % 20);
        cfg.lookahead_ms = 1.0f;
        cfg.release_ms = 20.0f + static_cast<float>(i % 100);
        bw.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        bw.process(channels, 1, 256);
        return all_finite(block);
      });
  BrickwallLimiterConfig final{};
  final.ceiling_db = -0.5f;
  final.lookahead_ms = 1.0f;
  bw.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  bw.process(channels, 1, 256);
  REQUIRE(bw.config().ceiling_db == -0.5f);
}

TEST_CASE("TransientShaper set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  TransientShaper ts({});
  ts.prepare(48000.0, 256);
  std::vector<float> block(256);
  float* channels[] = {block.data()};
  // lookahead_ms is held constant — buffers are sized in prepare().
  run_rt_safe_race(
      [&](int i) {
        TransientShaperConfig cfg{};
        cfg.attack_gain_db = -6.0f + 0.5f * static_cast<float>(i % 24);
        cfg.sustain_gain_db = -3.0f + 0.5f * static_cast<float>(i % 12);
        cfg.fast_attack_ms = 0.1f + 0.1f * static_cast<float>(i % 10);
        cfg.fast_release_ms = 10.0f + static_cast<float>(i % 30);
        cfg.slow_attack_ms = 5.0f + static_cast<float>(i % 30);
        cfg.slow_release_ms = 100.0f + static_cast<float>(i % 300);
        cfg.sensitivity = 0.5f + 0.1f * static_cast<float>(i % 10);
        cfg.max_gain_db = 6.0f + static_cast<float>(i % 12);
        cfg.gain_smoothing_ms = static_cast<float>(i % 20);
        cfg.lookahead_ms = 0.0f;
        ts.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        ts.process(channels, 1, 256);
        return all_finite(block);
      });
  TransientShaperConfig final{};
  final.attack_gain_db = 6.0f;
  ts.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  ts.process(channels, 1, 256);
  REQUIRE(ts.config().attack_gain_db == 6.0f);
}

TEST_CASE("SidechainRouter set_config is safe to call concurrently with process",
          "[mastering][dynamics][rt-safe]") {
  SidechainRouter sr({});
  sr.prepare(48000.0, 256);
  std::vector<float> block(256);
  std::vector<float> key(256);
  float* channels[] = {block.data()};
  const float* key_channels[] = {key.data()};
  // lookahead_ms is held constant.
  run_rt_safe_race(
      [&](int i) {
        SidechainRouterConfig cfg{};
        cfg.threshold_db = -30.0f + static_cast<float>(i % 20);
        cfg.ratio = 2.0f + static_cast<float>(i % 6);
        cfg.attack_ms = 1.0f + static_cast<float>(i % 10);
        cfg.release_ms = 50.0f + static_cast<float>(i % 200);
        cfg.range_db = 6.0f + static_cast<float>(i % 18);
        cfg.sidechain_hpf_enabled = (i % 3) == 0;
        cfg.sidechain_hpf_hz = 60.0f + static_cast<float>(i % 200);
        cfg.mono_summing = (i & 1) != 0;
        cfg.key_listen = false;
        cfg.lookahead_ms = 0.0f;
        sr.set_config(cfg);
      },
      [&] {
        fill_sine_block(block, 1000.0f, 48000);
        fill_sine_block(key, 500.0f, 48000);
        sr.set_sidechain(key_channels, 1, 256);
        sr.process(channels, 1, 256);
        return all_finite(block);
      });
  SidechainRouterConfig final{};
  final.threshold_db = -18.0f;
  sr.set_config(final);
  fill_sine_block(block, 1000.0f, 48000);
  fill_sine_block(key, 500.0f, 48000);
  sr.set_sidechain(key_channels, 1, 256);
  sr.process(channels, 1, 256);
  REQUIRE(sr.config().threshold_db == -18.0f);
}
