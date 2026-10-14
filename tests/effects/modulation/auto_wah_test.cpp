/// @file auto_wah_test.cpp
/// @brief Tail-boundary regressions for the envelope wah.

#include "effects/modulation/auto_wah.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "rt/tail_budget.h"
#include "util/constants.h"

namespace {

using sonare::effects::modulation::AutoWah;
using sonare::effects::modulation::AutoWahConfig;
using sonare::effects::modulation::AutoWahDirection;

constexpr int kSampleRate = 8000;
constexpr int kPrepareBlock = 64;
constexpr int kGuardSamples = 4096;

AutoWahConfig high_endpoint_config(float max_hz) {
  AutoWahConfig config;
  config.sensitivity = 0.0f;
  config.min_hz = 300.0f;
  config.max_hz = max_hz;
  config.resonance = 4.0f;
  config.dry_wet = 1.0f;
  config.lfo_rate_hz = 0.0f;
  config.lfo_depth = 0.0f;
  config.direction = AutoWahDirection::kDown;
  return config;
}

std::vector<float> render_impulse(AutoWah& processor, int samples) {
  std::vector<float> output(static_cast<std::size_t>(samples), 0.0f);
  output.front() = 1.0f;
  for (int offset = 0; offset < samples;) {
    const int count = std::min(kPrepareBlock, samples - offset);
    float* channels[] = {output.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
  }
  return output;
}

std::vector<float> render_tone_then_silence(AutoWah& processor, int tone_samples,
                                            int tail_samples) {
  const int total = tone_samples + tail_samples + kGuardSamples;
  std::vector<float> output(static_cast<std::size_t>(total), 0.0f);
  for (int i = 0; i < tone_samples; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
    output[static_cast<std::size_t>(i)] =
        0.3f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 3920.0 * t));
  }
  for (int offset = 0; offset < total;) {
    const int count = std::min(kPrepareBlock, total - offset);
    float* channels[] = {output.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
  }
  return output;
}

float max_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float difference = 0.0f;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    difference = std::max(difference, std::abs(lhs[i] - rhs[i]));
  }
  return difference;
}

int last_above(const std::vector<float>& samples, float floor) {
  int last = -1;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (std::abs(samples[i]) > floor) last = static_cast<int>(i);
  }
  return last;
}

float peak_after(const std::vector<float>& samples, int first) {
  float peak = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

}  // namespace

TEST_CASE("auto-wah reports the high-cutoff impulse tail", "[boundary]") {
  // At and above Nyquist, requests clamp to the same 3920 Hz endpoint.
  for (const float requested_max : {3800.0f, 3920.0f, 4000.0f, 8000.0f}) {
    AutoWah processor(high_endpoint_config(requested_max));
    processor.prepare(kSampleRate, kPrepareBlock);
    const int declared_tail = processor.tail_samples();
    CAPTURE(requested_max, declared_tail);
    REQUIRE(declared_tail > 0);
    REQUIRE(declared_tail < kGuardSamples);

    const std::vector<float> output = render_impulse(processor, declared_tail + kGuardSamples);
    for (const float sample : output) REQUIRE(std::isfinite(sample));

    const int last = last_above(output, static_cast<float>(sonare::rt::kTailFloor));
    INFO("last impulse sample above tail floor: " << last);
    REQUIRE(last >= 0);
    REQUIRE(last < declared_tail);
    REQUIRE(peak_after(output, declared_tail) <= static_cast<float>(sonare::rt::kTailFloor));
  }
}

TEST_CASE("auto-wah clamps reversed endpoints and keeps the ordinary-rate bound", "[boundary]") {
  AutoWahConfig forward_config = high_endpoint_config(3920.0f);
  AutoWahConfig reverse_config = forward_config;
  std::swap(reverse_config.min_hz, reverse_config.max_hz);

  AutoWah forward(forward_config);
  AutoWah reverse(reverse_config);
  forward.prepare(kSampleRate, kPrepareBlock);
  reverse.prepare(kSampleRate, kPrepareBlock);
  REQUIRE(forward.tail_samples() == reverse.tail_samples());
  const int length = forward.tail_samples() + kGuardSamples;
  const std::vector<float> forward_output = render_impulse(forward, length);
  const std::vector<float> reverse_output = render_impulse(reverse, length);
  REQUIRE(max_difference(forward_output, reverse_output) <= 2.0e-6f);

  AutoWah ordinary(high_endpoint_config(2500.0f));
  ordinary.prepare(48000, kPrepareBlock);
  const int declared_tail = ordinary.tail_samples();
  CAPTURE(declared_tail);
  REQUIRE(declared_tail > 0);
  const std::vector<float> ordinary_output =
      render_impulse(ordinary, declared_tail + kGuardSamples);
  for (const float sample : ordinary_output) REQUIRE(std::isfinite(sample));
  REQUIRE(last_above(ordinary_output, static_cast<float>(sonare::rt::kTailFloor)) < declared_tail);
}

TEST_CASE("auto-wah high endpoint tone release fits the declared tail", "[boundary]") {
  AutoWah processor(high_endpoint_config(3920.0f));
  processor.prepare(kSampleRate, kPrepareBlock);
  const int declared_tail = processor.tail_samples();
  constexpr int kToneSamples = 1600;
  const std::vector<float> output =
      render_tone_then_silence(processor, kToneSamples, declared_tail);
  for (const float sample : output) REQUIRE(std::isfinite(sample));
  const float release_floor = 3.0e-6f;
  REQUIRE(last_above(output, release_floor) < kToneSamples + declared_tail);
  REQUIRE(peak_after(output, kToneSamples + declared_tail) <= release_floor);
}

