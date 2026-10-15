/// @file rotary_test.cpp
/// @brief Model and channel-layout lifecycle regressions for Rotary.

#include "effects/modulation/rotary.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::effects::modulation::RotaryModel;

constexpr int kPrepareBlock = 127;
constexpr int kWarmSamples = 4800;
constexpr int kGapSamples = 48000;
constexpr int kReentrySamples = 9600;
constexpr int kRightSettledSamples = 512;
constexpr double kLifecycleToneHz = 440.0;
constexpr float kLifecycleToneAmplitude = 0.1f;

void fill_left_tone(std::vector<float>& samples, int first_sample, double sample_rate,
                    float amplitude = kLifecycleToneAmplitude) {
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double time = static_cast<double>(first_sample + static_cast<int>(i)) / sample_rate;
    samples[i] = amplitude *
                 static_cast<float>(std::sin(sonare::constants::kTwoPiD * kLifecycleToneHz * time));
  }
}

void process_blocks(Rotary& processor, float* const* channels, int num_channels, int samples) {
  int offset = 0;
  for (const int requested : {1, 7, 31, 127}) {
    if (offset >= samples) break;
    const int count = std::min(requested, samples - offset);
    float* block[2] = {channels[0] + offset, num_channels > 1 ? channels[1] + offset : nullptr};
    processor.process(block, num_channels, count);
    offset += count;
  }
  while (offset < samples) {
    const int count = std::min(kPrepareBlock, samples - offset);
    float* block[2] = {channels[0] + offset, num_channels > 1 ? channels[1] + offset : nullptr};
    processor.process(block, num_channels, count);
    offset += count;
  }
}

float peak(const std::vector<float>& samples) {
  float out = 0.0f;
  for (const float sample : samples) out = std::max(out, std::abs(sample));
  return out;
}

float difference(const std::vector<float>& lhs, const std::vector<float>& rhs, int first = 0) {
  REQUIRE(lhs.size() == rhs.size());
  float out = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < lhs.size(); ++i) {
    out = std::max(out, std::abs(lhs[i] - rhs[i]));
  }
  return out;
}

RotaryConfig config(RotaryModel model) {
  RotaryConfig value;
  value.model = model;
  value.rate_hz = 0.0f;
  value.drum_rate_hz = 0.0f;
  value.depth_ms = 1.2f;
  value.tremolo = 0.5f;
  value.dry_wet = 1.0f;
  value.horn_level_db = -12.0f;
  return value;
}

struct StereoOutput {
  std::vector<float> left;
  std::vector<float> right;
};

