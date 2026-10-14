/// @file dynamics_expander_gate_test.cpp
/// @brief Expander, gate, upward dynamics, and de-esser tests.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <utility>

#include "dynamics_test_helpers.h"
#include "util/exception.h"

TEST_CASE("Expander reduces signal below threshold and preserves louder signal",
          "[mastering][dynamics]") {
  Expander expander({-30.0f, 2.0f, 0.0f, 20.0f, -40.0f});
  expander.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.01f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(expander, quiet);
  expander.reset();
  process(expander, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before < 0.35f);
  REQUIRE(rms_tail(loud, 4096) / loud_before > 0.95f);
  REQUIRE(expander.last_gain_reduction_db() == 0.0f);
}

TEST_CASE("Expander validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Expander({-40.0f, 0.5f, 5.0f, 100.0f, -40.0f}));
  REQUIRE_THROWS(Expander({-40.0f, 2.0f, -1.0f, 100.0f, -40.0f}));
  REQUIRE_THROWS(Expander({-40.0f, 2.0f, 5.0f, 100.0f, 1.0f}));
}

TEST_CASE("Gate strongly attenuates noise below threshold", "[mastering][dynamics]") {
  Gate gate({-35.0f, 0.0f, 20.0f, -70.0f});
  gate.prepare(48000.0, 1024);

  std::vector<float> noise(48000, 0.005f);
  std::vector<float> signal(48000, 0.4f);
  const float noise_before = rms_tail(noise, 4096);
  const float signal_before = rms_tail(signal, 4096);

  process(gate, noise);
  gate.reset();
  process(gate, signal);

  REQUIRE(rms_tail(noise, 4096) / noise_before < 0.05f);
  REQUIRE(rms_tail(signal, 4096) / signal_before > 0.95f);
}

TEST_CASE("Gate detector follows the non-excluded planes and applies one gain to all",
          "[mastering][dynamics][detector-exclusion]") {
  constexpr int kBlock = 1024;
  constexpr int kBlocks = 40;
  const auto run = [&](int excluded_channel) {
    Gate gate({-35.0f, 0.0f, 20.0f, -70.0f});
    gate.prepare(48000.0, kBlock);
    gate.set_detector_excluded_channel(excluded_channel);
    std::vector<float> loud(kBlock * kBlocks, 0.4f);
    std::vector<float> quiet(kBlock * kBlocks, 0.005f);
    for (int b = 0; b < kBlocks; ++b) {
      float* planes[2] = {loud.data() + b * kBlock, quiet.data() + b * kBlock};
      gate.process(planes, 2, kBlock);
    }
    return std::make_pair(loud.back(), quiet.back());
  };

  // Nothing excluded: the loud plane keeps the gate open for both planes.
  const auto open = run(-1);
  REQUIRE(open.first == Catch::Approx(0.4f).margin(1.0e-4f));
  REQUIRE(open.second == Catch::Approx(0.005f).margin(1.0e-6f));

  // Loud plane excluded: only the quiet plane is heard, the gate closes, and both planes drop.
  const auto closed = run(0);
  REQUIRE(closed.first < 0.4f * 0.05f);
  REQUIRE(closed.second < 0.005f * 0.05f);
  REQUIRE(closed.first / 0.4f == Catch::Approx(closed.second / 0.005f).epsilon(1.0e-5));
}

TEMPLATE_TEST_CASE("Linked dynamics share one gain across the prepared channel limit",
                   "[mastering][dynamics][linked-follower]", Expander, UpwardCompressor,
                   UpwardExpander) {
  constexpr int kBlock = 512;
  const int limit = static_cast<int>(sonare::mastering::dynamics::kRealtimePreparedChannels);

  const auto run = [&](int channels, bool automate) {
    TestType processor;
    processor.prepare(48000.0, kBlock);
    std::vector<std::vector<float>> planes(static_cast<size_t>(channels));
    for (int ch = 0; ch < channels; ++ch) {
      planes[static_cast<size_t>(ch)].assign(kBlock, 0.01f * static_cast<float>(ch + 1));
    }
    std::vector<float*> pointers;
    for (auto& plane : planes) pointers.push_back(plane.data());
    for (int block = 0; block < 4; ++block) {
      for (int ch = 0; ch < channels; ++ch) {
        planes[static_cast<size_t>(ch)].assign(kBlock, 0.01f * static_cast<float>(ch + 1));
      }
      // Re-sending the held threshold must neither reset the envelope nor change the result.
      if (automate) REQUIRE(processor.set_parameter(0, processor.config().threshold_db));
      processor.process(pointers.data(), channels, kBlock);
    }
    return planes;
  };

  const auto planes = run(limit, false);
  for (int ch = 1; ch < limit; ++ch) {
    const auto& plane = planes[static_cast<size_t>(ch)];
    INFO("channel " << ch);
    // The same gain on every plane: the ratio of output to input is the channel-0 ratio.
    REQUIRE(plane.back() / (0.01f * static_cast<float>(ch + 1)) ==
            Catch::Approx(planes[0].back() / 0.01f).epsilon(1.0e-5));
  }
  REQUIRE(run(limit, true) == planes);

  TestType processor;
  processor.prepare(48000.0, kBlock);
  std::vector<std::vector<float>> too_many(static_cast<size_t>(limit) + 1,
                                           std::vector<float>(kBlock, 0.1f));
  std::vector<float*> pointers;
  for (auto& plane : too_many) pointers.push_back(plane.data());
  REQUIRE_THROWS_AS(processor.process(pointers.data(), limit + 1, kBlock), sonare::SonareException);
}

