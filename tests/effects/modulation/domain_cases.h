#pragma once

/// @file domain_cases.h
/// @brief Table-driven checks that a modulation processor refuses a config field
///        outside its domain, at construction and in live automation.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::test::modulation_domain {

/// One config field: its automation id (kConstructionOnly when the field is not
/// automatable) and the closed interval it accepts. Infinite bounds are not probed.
template <typename Config>
struct DomainRow {
  const char* field;
  int id;
  float Config::*member;
  float lo;
  float hi;
};

inline constexpr int kConstructionOnly = -1;
inline constexpr float kInf = std::numeric_limits<float>::infinity();
inline constexpr double kRate = 48000.0;
inline constexpr int kBlock = 256;

/// Renders a fixed stereo signal through a prepared processor, left then right.
template <typename Processor>
std::vector<float> render(Processor& processor) {
  constexpr int kSamples = 2048;
  std::vector<float> left(kSamples);
  std::vector<float> right(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    left[static_cast<std::size_t>(i)] = 0.5f * std::sin(sonare::constants::kTwoPi * 330.0f * t) +
                                        0.25f * std::sin(sonare::constants::kTwoPi * 1250.0f * t);
    right[static_cast<std::size_t>(i)] = 0.8f * left[static_cast<std::size_t>(i)];
  }
  for (int offset = 0; offset < kSamples; offset += kBlock) {
    float* channels[2] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, kBlock);
  }
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

/// Constructing with @p config must throw InvalidParameter naming @p field.
template <typename Processor, typename Config>
void require_refused(const char* name, const char* field, const Config& config, float value) {
  INFO(name << "." << field << " = " << value);
  bool threw = false;
  try {
    Processor processor(config);
  } catch (const sonare::SonareException& error) {
    threw = true;
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    const std::string message = error.what();
    CHECK(message.find(name) != std::string::npos);
    CHECK(message.find(field) != std::string::npos);
    CHECK(message.find("out of range") != std::string::npos);
  }
  CHECK(threw);
}

/// Checks one row against @p base, which must hold every field inside its domain
/// and differ from both bounds of the row wherever the field is audible.
template <typename Processor, typename Config>
void check_row(const char* name, const Config& base, const DomainRow<Config>& row) {
  INFO(name << "." << row.field);
  const auto with = [&](float value) {
    Config config = base;
    config.*(row.member) = value;
    return config;
  };
  const auto render_after = [&](const Config& config, auto&& mutate) {
    Processor processor(config);
    processor.prepare(kRate, kBlock);
    mutate(processor);
    return render(processor);
  };
  const auto untouched = [](Processor&) {};

  std::vector<float> outside = {std::numeric_limits<float>::quiet_NaN(), kInf, -kInf};
  if (std::isfinite(row.lo)) outside.push_back(std::nextafter(row.lo, -kInf));
  if (std::isfinite(row.hi)) outside.push_back(std::nextafter(row.hi, kInf));
  std::vector<float> inside;
  if (std::isfinite(row.lo)) inside.push_back(row.lo);
  if (std::isfinite(row.hi)) inside.push_back(row.hi);

  for (const float value : outside) {
    require_refused<Processor>(name, row.field, with(value), value);
  }
  for (const float value : inside) {
    INFO("bound " << value);
    CHECK_NOTHROW(Processor(with(value)));
  }

  if (row.id < 0) return;
  const auto id = static_cast<unsigned int>(row.id);
  const std::vector<float> control = render_after(base, untouched);
  for (const float value : outside) {
    INFO("live refused " << value);
    const std::vector<float> after = render_after(
        base, [&](Processor& processor) { CHECK_FALSE(processor.set_parameter(id, value)); });
    CHECK(after == control);
  }
  for (const float value : inside) {
    INFO("live bound " << value);
    const std::vector<float> expected = render_after(with(value), untouched);
    const std::vector<float> actual = render_after(
        base, [&](Processor& processor) { CHECK(processor.set_parameter(id, value)); });
    CHECK(actual == expected);
    if (value != base.*(row.member)) CHECK(actual != control);
  }
}

}  // namespace sonare::test::modulation_domain
