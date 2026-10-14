/// @file stereo_delay_test.cpp
/// @brief Remaining target/current tail and output-scale boundaries.

#include "effects/delay/stereo_delay.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "rt/tail_budget.h"

namespace {

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;

constexpr float kInput = 0.25f;

void process_stereo(StereoDelay& processor, std::vector<float>& left, std::vector<float>& right,
                    int block = 256) {
  REQUIRE(left.size() == right.size());
  for (int offset = 0; offset < static_cast<int>(left.size());) {
    const int count = std::min(block, static_cast<int>(left.size()) - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
    offset += count;
  }
}

void prime(StereoDelay& processor, int samples = 64) {
  std::vector<float> left(static_cast<std::size_t>(samples), kInput);
  std::vector<float> right = left;
  process_stereo(processor, left, right);
}

float peak_after(const std::vector<float>& left, const std::vector<float>& right, int first) {
  REQUIRE(left.size() == right.size());
  float peak = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < left.size(); ++i) {
    peak = std::max(peak, std::abs(left[i]));
    peak = std::max(peak, std::abs(right[i]));
  }
  return peak;
}

float peak(const std::vector<float>& left, const std::vector<float>& right) {
  return peak_after(left, right, 0);
}

StereoDelayConfig loop_config() {
  StereoDelayConfig config;
  config.delay_time_l_ms = 20.0f;
  config.delay_time_r_ms = 20.0f;
  config.feedback = 0.9f;
  config.dry_wet = 1.0f;
  return config;
}

std::vector<float> silence(int samples) {
  return std::vector<float>(static_cast<std::size_t>(samples), 0.0f);
}

}  // namespace

TEST_CASE("StereoDelay tail covers a glided delay's resident target/current state", "[boundary]") {
  for (const int sample_rate : {1000, 48000, 96000}) {
    StereoDelayConfig config;
    config.delay_time_l_ms = 100.0f;
    config.delay_time_r_ms = 100.0f;
    config.feedback = 0.0f;
    config.dry_wet = 1.0f;
    config.glide_ms = 1000.0f;
    StereoDelay subject(config);
    subject.prepare(sample_rate, 256);
    prime(subject);
    const int old_tail = subject.tail_samples();
    REQUIRE(subject.set_parameter(0, 10.0f));
    REQUIRE(subject.set_parameter(1, 10.0f));
    const int declared_tail = subject.tail_samples();
    const int render = declared_tail + std::max(256, sample_rate / 2);
    std::vector<float> left = silence(render);
    std::vector<float> right = left;
    process_stereo(subject, left, right);
    CAPTURE(sample_rate, old_tail, declared_tail, peak_after(left, right, declared_tail));
    REQUIRE(declared_tail >= old_tail);
    REQUIRE(peak_after(left, right, declared_tail) <= 1.0e-6f);
  }
}

TEST_CASE("StereoDelay tail covers feedback that is still gliding to zero", "[boundary]") {
  for (const int sample_rate : {1000, 48000, 96000}) {
    StereoDelay subject(loop_config());
    subject.prepare(sample_rate, 256);
    prime(subject);
    REQUIRE(subject.set_parameter(2, 0.0f));
    const int declared_tail = subject.tail_samples();
    const int render = declared_tail + std::max(2048, sample_rate / 10);
    std::vector<float> left = silence(render);
    std::vector<float> right = left;
    process_stereo(subject, left, right);
    CAPTURE(sample_rate, declared_tail, peak_after(left, right, declared_tail));
    REQUIRE(declared_tail > 0);
    REQUIRE(peak_after(left, right, declared_tail) <= kInput * sonare::rt::kTailFloor);

    StereoDelay static_zero(loop_config());
    REQUIRE(static_zero.set_parameter(2, 0.0f));
    static_zero.prepare(sample_rate, 256);
    std::vector<float> static_silence = silence(render);
    std::vector<float> static_right = static_silence;
    process_stereo(static_zero, static_silence, static_right);
    REQUIRE(peak(static_silence, static_right) == 0.0f);
  }
}

TEST_CASE("StereoDelay tail covers a wet glide to an exact dry target", "[boundary]") {
  for (const int sample_rate : {1000, 48000, 96000}) {
    StereoDelay subject(loop_config());
    subject.prepare(sample_rate, 256);
    prime(subject);
    REQUIRE(subject.set_parameter(4, 0.0f));
    const int declared_tail = subject.tail_samples();
    const int render = std::max(2048, sample_rate / 10);
    std::vector<float> left = silence(render);
    std::vector<float> right = left;
    process_stereo(subject, left, right);
    CAPTURE(sample_rate, declared_tail, peak(left, right));
    REQUIRE(declared_tail > 0);
    REQUIRE(peak_after(left, right, declared_tail) <= kInput * sonare::rt::kTailFloor);

    StereoDelay static_dry(loop_config());
    REQUIRE(static_dry.set_parameter(4, 0.0f));
    static_dry.prepare(sample_rate, 256);
    std::vector<float> static_input = silence(render);
    std::vector<float> static_right = static_input;
    process_stereo(static_dry, static_input, static_right);
    REQUIRE(peak(static_input, static_right) == 0.0f);
  }
}

TEST_CASE("StereoDelay tail grows for heard output levels without changing loop gain",
          "[boundary]") {
  StereoDelayConfig unity_config = loop_config();
  StereoDelayConfig boosted_config = unity_config;
  boosted_config.tap1_level_db = 12.0f;
  boosted_config.tap2_level_db = 12.0f;

  for (const int sample_rate : {1000, 48000, 96000}) {
    StereoDelay unity(unity_config);
    StereoDelay boosted(boosted_config);
    unity.prepare(sample_rate, 256);
    boosted.prepare(sample_rate, 256);
    prime(unity, 1);
    prime(boosted, 1);
    const int unity_tail = unity.tail_samples();
    const int boosted_tail = boosted.tail_samples();
    CAPTURE(sample_rate, unity_tail, boosted_tail);
    REQUIRE(boosted_tail > unity_tail);

    if (sample_rate == 1000) {
      const int render = boosted_tail + 128;
      std::vector<float> left = silence(render);
      std::vector<float> right = left;
      process_stereo(boosted, left, right);
      REQUIRE(peak_after(left, right, boosted_tail) <= sonare::rt::kTailFloor);
    }
  }
}

TEST_CASE("StereoDelay finite feed-forward tap remains finite-support after gain scaling",
          "[boundary]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 0.0f;
  config.delay_time_r_ms = 0.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.tap1_level_db = -120.0f;
  config.tap2_level_db = -120.0f;
  config.tap3_ms = 30.0f;
  config.tap3_level_db = 12.0f;
  StereoDelay subject(config);
  subject.prepare(1000.0, 256);
  prime(subject, 1);
  const int declared_tail = subject.tail_samples();
  std::vector<float> left = silence(declared_tail + 128);
  std::vector<float> right = left;
  process_stereo(subject, left, right);
  REQUIRE(declared_tail > 0);
  REQUIRE(peak_after(left, right, declared_tail) == 0.0f);
}
