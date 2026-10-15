/// @file ring_modulator_test.cpp
/// @brief Input-domain contract of the ring modulator: construction and live automation.

#include "effects/modulation/ring_modulator.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"

namespace {

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::effects::modulation::RingModulator;
using sonare::effects::modulation::RingModulatorConfig;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kBlock = 2048;
constexpr double kRate = 48000.0;

/// One numeric field of RingModulatorConfig with its automation id and closed domain.
struct DomainRow {
  const char* name;
  float RingModulatorConfig::*member;
  unsigned int id;
  float lo;
  float hi;
};

const DomainRow kDomainRows[] = {
    {"carrierHz", &RingModulatorConfig::carrier_hz, 0, 0.0f, kInf},
    {"dryWet", &RingModulatorConfig::dry_wet, 1, 0.0f, 1.0f},
    {"phaseDeg", &RingModulatorConfig::phase_deg, 3, 0.0f, 360.0f},
    {"stereoSpread", &RingModulatorConfig::stereo_spread, 4, 0.0f, 1.0f},
};

std::vector<float> refused(const DomainRow& row) {
  std::vector<float> values{kNaN, kInf, -kInf, std::nextafter(row.lo, -kInf)};
  if (std::isfinite(row.hi)) values.push_back(std::nextafter(row.hi, kInf));
  return values;
}

std::vector<float> accepted(const DomainRow& row) {
  return {row.lo, std::isfinite(row.hi) ? row.hi : 1234.5f};
}

RingModulatorConfig base_config() {
  RingModulatorConfig config;
  config.dry_wet = 0.7f;
  config.phase_deg = 40.0f;
  config.stereo_spread = 0.3f;
  return config;
}

std::vector<float> render(RingModulator& modulator) {
  std::vector<float> left(static_cast<std::size_t>(kBlock));
  std::vector<float> right(static_cast<std::size_t>(kBlock));
  for (int i = 0; i < kBlock; ++i) {
    const float t = static_cast<float>(i);
    left[static_cast<std::size_t>(i)] = 0.5f * std::sin(0.05f * t);
    right[static_cast<std::size_t>(i)] = 0.4f * std::sin(0.11f * t + 0.5f);
  }
  float* channels[] = {left.data(), right.data()};
  modulator.process(channels, 2, kBlock);
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

}  // namespace

TEST_CASE("RingModulator construction refuses a value outside a field's domain", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : refused(row)) {
      CAPTURE(row.name, bad);
      RingModulatorConfig config;
      config.*row.member = bad;
      try {
        RingModulator modulator(config);
        FAIL("construction accepted " << row.name);
      } catch (const SonareException& error) {
        CHECK(error.code() == ErrorCode::InvalidParameter);
        CHECK(std::string(error.what()).find(row.name) != std::string::npos);
      }
      CHECK_THROWS_AS(RingModulator::validate_config(config), SonareException);
    }
    for (const float good : accepted(row)) {
      CAPTURE(row.name, good);
      RingModulatorConfig config;
      config.*row.member = good;
      CHECK_NOTHROW(RingModulator::validate_config(config));
      CHECK_NOTHROW(RingModulator(config));
    }
  }
}

TEST_CASE("RingModulator live automation refuses an out-of-domain value and changes nothing",
          "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : refused(row)) {
      CAPTURE(row.name, bad);
      RingModulator touched(base_config());
      touched.prepare(kRate, kBlock);
      CHECK_FALSE(touched.set_parameter(row.id, bad));
      RingModulator untouched(base_config());
      untouched.prepare(kRate, kBlock);
      CHECK(render(touched) == render(untouched));
    }
  }
}

TEST_CASE("RingModulator live automation applies a value on a bound exactly", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float good : accepted(row)) {
      CAPTURE(row.name, good);
      RingModulator live(base_config());
      live.prepare(kRate, kBlock);
      REQUIRE(live.set_parameter(row.id, good));
      RingModulatorConfig config = base_config();
      config.*row.member = good;
      RingModulator constructed(config);
      constructed.prepare(kRate, kBlock);
      CHECK(render(live) == render(constructed));
    }
  }
}