namespace {

constexpr int kLifecycleRateBlock = 127;
constexpr int kLifecycleWarm = 4800;
constexpr int kLifecycleGap = 48000;
constexpr int kLifecycleReentry = 9600;
constexpr double kLifecycleToneHz = 500.0;
constexpr float kLifecycleToneAmplitude = 0.2f;

void fill_left_tone(std::vector<float>& samples, int first_sample, double sample_rate) {
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double time = static_cast<double>(first_sample + static_cast<int>(i)) / sample_rate;
    samples[i] = kLifecycleToneAmplitude *
                 static_cast<float>(std::sin(sonare::constants::kTwoPiD * kLifecycleToneHz * time));
  }
}

template <typename Processor>
void process_lifecycle(Processor& processor, float* const* channels, int num_channels,
                       int samples) {
  int offset = 0;
  for (const int requested : {1, 7, 31, 127}) {
    if (offset >= samples) break;
    const int count = std::min(requested, samples - offset);
    float* block[2] = {channels[0] + offset, num_channels > 1 ? channels[1] + offset : nullptr};
    processor.process(block, num_channels, count);
    offset += count;
  }
  while (offset < samples) {
    const int count = std::min(kLifecycleRateBlock, samples - offset);
    float* block[2] = {channels[0] + offset, num_channels > 1 ? channels[1] + offset : nullptr};
    processor.process(block, num_channels, count);
    offset += count;
  }
}

float lifecycle_peak(const std::vector<float>& samples) {
  float out = 0.0f;
  for (const float sample : samples) out = std::max(out, std::abs(sample));
  return out;
}

float lifecycle_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float out = 0.0f;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    out = std::max(out, std::abs(lhs[i] - rhs[i]));
  }
  return out;
}

AutoWahConfig lifecycle_config() {
  AutoWahConfig config;
  config.sensitivity = 0.0f;
  config.min_hz = 500.0f;
  config.max_hz = 500.0f;
  config.resonance = 4.0f;
  config.dry_wet = 1.0f;
  config.lfo_rate_hz = 0.0f;
  config.lfo_depth = 0.0f;
  config.direction = AutoWahDirection::kDown;
  return config;
}

struct LifecycleOutput {
  std::vector<float> left;
  std::vector<float> right;
};

LifecycleOutput layout_reentry(double sample_rate) {
  AutoWah processor(lifecycle_config());
  processor.prepare(sample_rate, kLifecycleRateBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kLifecycleWarm));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kLifecycleWarm), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_lifecycle(processor, warm, 2, kLifecycleWarm);
  std::vector<float> gap(static_cast<std::size_t>(kLifecycleGap));
  fill_left_tone(gap, kLifecycleWarm, sample_rate);
  float* mono[] = {gap.data(), nullptr};
  process_lifecycle(processor, mono, 1, kLifecycleGap);
  LifecycleOutput output{std::vector<float>(static_cast<std::size_t>(kLifecycleReentry)),
                         std::vector<float>(static_cast<std::size_t>(kLifecycleReentry), 0.0f)};
  fill_left_tone(output.left, kLifecycleWarm + kLifecycleGap, sample_rate);
  float* stereo[] = {output.left.data(), output.right.data()};
  process_lifecycle(processor, stereo, 2, kLifecycleReentry);
  return output;
}

LifecycleOutput continuous(double sample_rate, bool reset_before_gap) {
  AutoWah processor(lifecycle_config());
  processor.prepare(sample_rate, kLifecycleRateBlock);
  std::vector<float> warm_l(static_cast<std::size_t>(kLifecycleWarm));
  fill_left_tone(warm_l, 0, sample_rate);
  std::vector<float> warm_r(static_cast<std::size_t>(kLifecycleWarm), 0.1f);
  float* warm[] = {warm_l.data(), warm_r.data()};
  process_lifecycle(processor, warm, 2, kLifecycleWarm);
  if (reset_before_gap) processor.reset();
  std::vector<float> gap_l(static_cast<std::size_t>(kLifecycleGap));
  fill_left_tone(gap_l, kLifecycleWarm, sample_rate);
  std::vector<float> gap_r(static_cast<std::size_t>(kLifecycleGap), 0.0f);
  float* gap[] = {gap_l.data(), gap_r.data()};
  process_lifecycle(processor, gap, 2, kLifecycleGap);
  LifecycleOutput output{std::vector<float>(static_cast<std::size_t>(kLifecycleReentry)),
                         std::vector<float>(static_cast<std::size_t>(kLifecycleReentry), 0.0f)};
  fill_left_tone(output.left, kLifecycleWarm + kLifecycleGap, sample_rate);
  float* stereo[] = {output.left.data(), output.right.data()};
  process_lifecycle(processor, stereo, 2, kLifecycleReentry);
  return output;
}

}  // namespace

TEST_CASE("auto-wah drops inactive right-plane history on mono re-entry", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const LifecycleOutput subject = layout_reentry(sample_rate);
    const LifecycleOutput control = continuous(sample_rate, false);
    const LifecycleOutput reset = continuous(sample_rate, true);
    CAPTURE(sample_rate);
    REQUIRE(lifecycle_peak(subject.left) > 1.0e-3f);
    REQUIRE(lifecycle_peak(control.left) > 1.0e-3f);
    REQUIRE(lifecycle_peak(reset.left) > 1.0e-3f);
    REQUIRE(lifecycle_peak(subject.right) <= 1.0e-5f);
    REQUIRE(lifecycle_peak(control.right) <= 1.0e-5f);
    REQUIRE(lifecycle_peak(reset.right) <= 1.0e-5f);
    REQUIRE(lifecycle_difference(subject.left, control.left) <= 2.0e-5f);
    REQUIRE(lifecycle_difference(subject.right, control.right) <= 2.0e-5f);
  }
}
