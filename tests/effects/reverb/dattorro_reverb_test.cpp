/// @file dattorro_reverb_test.cpp
/// @brief Tests for the Dattorro tank's damping as a physical quantity.

#include "effects/reverb/dattorro_reverb.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"

using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;

namespace {

std::vector<float> impulse_response(const DattorroReverbConfig& config, double sample_rate) {
  DattorroReverb reverb(config);
  reverb.prepare(sample_rate, 512);
  std::vector<float> left(static_cast<size_t>(0.5 * sample_rate), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  reverb.process(channels, 2, static_cast<int>(left.size()));
  return left;
}

/// Largest sample-wise difference relative to the response's peak.
double relative_difference(const std::vector<float>& a, const std::vector<float>& b) {
  double peak = 0.0;
  double diff = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    peak = std::max(peak, static_cast<double>(std::abs(a[i])));
    diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
  }
  return diff / peak;
}

}  // namespace

TEST_CASE("Dattorro damping keeps its corner in Hz at every rate", "[dattorro][sample_rate]") {
  // damping 0.5 states d = 0.2 at the reference rate; that one-pole's half-power corner is the
  // physical quantity the tank holds.
  const float reference_coefficient = 0.5f * 0.4f;
  const double corner_hz = DattorroReverb::damping_corner_hz(
      reference_coefficient, DattorroReverb::kDampingReferenceSampleRate);
  REQUIRE(corner_hz > 1000.0);
  REQUIRE(corner_hz < 3000.0);
  // The corner and the coefficient are inverses at the reference rate.
  CHECK(DattorroReverb::damping_coefficient(
            corner_hz, DattorroReverb::kDampingReferenceSampleRate) == reference_coefficient);

  DattorroReverbConfig by_damping;
  by_damping.damping = 0.5f;
  by_damping.dry_wet = 1.0f;
  by_damping.mod_depth_samples = 0.0f;
  DattorroReverbConfig by_corner = by_damping;
  by_corner.damping_hz = static_cast<float>(corner_hz);

  for (double sr : {44100.0, 48000.0, 96000.0}) {
    INFO("sr " << sr);
    // The damping path renders what the corner path renders when asked for that corner, so its
    // corner is the requested one at each rate, and hence the same across rates.
    CHECK(relative_difference(impulse_response(by_damping, sr), impulse_response(by_corner, sr)) <=
          1e-4);
    const float coefficient = DattorroReverb::damping_coefficient(corner_hz, sr);
    CHECK(std::abs(DattorroReverb::damping_corner_hz(coefficient, sr) / corner_hz - 1.0) <= 1e-4);
  }

  // The coefficient itself does move with the rate: a raw per-sample value would not.
  CHECK(DattorroReverb::damping_coefficient(corner_hz, 44100.0) >
        DattorroReverb::damping_coefficient(corner_hz, 48000.0));
}

