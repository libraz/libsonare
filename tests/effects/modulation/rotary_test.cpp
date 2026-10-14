/// @file rotary_test.cpp
/// @brief Model and channel-layout lifecycle regressions for Rotary.

#include "effects/modulation/rotary.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"

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