TEST_CASE("Gate hold and hysteresis keep the gate open briefly", "[mastering][dynamics]") {
  Gate gate({-30.0f, 0.0f, 0.0f, -80.0f, 10.0f, -40.0f, 0.0f});
  gate.prepare(1000.0, 64);

  std::vector<float> signal(32, 0.001f);
  signal[0] = 0.5f;
  process(gate, signal);

  REQUIRE(signal[1] > 0.0008f);
  REQUIRE(std::abs(signal.back()) < 0.0002f);
}

TEST_CASE("Gate validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Gate({-40.0f, -1.0f, 100.0f, -80.0f}));
  REQUIRE_THROWS(Gate({-40.0f, 1.0f, -100.0f, -80.0f}));
  REQUIRE_THROWS(Gate({-40.0f, 1.0f, 100.0f, 1.0f}));
}

TEST_CASE("Gate rejects non-finite configuration fields but preserves negative infinity range",
          "[mastering][dynamics][validation]") {
  const GateConfig baseline{-40.0f, 1.0f, 100.0f, -80.0f, 10.0f, -45.0f, 120.0f};
  Gate gate(baseline);
  const std::array<float GateConfig::*, 6> finite_fields = {
      &GateConfig::threshold_db, &GateConfig::attack_ms,          &GateConfig::release_ms,
      &GateConfig::hold_ms,      &GateConfig::close_threshold_db, &GateConfig::key_hpf_hz};
  const std::array<float, 3> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
                                                  std::numeric_limits<float>::infinity(),
                                                  -std::numeric_limits<float>::infinity()};

  for (const auto field : finite_fields) {
    for (const float value : non_finite_values) {
      auto invalid = baseline;
      invalid.*field = value;
      REQUIRE_THROWS(gate.set_config(invalid));
      CHECK(gate.config().threshold_db == baseline.threshold_db);
      CHECK(gate.config().attack_ms == baseline.attack_ms);
      CHECK(gate.config().release_ms == baseline.release_ms);
      CHECK(gate.config().range_db == baseline.range_db);
      CHECK(gate.config().hold_ms == baseline.hold_ms);
      CHECK(gate.config().close_threshold_db == baseline.close_threshold_db);
      CHECK(gate.config().key_hpf_hz == baseline.key_hpf_hz);
    }
  }

  for (const float value :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    auto invalid = baseline;
    invalid.range_db = value;
    REQUIRE_THROWS(gate.set_config(invalid));
    CHECK(gate.config().range_db == baseline.range_db);
  }

  auto open_floor = baseline;
  open_floor.range_db = -std::numeric_limits<float>::infinity();
  REQUIRE_NOTHROW(gate.set_config(open_floor));
  CHECK(gate.config().range_db == -std::numeric_limits<float>::infinity());
}

TEST_CASE("Gate saturates an oversized hold duration before converting to samples",
          "[mastering][dynamics][validation]") {
  Gate gate({-30.0f, 0.0f, 0.0f, -80.0f, std::numeric_limits<float>::max(), -35.0f, 0.0f});
  gate.prepare(48000.0, 16);

  std::vector<float> samples = {1.0f, 0.0f, 0.0f};
  float* channels[] = {samples.data()};
  REQUIRE_NOTHROW(gate.process(channels, 1, static_cast<int>(samples.size())));
  REQUIRE(std::all_of(samples.begin(), samples.end(),
                      [](float value) { return std::isfinite(value); }));
}

