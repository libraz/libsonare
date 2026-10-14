/// @file flanger_test.cpp
/// @brief Lifecycle regressions for the shared flanger pre-filter.

#include "effects/modulation/flanger.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using sonare::effects::modulation::Flanger;
using sonare::effects::modulation::FlangerConfig;
using sonare::effects::modulation::PreFilterMode;

constexpr int kPrepareBlock = 127;
constexpr int kWarmSamples = 4800;
constexpr int kGapSamples = 48000;
constexpr int kReentrySamples = 9600;

void process_blocks(Flanger& processor, float* const* channels, int num_channels, int samples) {
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

float difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float out = 0.0f;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    out = std::max(out, std::abs(lhs[i] - rhs[i]));
  }
  return out;
}

FlangerConfig config() {
  FlangerConfig value;
  value.rate_hz = 0.0f;
  value.depth_ms = 0.0f;
  value.center_delay_ms = 3.0f;
  value.feedback = 0.0f;
  value.dry_wet = 1.0f;
  value.pre_filter_hz = 100.0f;
  value.pre_filter_mode = PreFilterMode::kLowPass;
  return value;
}

std::vector<float> reentry(double sample_rate) {
  Flanger processor(config());
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples), 0.1f);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  REQUIRE(processor.set_parameter(8, 0.0f));
  std::vector<float> gap(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap_channels[] = {gap.data(), gap.data()};
  process_blocks(processor, gap_channels, 1, kGapSamples);
  REQUIRE(processor.set_parameter(8, 1.0f));
  std::vector<float> output(static_cast<std::size_t>(kReentrySamples), 0.0f);
  float* reentry_channels[] = {output.data(), output.data()};
  process_blocks(processor, reentry_channels, 1, kReentrySamples);
  return output;
}

std::vector<float> control(double sample_rate, bool reset_before_reentry) {
  Flanger processor(config());
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples), 0.1f);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  if (reset_before_reentry) processor.reset();
  std::vector<float> gap(static_cast<std::size_t>(kGapSamples), 0.0f);
  float* gap_channels[] = {gap.data(), gap.data()};
  process_blocks(processor, gap_channels, 1, kGapSamples);
  std::vector<float> output(static_cast<std::size_t>(kReentrySamples), 0.0f);
  float* reentry_channels[] = {output.data(), output.data()};
  process_blocks(processor, reentry_channels, 1, kReentrySamples);
  return output;
}

}  // namespace

TEST_CASE("flanger pre-filter re-entry drops stale charge at 48 and 96 kHz", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const std::vector<float> subject = reentry(sample_rate);
    const std::vector<float> continuous = control(sample_rate, false);
    const std::vector<float> reset = control(sample_rate, true);
    CAPTURE(sample_rate);
    REQUIRE(peak(subject) <= 1.0e-5f);
    REQUIRE(peak(continuous) <= 1.0e-5f);
    REQUIRE(peak(reset) <= 1.0e-5f);
    REQUIRE(difference(subject, continuous) <= 2.0e-5f);
  }
}
