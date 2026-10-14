/// @file vowel_filter_test.cpp
/// @brief Lifecycle regressions for VowelFilter's stereo planes and alignment paths.

#include "effects/filter/vowel_filter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"

namespace {

using sonare::effects::filter::VowelFilter;
using sonare::effects::filter::VowelFilterConfig;

constexpr int kPrepareBlock = 127;
constexpr int kWarmSamples = 4800;
constexpr int kGapSamples = 48000;
constexpr int kReentrySamples = 9600;
constexpr double kLifecycleToneHz = 440.0;
constexpr float kLifecycleToneAmplitude = 0.1f;

void fill_left_tone(std::vector<float>& samples, int first_sample, double sample_rate) {
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double time = static_cast<double>(first_sample + static_cast<int>(i)) / sample_rate;
    samples[i] = kLifecycleToneAmplitude *
                 static_cast<float>(std::sin(sonare::constants::kTwoPiD * kLifecycleToneHz * time));
  }
}

void process_blocks(VowelFilter& processor, float* const* channels, int num_channels, int samples) {
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

VowelFilterConfig config() {
  VowelFilterConfig value;
  value.vowel = 1.0f;
  value.accel_ms = 0.0f;
  value.drive = 0.5f;
  value.drive_on = true;
  value.dry_wet = 1.0f;
  return value;
}

struct StereoOutput {
  std::vector<float> left;
  std::vector<float> right;
};

StereoOutput layout_reentry(double sample_rate) {
  VowelFilter processor(config());
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

StereoOutput control(double sample_rate, bool reset_before_reentry) {
  VowelFilter processor(config());
  processor.prepare(sample_rate, kPrepareBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kWarmSamples));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kWarmSamples), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_blocks(processor, warm, 2, kWarmSamples);
  if (reset_before_reentry) processor.reset();
  std::vector<float> gap_l(static_cast<std::size_t>(kGapSamples));
  fill_left_tone(gap_l, kWarmSamples, sample_rate);
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

TEST_CASE("vowel filter clears the returning right plane and its alignment path", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const StereoOutput subject = layout_reentry(sample_rate);
    const StereoOutput continuous = control(sample_rate, false);
    const StereoOutput reset = control(sample_rate, true);
    CAPTURE(sample_rate);
    REQUIRE(peak(subject.left) > 1.0e-3f);
    REQUIRE(peak(continuous.left) > 1.0e-3f);
    REQUIRE(peak(reset.left) > 1.0e-3f);
    REQUIRE(peak(subject.right) <= 1.0e-5f);
    REQUIRE(peak(continuous.right) <= 1.0e-5f);
    REQUIRE(peak(reset.right) <= 1.0e-5f);
    REQUIRE(difference(subject.left, continuous.left) <= 2.0e-5f);
    REQUIRE(difference(subject.right, continuous.right) <= 2.0e-5f);
  }
}
