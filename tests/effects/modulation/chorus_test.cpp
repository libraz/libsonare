/// @file chorus_test.cpp
/// @brief Lifecycle regressions for the shared chorus pre-filter.

#include "effects/modulation/chorus.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using sonare::effects::modulation::Chorus;
using sonare::effects::modulation::ChorusConfig;
using sonare::effects::modulation::PreFilterMode;

constexpr int kPrepareBlock = 127;
constexpr int kWarmSamples = 4800;
constexpr int kGapSamples = 48000;
constexpr int kReentrySamples = 9600;

void process_blocks(Chorus& processor, float* const* channels, int num_channels, int samples) {
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

float peak(const std::vector<float>& samples, int first = 0) {
  float out = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < samples.size(); ++i) {
    out = std::max(out, std::abs(samples[i]));
  }
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

ChorusConfig config() {
  ChorusConfig value;
  value.rate_hz = 0.0f;
  value.depth_ms = 0.0f;
  value.center_delay_ms = 20.0f;
  value.feedback = 0.0f;
  value.dry_wet = 1.0f;
  value.pre_filter_hz = 100.0f;
  value.pre_filter_mode = PreFilterMode::kLowPass;
  return value;
}

struct LayoutRun {
  std::vector<float> left;
  std::vector<float> right;
};

LayoutRun run_reentry(double sample_rate, bool bypass_filter) {
  Chorus processor(config());
  processor.prepare(sample_rate, kPrepareBlock);
  LayoutRun result{std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f),
                   std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples), 0.1f);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  REQUIRE(processor.set_parameter(7, bypass_filter ? 0.0f : 1.0f));
  std::vector<float> gap(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap_channels[] = {gap.data(), gap.data()};
  process_blocks(processor, gap_channels, 1, kGapSamples);
  REQUIRE(processor.set_parameter(7, 1.0f));
  float* reentry[] = {result.left.data(), result.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return result;
}

LayoutRun run_control(double sample_rate, bool reset_before_reentry) {
  Chorus processor(config());
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples), 0.1f);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  if (reset_before_reentry) processor.reset();
  std::vector<float> gap(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap_channels[] = {gap.data(), gap.data()};
  process_blocks(processor, gap_channels, 2, kGapSamples);
  LayoutRun result{std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f),
                   std::vector<float>(static_cast<std::size_t>(kReentrySamples), 0.0f)};
  float* reentry[] = {result.left.data(), result.right.data()};
  process_blocks(processor, reentry, 2, kReentrySamples);
  return result;
}

}  // namespace

TEST_CASE("chorus pre-filter re-entry drops stale charge at 48 and 96 kHz", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const LayoutRun subject = run_reentry(sample_rate, true);
    const LayoutRun continuous = run_control(sample_rate, false);
    const LayoutRun reset = run_control(sample_rate, true);
    CAPTURE(sample_rate);
    REQUIRE(peak(subject.left) <= 1.0e-5f);
    REQUIRE(peak(subject.right) <= 1.0e-5f);
    REQUIRE(peak(continuous.right) <= 1.0e-5f);
    REQUIRE(peak(reset.right) <= 1.0e-5f);
    REQUIRE(difference(subject.left, continuous.left) <= 2.0e-5f);
    REQUIRE(difference(subject.right, continuous.right) <= 2.0e-5f);
  }
}