TEST_CASE("Dattorro damping automation rebuilds the coefficient at the working rate",
          "[dattorro][sample_rate]") {
  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.mod_depth_samples = 0.0f;
  config.damping = 0.9f;
  const std::vector<float> direct = impulse_response(config, 44100.0);

  config.damping = 0.1f;
  DattorroReverb reverb(config);
  reverb.prepare(44100.0, 512);
  REQUIRE(reverb.set_parameter(1, 0.9f));
  std::vector<float> left(direct.size(), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  reverb.process(channels, 2, static_cast<int>(left.size()));
  CHECK(left == direct);
}

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

struct DattorroDomainRow {
  const char* name;
  float DattorroReverbConfig::*member;
  unsigned int id;  ///< live parameter id carrying the same quantity and unit.
  float lo;
  float hi;
  float inside_lo;  ///< accepted values: the bound, or a representative value when unbounded.
  float inside_hi;
};

const DattorroDomainRow kDattorroRows[] = {
    {"decay", &DattorroReverbConfig::decay, 0, 0.0f, 0.98f, 0.0f, 0.98f},
    {"damping", &DattorroReverbConfig::damping, 1, 0.0f, 1.0f, 0.0f, 1.0f},
    {"dry_wet", &DattorroReverbConfig::dry_wet, 2, 0.0f, 1.0f, 0.0f, 1.0f},
    {"mod_rate_hz", &DattorroReverbConfig::mod_rate_hz, 3, -kInf, kInf, -2.0f, 2.0f},
    {"mod_depth_samples", &DattorroReverbConfig::mod_depth_samples, 4, 0.0f, 672.0f, 0.0f, 672.0f},
    {"damping_hz", &DattorroReverbConfig::damping_hz, 5, 0.0f, kInf, 0.0f, 5000.0f},
    {"gate_threshold_db", &DattorroReverbConfig::gate_threshold_db, 6, -kInf, kInf, -90.0f, 0.0f},
    {"gate_hold_ms", &DattorroReverbConfig::gate_hold_ms, 7, 0.0f, 1.0e6f, 0.0f, 1.0e6f},
};

/// Values outside the row's domain: just past each finite bound, plus the non-finite ones.
std::vector<float> dattorro_refused(const DattorroDomainRow& row) {
  std::vector<float> values{std::numeric_limits<float>::quiet_NaN(), kInf, -kInf};
  if (std::isfinite(row.lo)) values.push_back(std::nextafter(row.lo, -kInf));
  if (std::isfinite(row.hi)) values.push_back(std::nextafter(row.hi, kInf));
  return values;
}

std::vector<float> dattorro_render(DattorroReverb& reverb) {
  reverb.prepare(48000.0, 256);
  std::vector<float> left(2048, 0.0f);
  std::vector<float> right(2048, 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};
  reverb.process(channels, 2, 2048);
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

void require_dattorro_refusal(const DattorroReverbConfig& config, const std::string& field) {
  try {
    DattorroReverb subject(config);
    FAIL("constructed with " << field << " out of range");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    CHECK(std::string(error.what()).find("DattorroReverb: " + field + " out of range") !=
          std::string::npos);
  }
}

constexpr float kDattorroPreDelayMax = 4000.0f * 29761.0f / 1000.0f;

}  // namespace

TEST_CASE("DattorroReverb construction refuses a field outside its domain", "[effects][reverb]") {
  for (const DattorroDomainRow& row : kDattorroRows) {
    for (const float value : dattorro_refused(row)) {
      CAPTURE(row.name, value);
      DattorroReverbConfig config;
      config.*row.member = value;
      require_dattorro_refusal(config, row.name);
    }
  }
  for (const float value : {-1.0f, std::numeric_limits<float>::quiet_NaN(), kInf,
                            std::nextafter(kDattorroPreDelayMax, kInf)}) {
    CAPTURE(value);
    DattorroReverbConfig config;
    config.pre_delay_samples = value;
    require_dattorro_refusal(config, "pre_delay_samples");
  }
  for (const int value : {-1, 7}) {
    CAPTURE(value);
    DattorroReverbConfig config;
    config.character = value;
    require_dattorro_refusal(config, "character");
  }
}

TEST_CASE("DattorroReverb live automation refuses outside the domain and applies a bound exactly",
          "[effects][reverb]") {
  for (const DattorroDomainRow& row : kDattorroRows) {
    DattorroReverb untouched;
    const std::vector<float> expected = dattorro_render(untouched);
    for (const float value : dattorro_refused(row)) {
      CAPTURE(row.name, value);
      DattorroReverb subject;
      CHECK_FALSE(subject.set_parameter(row.id, value));
      CHECK(dattorro_render(subject) == expected);
    }
    for (const float value : {row.inside_lo, row.inside_hi}) {
      CAPTURE(row.name, value);
      DattorroReverbConfig config;
      config.*row.member = value;
      DattorroReverb built(config);
      DattorroReverb automated;
      REQUIRE(automated.set_parameter(row.id, value));
      CHECK(dattorro_render(automated) == dattorro_render(built));
    }
  }
}

TEST_CASE("DattorroReverb live pre-delay and character stay inside what was prepared",
          "[effects][reverb]") {
  DattorroReverb untouched;
  const std::vector<float> expected = dattorro_render(untouched);
  // The pre-delay ring holds the larger of 127 ms and the configured pre-delay.
  for (const float ms : {-1.0f, 127.5f, std::numeric_limits<float>::quiet_NaN(), kInf}) {
    CAPTURE(ms);
    DattorroReverb subject;
    CHECK_FALSE(subject.set_parameter(9, ms));
    CHECK(dattorro_render(subject) == expected);
  }
  for (const float ms : {0.0f, 127.0f}) {
    CAPTURE(ms);
    DattorroReverbConfig config;
    config.pre_delay_samples = ms * 29761.0f / 1000.0f;
    DattorroReverb built(config);
    DattorroReverb automated;
    REQUIRE(automated.set_parameter(9, ms));
    CHECK(dattorro_render(automated) == dattorro_render(built));
  }
  DattorroReverbConfig long_pre_delay;
  long_pre_delay.pre_delay_samples = 200.0f * 29761.0f / 1000.0f;
  DattorroReverb roomy(long_pre_delay);
  CHECK(roomy.set_parameter(9, 190.0f));
  CHECK_FALSE(roomy.set_parameter(9, 201.0f));

  for (const float value : {-1.0f, 7.0f, 1.5f, std::numeric_limits<float>::quiet_NaN()}) {
    CAPTURE(value);
    DattorroReverb subject;
    CHECK_FALSE(subject.set_parameter(10, value));
    CHECK(dattorro_render(subject) == expected);
  }
  for (const int character : {0, 6}) {
    CAPTURE(character);
    DattorroReverbConfig config;
    config.character = character;
    DattorroReverb built(config);
    DattorroReverb automated;
    REQUIRE(automated.set_parameter(10, static_cast<float>(character)));
    CHECK(dattorro_render(automated) == dattorro_render(built));
  }
}