StereoOutput model_reentry(double sample_rate) {
  Rotary processor(config(RotaryModel::kClassic));
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  REQUIRE(processor.set_parameter(13, static_cast<float>(RotaryModel::kGeometric)));
  std::vector<float> gap_l(static_cast<std::size_t>(kGapSamples));
  fill_left_tone(gap_l, kWarmSamples, sample_rate);
  float* gap[] = {gap_l.data(), nullptr};
  process_blocks(processor, gap, 1, kGapSamples);
  REQUIRE(processor.set_parameter(13, static_cast<float>(RotaryModel::kClassic)));
  StereoOutput output{std::vector<float>(static_cast<std::size_t>(kReentrySamples)),
                      std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  fill_left_tone(output.left, kWarmSamples + kGapSamples, sample_rate);
  float* reentry[] = {output.left.data(), output.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return output;
}

StereoOutput model_control(double sample_rate, bool reset_before_reentry) {
  Rotary processor(config(RotaryModel::kClassic));
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  REQUIRE(processor.set_parameter(13, static_cast<float>(RotaryModel::kGeometric)));
  std::vector<float> gap_l(static_cast<std::size_t>(kGapSamples));
  fill_left_tone(gap_l, kWarmSamples, sample_rate, 2.0f * kLifecycleToneAmplitude);
  std::vector<float> gap_r(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap[] = {gap_l.data(), gap_r.data()};
  process_blocks(processor, gap, 2, kGapSamples);
  REQUIRE(processor.set_parameter(13, static_cast<float>(RotaryModel::kClassic)));
  if (reset_before_reentry) processor.reset();
  StereoOutput output{std::vector<float>(static_cast<std::size_t>(kReentrySamples)),
                      std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  fill_left_tone(output.left, kWarmSamples + kGapSamples, sample_rate);
  float* reentry[] = {output.left.data(), output.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return output;
}

StereoOutput layout_reentry(double sample_rate, RotaryModel model) {
  Rotary processor(config(model));
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  std::vector<float> gap(static_cast<std::size_t>(kGapSamples));
  fill_left_tone(gap, kWarmSamples, sample_rate);
  float* mono[] = {gap.data(), nullptr};
  process_blocks(processor, mono, 1, kGapSamples);
  StereoOutput output{std::vector<float>(static_cast<std::size_t>(kReentrySamples)),
                      std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  fill_left_tone(output.left, kWarmSamples + kGapSamples, sample_rate);
  float* reentry[] = {output.left.data(), output.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return output;
}

StereoOutput layout_control(double sample_rate, RotaryModel model, bool reset_before_reentry) {
  Rotary processor(config(model));
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  if (reset_before_reentry) processor.reset();
  std::vector<float> gap_l(static_cast<std::size_t>(kGapSamples));
  // Geometric sums active planes before its shared rotor. Doubling the left
  // control during the stereo gap matches the subject's mono shared feed while
  // keeping the control's right input at zero for an independent-plane check.
  const float gap_amplitude =
      model == RotaryModel::kGeometric ? 2.0f * kLifecycleToneAmplitude : kLifecycleToneAmplitude;
  fill_left_tone(gap_l, kWarmSamples, sample_rate, gap_amplitude);
  std::vector<float> gap_r(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap[] = {gap_l.data(), gap_r.data()};
  process_blocks(processor, gap, 2, kGapSamples);
  StereoOutput output{std::vector<float>(static_cast<std::size_t>(kReentrySamples)),
                      std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  fill_left_tone(output.left, kWarmSamples + kGapSamples, sample_rate);
  float* reentry[] = {output.left.data(), output.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return output;
}

}  // namespace

TEST_CASE("rotary model re-entry clears Classic history at 48 and 96 kHz", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const StereoOutput subject = model_reentry(sample_rate);
    const StereoOutput control = model_control(sample_rate, false);
    const StereoOutput reset = model_control(sample_rate, true);
    CAPTURE(sample_rate);
    REQUIRE(peak(subject.left) > 1.0e-3f);
    REQUIRE(peak(control.left) > 1.0e-3f);
    REQUIRE(peak(reset.left) > 1.0e-3f);
    REQUIRE(peak(subject.right) <= 1.0e-5f);
    REQUIRE(peak(control.right) <= 1.0e-5f);
    REQUIRE(peak(reset.right) <= 1.0e-5f);
    REQUIRE(difference(subject.left, control.left) <= 2.0e-5f);
    REQUIRE(difference(subject.right, control.right) <= 2.0e-5f);
  }
}

TEST_CASE("rotary Classic and Geometric clear the returning right plane", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    for (const RotaryModel model : {RotaryModel::kClassic, RotaryModel::kGeometric}) {
      const StereoOutput subject = layout_reentry(sample_rate, model);
      const StereoOutput control = layout_control(sample_rate, model, false);
      const StereoOutput reset = layout_control(sample_rate, model, true);
      CAPTURE(sample_rate, model);
      REQUIRE(peak(subject.left) > 1.0e-3f);
      REQUIRE(peak(control.left) > 1.0e-3f);
      REQUIRE(peak(reset.left) > 1.0e-3f);
      if (model == RotaryModel::kClassic) {
        REQUIRE(peak(subject.right) <= 1.0e-5f);
        REQUIRE(peak(control.right) <= 1.0e-5f);
        REQUIRE(peak(reset.right) <= 1.0e-5f);
      } else {
        REQUIRE(peak(subject.right) > 1.0e-3f);
        REQUIRE(peak(control.right) > 1.0e-3f);
        REQUIRE(peak(reset.right) > 1.0e-3f);
        REQUIRE(difference(subject.right, control.right) > 1.0e-3f);
        REQUIRE(difference(subject.right, control.right, kRightSettledSamples) <= 2.0e-5f);
      }
      REQUIRE(difference(subject.left, control.left) <= 2.0e-5f);
      if (model == RotaryModel::kClassic) {
        REQUIRE(difference(subject.right, control.right) <= 2.0e-5f);
      }
    }
  }
}

namespace {

using sonare::ErrorCode;
using sonare::SonareException;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kDomainBlock = 2048;

/// One numeric field of RotaryConfig with its automation id and closed domain.
struct DomainRow {
  const char* name;
  float RotaryConfig::*member;
  unsigned int id;
  float lo;
  float hi;
  float base_speed;  ///< the speed switch the base configuration needs for the field to be audible.
  bool step_fast;    ///< switch to fast between the two blocks so a glide field is exercised.
  bool gain_db;      ///< domain is "finite linear gain" rather than an interval.
};

const DomainRow kDomainRows[] = {
    {"rateHz", &RotaryConfig::rate_hz, 0, 0.0f, kInf, -1.0f, false, false},
    {"depthMs", &RotaryConfig::depth_ms, 1, 0.0f, 20.0f, -1.0f, false, false},
    {"tremolo", &RotaryConfig::tremolo, 2, 0.0f, 1.0f, -1.0f, false, false},
    {"dryWet", &RotaryConfig::dry_wet, 3, 0.0f, 1.0f, -1.0f, false, false},
    {"drumRateHz", &RotaryConfig::drum_rate_hz, 4, 0.0f, kInf, -1.0f, false, false},
    {"hornSlowHz", &RotaryConfig::horn_slow_hz, 5, 0.0f, kInf, 0.0f, false, false},
    {"hornFastHz", &RotaryConfig::horn_fast_hz, 6, 0.0f, kInf, 1.0f, false, false},
    {"drumSlowHz", &RotaryConfig::drum_slow_hz, 7, 0.0f, kInf, 0.0f, false, false},
    {"drumFastHz", &RotaryConfig::drum_fast_hz, 8, 0.0f, kInf, 1.0f, false, false},
    {"speed", &RotaryConfig::speed, 9, -1.0f, 1.0f, -1.0f, false, false},
    {"hornLevelDb", &RotaryConfig::horn_level_db, 10, -kInf, kInf, -1.0f, false, true},
    {"drumLevelDb", &RotaryConfig::drum_level_db, 11, -kInf, kInf, -1.0f, false, true},
    {"stereoSpread", &RotaryConfig::stereo_spread, 14, 0.0f, 1.0f, -1.0f, false, false},
    {"accelTauS", &RotaryConfig::accel_tau_s, 15, 0.0f, kInf, 0.0f, true, false},
    {"decelTauS", &RotaryConfig::decel_tau_s, 16, 0.0f, kInf, 0.0f, true, false},
    {"undershootHz", &RotaryConfig::undershoot_hz, 17, 0.0f, kInf, 0.0f, true, false},
    {"drumUndershootHz", &RotaryConfig::drum_undershoot_hz, 18, 0.0f, kInf, 0.0f, true, false},
};

std::vector<float> domain_refused(const DomainRow& row) {
  std::vector<float> values{kNaN, kInf, -kInf};
  if (row.gain_db) {
    values.push_back(1000.0f);  // finite, but its linear gain overflows a float.
    return values;
  }
  if (std::isfinite(row.lo)) values.push_back(std::nextafter(row.lo, -kInf));
  if (std::isfinite(row.hi)) values.push_back(std::nextafter(row.hi, kInf));
  return values;
}

std::vector<float> domain_accepted(const DomainRow& row) {
  if (row.gain_db) return {-120.0f, 6.0f};
  return {row.lo, std::isfinite(row.hi) ? row.hi : 2.0f};
}

RotaryConfig domain_base(const DomainRow& row) {
  RotaryConfig value;
  value.speed = row.base_speed;
  return value;
}

/// Renders two blocks of a fixed stereo signal; between them, optionally steps the speed switch.
std::vector<float> domain_render(Rotary& rotary, bool step_fast) {
  std::vector<float> out;
  for (int block = 0; block < 2; ++block) {
    std::vector<float> left(static_cast<std::size_t>(kDomainBlock));
    std::vector<float> right(static_cast<std::size_t>(kDomainBlock));
    for (int i = 0; i < kDomainBlock; ++i) {
      const float t = static_cast<float>(block * kDomainBlock + i);
      left[static_cast<std::size_t>(i)] = 0.4f * std::sin(0.05f * t) + 0.1f * std::sin(0.31f * t);
      right[static_cast<std::size_t>(i)] = 0.3f * std::sin(0.07f * t + 1.0f);
    }
    float* channels[] = {left.data(), right.data()};
    rotary.process(channels, 2, kDomainBlock);
    out.insert(out.end(), left.begin(), left.end());
    out.insert(out.end(), right.begin(), right.end());
    if (block == 0 && step_fast) REQUIRE(rotary.set_parameter(9, 1.0f));
  }
  return out;
}

void require_refused_with_field(const RotaryConfig& config, const char* field) {
  try {
    Rotary rotary(config);
    FAIL("construction accepted " << field);
  } catch (const SonareException& error) {
    CHECK(error.code() == ErrorCode::InvalidParameter);
    CHECK(std::string(error.what()).find(field) != std::string::npos);
  }
}

}  // namespace

TEST_CASE("Rotary construction refuses a value outside a field's domain", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : domain_refused(row)) {
      CAPTURE(row.name, bad);
      RotaryConfig config;
      config.*row.member = bad;
      require_refused_with_field(config, row.name);
      CHECK_THROWS_AS(Rotary::validate_config(config), SonareException);
    }
    for (const float good : domain_accepted(row)) {
      CAPTURE(row.name, good);
      RotaryConfig config;
      config.*row.member = good;
      CHECK_NOTHROW(Rotary::validate_config(config));
      CHECK_NOTHROW(Rotary(config));
    }
  }
}

TEST_CASE("Rotary live automation refuses an out-of-domain value and changes nothing",
          "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : domain_refused(row)) {
      CAPTURE(row.name, bad);
      Rotary touched(domain_base(row));
      touched.prepare(48000.0, kDomainBlock);
      CHECK_FALSE(touched.set_parameter(row.id, bad));
      Rotary untouched(domain_base(row));
      untouched.prepare(48000.0, kDomainBlock);
      CHECK(domain_render(touched, row.step_fast) == domain_render(untouched, row.step_fast));
    }
  }
}

TEST_CASE("Rotary live automation applies a value on a bound exactly", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float good : domain_accepted(row)) {
      CAPTURE(row.name, good);
      Rotary live(domain_base(row));
      REQUIRE(live.set_parameter(row.id, good));
      live.prepare(48000.0, kDomainBlock);
      RotaryConfig config = domain_base(row);
      config.*row.member = good;
      Rotary constructed(config);
      constructed.prepare(48000.0, kDomainBlock);
      CHECK(domain_render(live, row.step_fast) == domain_render(constructed, row.step_fast));
    }
  }
}

TEST_CASE("Rotary live rate reaches the rotor", "[effects]") {
  Rotary rotary;
  rotary.prepare(48000.0, kDomainBlock);
  REQUIRE(rotary.set_parameter(0, 3.0f));
  domain_render(rotary, false);
  CHECK(rotary.horn_rate_hz() == 3.0f);
}