TEST_CASE("UpwardCompressor raises quiet signal below threshold", "[mastering][dynamics]") {
  UpwardCompressor upward({-20.0f, 2.0f, 0.0f, 20.0f, 12.0f});
  upward.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.02f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(upward, quiet);
  upward.reset();
  process(upward, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 2.0f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 1.05f);
  REQUIRE(upward.last_gain_db() == 0.0f);
  REQUIRE(upward.last_gain_reduction_db() == upward.last_gain_db());
}

TEST_CASE("UpwardCompressor validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 0.5f, 10.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 2.0f, -1.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 2.0f, 10.0f, 100.0f, -1.0f}));
}

TEST_CASE("UpwardCompressor preserves existing channel state when channel count grows",
          "[mastering][dynamics]") {
  UpwardCompressor mono_path({-20.0f, 2.0f, 20.0f, 80.0f, 12.0f});
  UpwardCompressor stereo_path({-20.0f, 2.0f, 20.0f, 80.0f, 12.0f});
  mono_path.prepare(48000.0, 1024);
  stereo_path.prepare(48000.0, 1024);

  std::vector<float> warmup(4096, 0.02f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  std::vector<float> expected_left(512, 0.02f);
  std::vector<float> actual_left = expected_left;
  std::vector<float> actual_right(512, 0.02f);
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("UpwardCompressor shares one linked detector across channels", "[mastering][dynamics]") {
  // With linked stereo detection there is a single shared detector envelope, so
  // both channels always receive the same gain and an intervening mono block
  // legitimately advances that shared detector. The defining invariant is that
  // the left and right outputs of a stereo block are identical sample-by-sample
  // (preserved stereo image), even after a mono block in between.
  UpwardCompressor under_test({-20.0f, 2.0f, 20.0f, 80.0f, 12.0f});
  under_test.prepare(48000.0, 1024);

  std::vector<float> warm_l(4096, 0.02f);
  std::vector<float> warm_r(4096, 0.02f);
  process_stereo(under_test, warm_l, warm_r);

  std::vector<float> mono(512, 0.02f);
  process(under_test, mono);

  std::vector<float> test_l(512, 0.02f);
  std::vector<float> test_r(512, 0.02f);
  process_stereo(under_test, test_l, test_r);

  REQUIRE(max_abs_difference(test_l, test_r) < 1.0e-6f);
}

TEST_CASE("UpwardExpander raises signal above threshold and leaves quiet signal alone",
          "[mastering][dynamics]") {
  UpwardExpander upward({-24.0f, 1.5f, 0.0f, 20.0f, 12.0f});
  upward.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.02f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(upward, quiet);
  upward.reset();
  process(upward, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before < 1.05f);
  REQUIRE(rms_tail(loud, 4096) / loud_before > 1.4f);
  REQUIRE(upward.last_gain_db() > 3.0f);
  REQUIRE(upward.last_gain_reduction_db() == upward.last_gain_db());
}

TEST_CASE("UpwardExpander validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(UpwardExpander({-24.0f, 0.5f, 10.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardExpander({-24.0f, 2.0f, -1.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardExpander({-24.0f, 2.0f, 10.0f, 100.0f, -1.0f}));
}

TEST_CASE(
    "Dynamics configurations reject non-finite fields without changing the control-thread mirror",
    "[mastering][dynamics][validation]") {
  const std::array<float, 3> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
                                                  std::numeric_limits<float>::infinity(),
                                                  -std::numeric_limits<float>::infinity()};

  const ExpanderConfig expander_baseline{-40.0f, 2.0f, 5.0f, 100.0f, -40.0f};
  Expander expander(expander_baseline);
  const std::array<float ExpanderConfig::*, 5> expander_fields = {
      &ExpanderConfig::threshold_db, &ExpanderConfig::ratio, &ExpanderConfig::attack_ms,
      &ExpanderConfig::release_ms, &ExpanderConfig::range_db};
  for (const auto field : expander_fields) {
    for (const float value : non_finite_values) {
      auto invalid = expander_baseline;
      invalid.*field = value;
      REQUIRE_THROWS(expander.set_config(invalid));
      CHECK(expander.config().threshold_db == expander_baseline.threshold_db);
      CHECK(expander.config().ratio == expander_baseline.ratio);
      CHECK(expander.config().attack_ms == expander_baseline.attack_ms);
      CHECK(expander.config().release_ms == expander_baseline.release_ms);
      CHECK(expander.config().range_db == expander_baseline.range_db);
    }
  }

  const UpwardCompressorConfig compressor_baseline{-24.0f, 2.0f, 10.0f, 100.0f, 12.0f};
  UpwardCompressor compressor(compressor_baseline);
  const std::array<float UpwardCompressorConfig::*, 5> compressor_fields = {
      &UpwardCompressorConfig::threshold_db, &UpwardCompressorConfig::ratio,
      &UpwardCompressorConfig::attack_ms, &UpwardCompressorConfig::release_ms,
      &UpwardCompressorConfig::range_db};
  for (const auto field : compressor_fields) {
    for (const float value : non_finite_values) {
      auto invalid = compressor_baseline;
      invalid.*field = value;
      REQUIRE_THROWS(compressor.set_config(invalid));
      CHECK(compressor.config().threshold_db == compressor_baseline.threshold_db);
      CHECK(compressor.config().ratio == compressor_baseline.ratio);
      CHECK(compressor.config().attack_ms == compressor_baseline.attack_ms);
      CHECK(compressor.config().release_ms == compressor_baseline.release_ms);
      CHECK(compressor.config().range_db == compressor_baseline.range_db);
    }
  }
  auto huge_compressor_range = compressor_baseline;
  huge_compressor_range.range_db = std::numeric_limits<float>::max();
  REQUIRE_THROWS(compressor.set_config(huge_compressor_range));
  CHECK(compressor.config().range_db == compressor_baseline.range_db);

  const UpwardExpanderConfig expander_up_baseline{-24.0f, 1.5f, 5.0f, 80.0f, 12.0f};
  UpwardExpander upward_expander(expander_up_baseline);
  const std::array<float UpwardExpanderConfig::*, 5> upward_expander_fields = {
      &UpwardExpanderConfig::threshold_db, &UpwardExpanderConfig::ratio,
      &UpwardExpanderConfig::attack_ms, &UpwardExpanderConfig::release_ms,
      &UpwardExpanderConfig::range_db};
  for (const auto field : upward_expander_fields) {
    for (const float value : non_finite_values) {
      auto invalid = expander_up_baseline;
      invalid.*field = value;
      REQUIRE_THROWS(upward_expander.set_config(invalid));
      CHECK(upward_expander.config().threshold_db == expander_up_baseline.threshold_db);
      CHECK(upward_expander.config().ratio == expander_up_baseline.ratio);
      CHECK(upward_expander.config().attack_ms == expander_up_baseline.attack_ms);
      CHECK(upward_expander.config().release_ms == expander_up_baseline.release_ms);
      CHECK(upward_expander.config().range_db == expander_up_baseline.range_db);
    }
  }
  auto huge_expander_range = expander_up_baseline;
  huge_expander_range.range_db = std::numeric_limits<float>::max();
  REQUIRE_THROWS(upward_expander.set_config(huge_expander_range));
  CHECK(upward_expander.config().range_db == expander_up_baseline.range_db);
}

TEST_CASE("UpwardExpander preserves existing channel state when channel count grows",
          "[mastering][dynamics]") {
  UpwardExpander mono_path({-30.0f, 1.5f, 20.0f, 80.0f, 12.0f});
  UpwardExpander stereo_path({-30.0f, 1.5f, 20.0f, 80.0f, 12.0f});
  mono_path.prepare(48000.0, 1024);
  stereo_path.prepare(48000.0, 1024);

  std::vector<float> warmup(4096, 0.5f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  std::vector<float> expected_left(512, 0.5f);
  std::vector<float> actual_left = expected_left;
  std::vector<float> actual_right(512, 0.5f);
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("UpwardExpander shares one linked detector across channels", "[mastering][dynamics]") {
  // Linked stereo detection (mirrors UpwardCompressor): a single shared detector
  // envelope drives the same gain on both channels, so an intervening mono block
  // legitimately advances that shared detector. The defining invariant is that
  // the left and right outputs of a stereo block stay identical sample-by-sample
  // (preserved stereo image), even after a mono block in between — independent
  // per-channel followers would let L/R diverge and rotate the image.
  UpwardExpander under_test({-30.0f, 1.5f, 20.0f, 80.0f, 12.0f});
  under_test.prepare(48000.0, 1024);

  std::vector<float> warm_l(4096, 0.5f);
  std::vector<float> warm_r(4096, 0.5f);
  process_stereo(under_test, warm_l, warm_r);

  std::vector<float> mono(512, 0.5f);
  process(under_test, mono);

  std::vector<float> test_l(512, 0.5f);
  std::vector<float> test_r(512, 0.5f);
  process_stereo(under_test, test_l, test_r);

  REQUIRE(max_abs_difference(test_l, test_r) < 1.0e-6f);
}
