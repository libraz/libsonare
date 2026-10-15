/// @file stereo_delay_test.cpp
/// @brief Remaining target/current tail and output-scale boundaries.

#include "effects/delay/stereo_delay.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "effects/common/control_ranges.h"
#include "rt/tail_budget.h"
#include "util/exception.h"

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

namespace {

struct DomainRow {
  const char* name;
  float StereoDelayConfig::*member;
  unsigned int id;
  float lo;
  float hi;  ///< infinity when the field has no upper limit.
};

const DomainRow kDomainRows[] = {
    {"delay_time_l_ms", &StereoDelayConfig::delay_time_l_ms, 0, 0.0f, 4000.0f},
    {"delay_time_r_ms", &StereoDelayConfig::delay_time_r_ms, 1, 0.0f, 4000.0f},
    {"feedback", &StereoDelayConfig::feedback, 2, -sonare::effects::common::kMaxFeedback,
     sonare::effects::common::kMaxFeedback},
    {"ping_pong", &StereoDelayConfig::ping_pong, 3, 0.0f, 1.0f},
    {"dry_wet", &StereoDelayConfig::dry_wet, 4, 0.0f, 1.0f},
    {"damping_hz", &StereoDelayConfig::damping_hz, 5, 0.0f, std::numeric_limits<float>::infinity()},
    {"tap3_ms", &StereoDelayConfig::tap3_ms, 6, 0.0f, 4000.0f},
    {"tap4_ms", &StereoDelayConfig::tap4_ms, 7, 0.0f, 4000.0f},
    {"tap1_level_db", &StereoDelayConfig::tap1_level_db, 8, -120.0f, 40.0f},
    {"tap2_level_db", &StereoDelayConfig::tap2_level_db, 9, -120.0f, 40.0f},
    {"tap3_level_db", &StereoDelayConfig::tap3_level_db, 10, -120.0f, 40.0f},
    {"tap4_level_db", &StereoDelayConfig::tap4_level_db, 11, -120.0f, 40.0f},
    {"tap3_pan", &StereoDelayConfig::tap3_pan, 12, -1.0f, 1.0f},
    {"tap4_pan", &StereoDelayConfig::tap4_pan, 13, -1.0f, 1.0f},
    {"mod_rate_hz", &StereoDelayConfig::mod_rate_hz, 16, 0.0f, 100.0f},
    {"mod_depth_ms", &StereoDelayConfig::mod_depth_ms, 17, 0.0f, 4000.0f},
    {"mod_phase_deg", &StereoDelayConfig::mod_phase_deg, 18, 0.0f, 360.0f},
    {"glide_ms", &StereoDelayConfig::glide_ms, 19, 0.0f, 10000.0f},
};

/// The values just outside the domain, plus the non-finite ones.
std::vector<float> refused_values(const DomainRow& row) {
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<float> values{std::nextafter(row.lo, -inf), std::numeric_limits<float>::quiet_NaN(),
                            inf, -inf};
  if (std::isfinite(row.hi)) values.push_back(std::nextafter(row.hi, inf));
  return values;
}

void require_invalid_parameter(const StereoDelayConfig& config, const std::string& field) {
  try {
    StereoDelay subject(config);
    FAIL("constructed with " << field << " out of range");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    CHECK(std::string(error.what()).find("StereoDelay: " + field + " out of range") !=
          std::string::npos);
  }
}

}  // namespace

TEST_CASE("StereoDelay construction refuses a field outside its domain", "[effects][delay]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float value : refused_values(row)) {
      CAPTURE(row.name, value);
      StereoDelayConfig config;
      config.*row.member = value;
      require_invalid_parameter(config, row.name);
    }
  }
}

TEST_CASE("StereoDelay construction accepts a field on its bound", "[effects][delay]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float value : {row.lo, std::isfinite(row.hi) ? row.hi : 1.0e6f}) {
      CAPTURE(row.name, value);
      StereoDelayConfig config;
      config.*row.member = value;
      StereoDelay subject(config);
      CHECK(subject.config().*row.member == value);
    }
  }
}

TEST_CASE("StereoDelay set_config refuses a bad config and keeps the previous one",
          "[effects][delay]") {
  StereoDelayConfig held;
  held.delay_time_l_ms = 120.0f;
  held.feedback = 0.3f;
  held.dry_wet = 0.6f;
  StereoDelay subject(held);
  for (const DomainRow& row : kDomainRows) {
    for (const float value : refused_values(row)) {
      CAPTURE(row.name, value);
      StereoDelayConfig next = held;
      next.*row.member = value;
      CHECK_THROWS_AS(subject.set_config(next), sonare::SonareException);
      for (const DomainRow& other : kDomainRows) {
        CHECK(subject.config().*other.member == held.*other.member);
      }
    }
  }
  StereoDelayConfig good = held;
  good.feedback = -0.5f;
  CHECK_NOTHROW(subject.set_config(good));
  CHECK(subject.config().feedback == -0.5f);
}

TEST_CASE("StereoDelay live automation refuses outside the domain and applies a bound exactly",
          "[effects][delay]") {
  for (const DomainRow& row : kDomainRows) {
    StereoDelayConfig initial;
    StereoDelay subject(initial);
    subject.prepare(48000.0, 256);
    for (const float value : refused_values(row)) {
      CAPTURE(row.name, value);
      CHECK_FALSE(subject.set_parameter(row.id, value));
      for (const DomainRow& other : kDomainRows) {
        CHECK(subject.config().*other.member == initial.*other.member);
      }
    }
    for (const float value : {row.lo, std::isfinite(row.hi) ? row.hi : 1.0e6f}) {
      CAPTURE(row.name, value);
      REQUIRE(subject.set_parameter(row.id, value));
      CHECK(subject.config().*row.member == value);
    }
  }
}
