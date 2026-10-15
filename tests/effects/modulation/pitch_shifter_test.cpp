/// @file pitch_shifter_test.cpp
/// @brief Tail-boundary regressions for the realtime pitch shifter.

#include "effects/modulation/pitch_shifter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "effects/common/control_ranges.h"
#include "effects/common/mix_law.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::effects::common::MixLaw;
using sonare::effects::modulation::kDefaultWindowMs;
using sonare::effects::modulation::PitchShifter;
using sonare::effects::modulation::PitchShifterConfig;

constexpr int kPrepareBlock = 64;
constexpr int kWarmMilliseconds = 200;
constexpr int kGuardMilliseconds = 10;

PitchShifterConfig glide_config(MixLaw law) {
  PitchShifterConfig config;
  config.semitones = 7.0f;
  config.dry_wet = 1.0f;
  config.mix_law = law;
  config.anti_alias = true;
  return config;
}

PitchShifterConfig initial_tail_config(bool anti_alias) {
  PitchShifterConfig config;
  config.semitones = 7.0f;
  config.window_ms = 0.5f;
  config.dry_wet = 1.0f;
  config.anti_alias = anti_alias;
  return config;
}

std::vector<float> asymmetric_tone(int samples, int sample_rate) {
  std::vector<float> output(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
    output[static_cast<std::size_t>(i)] = static_cast<float>(0.2 * std::sin(kTwoPiD * 731.0 * t) +
                                                             0.13 * std::cos(kTwoPiD * 503.0 * t));
  }
  return output;
}

void process_chunked(PitchShifter& processor, std::vector<float>& samples) {
  for (int offset = 0; offset < static_cast<int>(samples.size());) {
    const int count = std::min(kPrepareBlock, static_cast<int>(samples.size()) - offset);
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
  }
}

void process_stereo_chunked(PitchShifter& processor, std::vector<float>& left,
                            std::vector<float>& right, int block = kPrepareBlock) {
  REQUIRE(left.size() == right.size());
  for (int offset = 0; offset < static_cast<int>(left.size());) {
    const int count = std::min(block, static_cast<int>(left.size()) - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
    offset += count;
  }
}

void process_mono_chunked(PitchShifter& processor, std::vector<float>& samples,
                          int block = kPrepareBlock) {
  for (int offset = 0; offset < static_cast<int>(samples.size());) {
    const int count = std::min(block, static_cast<int>(samples.size()) - offset);
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
  }
}

std::vector<float> render_silence(PitchShifter& processor, int samples) {
  std::vector<float> output(static_cast<std::size_t>(samples), 0.0f);
  process_chunked(processor, output);
  return output;
}

std::vector<float> render_impulse(PitchShifter& processor, int samples, float amplitude) {
  std::vector<float> output(static_cast<std::size_t>(samples), 0.0f);
  output.front() = amplitude;
  process_chunked(processor, output);
  return output;
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

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (const float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float max_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float difference = 0.0f;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    difference = std::max(difference, std::abs(lhs[i] - rhs[i]));
  }
  return difference;
}

void require_finite(const std::vector<float>& samples) {
  for (const float sample : samples) REQUIRE(std::isfinite(sample));
}

}  // namespace

TEST_CASE("pitch shifter tail includes a pending wet-to-dry glide", "[boundary]") {
  for (const int sample_rate : {8000, 48000, 96000}) {
    for (const MixLaw law : {MixLaw::kCrossfade, MixLaw::kTwoRamps}) {
      PitchShifter processor(glide_config(law));
      processor.prepare(sample_rate, kPrepareBlock);
      std::vector<float> warm =
          asymmetric_tone(sample_rate * kWarmMilliseconds / 1000, sample_rate);
      process_chunked(processor, warm);

      REQUIRE(processor.set_parameter(1, 0.0f));
      const int declared_tail = processor.tail_samples();
      CAPTURE(sample_rate, static_cast<int>(law), declared_tail);
      REQUIRE(declared_tail > 0);

      const int guard = std::max(64, sample_rate * kGuardMilliseconds / 1000);
      const std::vector<float> output = render_silence(processor, declared_tail + guard);
      require_finite(output);
      REQUIRE(peak_abs(output) > 1.0e-4f);

      const float floor = static_cast<float>(sonare::rt::kTailFloor);
      const int last = last_above(output, floor);
      INFO("last pending-glide sample above tail floor: " << last);
      REQUIRE(last >= 0);
      REQUIRE(last < declared_tail);
      REQUIRE(peak_after(output, declared_tail) <= floor);
      REQUIRE(processor.tail_samples() == 0);

      PitchShifter static_dry(glide_config(law));
      REQUIRE(static_dry.set_parameter(1, 0.0f));
      static_dry.prepare(sample_rate, kPrepareBlock);
      REQUIRE(static_dry.tail_samples() == 0);
      const std::vector<float> static_output = render_silence(static_dry, guard);
      require_finite(static_output);
      for (const float sample : static_output) REQUIRE(sample == 0.0f);

      PitchShifter normal_wet(glide_config(law));
      normal_wet.prepare(sample_rate, kPrepareBlock);
      const int normal_tail = normal_wet.tail_samples();
      REQUIRE(normal_tail >= declared_tail);

      PitchShifter tiny_positive(glide_config(law));
      REQUIRE(tiny_positive.set_parameter(1, 1.0e-8f));
      tiny_positive.prepare(sample_rate, kPrepareBlock);
      REQUIRE(tiny_positive.tail_samples() == normal_tail);

      // A partial ramp must report a shorter bound containing its remaining PCM.
      PitchShifter partial(glide_config(law));
      partial.prepare(sample_rate, kPrepareBlock);
      std::vector<float> partial_warm =
          asymmetric_tone(sample_rate * kWarmMilliseconds / 1000, sample_rate);
      process_chunked(partial, partial_warm);
      REQUIRE(partial.set_parameter(1, 0.0f));
      const int full_partial_tail = partial.tail_samples();
      REQUIRE(full_partial_tail == declared_tail);
      const int consumed = std::max(1, full_partial_tail / 2);
      const std::vector<float> consumed_output = render_silence(partial, consumed);
      require_finite(consumed_output);
      const int remaining_tail = partial.tail_samples();
      CAPTURE(remaining_tail);
      REQUIRE(remaining_tail > 0);
      REQUIRE(remaining_tail < full_partial_tail);
      const std::vector<float> remaining_output = render_silence(partial, remaining_tail + guard);
      require_finite(remaining_output);
      REQUIRE(last_above(remaining_output, floor) < remaining_tail);
      REQUIRE(peak_after(remaining_output, remaining_tail) <= floor);
      REQUIRE(partial.tail_samples() == 0);

      // A pending retarget must cover the interrupted glide and revealed history.
      PitchShifter interrupted(glide_config(law));
      interrupted.prepare(sample_rate, kPrepareBlock);
      std::vector<float> interrupted_warm =
          asymmetric_tone(sample_rate * kWarmMilliseconds / 1000, sample_rate);
      process_chunked(interrupted, interrupted_warm);
      REQUIRE(interrupted.set_parameter(1, 0.0f));
      const int interrupted_full = interrupted.tail_samples();
      REQUIRE(interrupted_full > 0);
      const int interrupted_prefix = std::max(1, interrupted_full / 2);
      (void)render_silence(interrupted, interrupted_prefix);
      REQUIRE(interrupted.set_parameter(1, 0.25f));
      const int retargeted_tail = interrupted.tail_samples();
      REQUIRE(retargeted_tail >= interrupted_full);
      const std::vector<float> retargeted_output =
          render_silence(interrupted, retargeted_tail + guard);
      require_finite(retargeted_output);
      REQUIRE(peak_after(retargeted_output, retargeted_tail) <= floor);

      // The opposite direction exposes history that was written while dry.
      PitchShifter reverse(glide_config(law));
      REQUIRE(reverse.set_parameter(1, 0.0f));
      reverse.prepare(sample_rate, kPrepareBlock);
      std::vector<float> dry_history =
          asymmetric_tone(sample_rate * kWarmMilliseconds / 1000, sample_rate);
      process_chunked(reverse, dry_history);
      REQUIRE(reverse.set_parameter(1, 1.0f));
      const int reverse_tail = reverse.tail_samples();
      REQUIRE(reverse_tail > 0);
      const std::vector<float> reverse_output = render_silence(reverse, reverse_tail + guard);
      require_finite(reverse_output);
      REQUIRE(peak_abs(reverse_output) > 1.0e-4f);
      REQUIRE(peak_after(reverse_output, reverse_tail) <= floor);
    }
  }
}

TEST_CASE("pitch shifter initial anti-alias tail covers an impulse", "[boundary]") {
  for (const int sample_rate : {8000, 48000, 96000}) {
    PitchShifterConfig enabled_config = initial_tail_config(true);
    PitchShifter fresh(enabled_config);
    fresh.prepare(sample_rate, kPrepareBlock);
    const int fresh_tail = fresh.tail_samples();
    CAPTURE(sample_rate, fresh_tail);
    REQUIRE(fresh_tail > 0);

    PitchShifter primed(enabled_config);
    primed.prepare(sample_rate, kPrepareBlock);
    std::vector<float> one_zero(1, 0.0f);
    process_chunked(primed, one_zero);
    primed.reset();
    const int reset_tail = primed.tail_samples();
    REQUIRE(reset_tail >= fresh_tail);

    PitchShifterConfig off_config = initial_tail_config(false);
    PitchShifter anti_alias_off(off_config);
    anti_alias_off.prepare(sample_rate, kPrepareBlock);
    const int off_tail = anti_alias_off.tail_samples();
    REQUIRE(off_tail > 0);

    const int render_length = std::max({fresh_tail, reset_tail, off_tail}) + 2048;
    const std::vector<float> fresh_output = render_impulse(fresh, render_length, 0.3f);
    const std::vector<float> reset_output = render_impulse(primed, render_length, 0.3f);
    const std::vector<float> off_output = render_impulse(anti_alias_off, render_length, 0.3f);
    require_finite(fresh_output);
    require_finite(reset_output);
    require_finite(off_output);

    const float floor = 3.0e-6f;
    REQUIRE(peak_abs(fresh_output) > 1.0e-3f);
    REQUIRE(peak_abs(reset_output) > 1.0e-3f);
    REQUIRE(peak_abs(off_output) > 1.0e-3f);
    REQUIRE(last_above(fresh_output, floor) < fresh_tail);
    REQUIRE(last_above(off_output, floor) < off_tail);
    REQUIRE(peak_after(fresh_output, fresh_tail) <= floor);
    REQUIRE(peak_after(off_output, off_tail) <= floor);

    // One zero-valued sample then reset must preserve the fresh impulse output.
    REQUIRE(max_difference(fresh_output, reset_output) <= 2.0e-6f);
  }
}

TEST_CASE("pitch shifter anti-alias metadata follows setters before processing", "[boundary]") {
  PitchShifterConfig final_config;
  final_config.semitones = 7.0f;
  final_config.semitones2 = 12.0f;
  final_config.level2 = 1.0f;
  final_config.mix_law = MixLaw::kTwoRamps;
  final_config.anti_alias = true;

  for (const bool enable_before_pitch : {false, true}) {
    PitchShifter via_setters;
    via_setters.prepare(48000, kPrepareBlock);
    if (enable_before_pitch) REQUIRE(via_setters.set_parameter(11, 1.0f));
    REQUIRE(via_setters.set_parameter(0, final_config.semitones));
    REQUIRE(via_setters.set_parameter(4, final_config.semitones2));
    REQUIRE(via_setters.set_parameter(6, final_config.level2));
    REQUIRE(via_setters.set_parameter(9, static_cast<float>(final_config.mix_law)));
    if (!enable_before_pitch) REQUIRE(via_setters.set_parameter(11, 1.0f));

    PitchShifter fresh(final_config);
    fresh.prepare(48000, kPrepareBlock);
    std::vector<float> reference_zero(1, 0.0f);
    process_chunked(fresh, reference_zero);
    fresh.reset();
    const int expected_tail = fresh.tail_samples();
    const int actual_tail = via_setters.tail_samples();
    CAPTURE(enable_before_pitch, expected_tail, actual_tail);
    REQUIRE(actual_tail == expected_tail);

    const int length = expected_tail + 2048;
    const std::vector<float> fresh_output = render_impulse(fresh, length, 0.3f);
    const std::vector<float> setter_output = render_impulse(via_setters, length, 0.3f);
    require_finite(fresh_output);
    require_finite(setter_output);
    REQUIRE(max_difference(fresh_output, setter_output) <= 2.0e-6f);
  }

  PitchShifter reprepare(final_config);
  reprepare.prepare(8000, kPrepareBlock);
  std::vector<float> dirty = asymmetric_tone(1600, 8000);
  process_chunked(reprepare, dirty);
  reprepare.prepare(48000, kPrepareBlock);

  PitchShifter fresh_after_reprepare(final_config);
  fresh_after_reprepare.prepare(48000, kPrepareBlock);
  REQUIRE(reprepare.tail_samples() == fresh_after_reprepare.tail_samples());
  const int length = fresh_after_reprepare.tail_samples() + 2048;
  const std::vector<float> reprepare_output = render_impulse(reprepare, length, 0.3f);
  const std::vector<float> fresh_output = render_impulse(fresh_after_reprepare, length, 0.3f);
  REQUIRE(max_difference(reprepare_output, fresh_output) <= 2.0e-6f);
}

TEST_CASE("pitch shifter tail grows for a second audible voice", "[boundary]") {
  for (const int sample_rate : {1000, 48000, 96000}) {
    PitchShifterConfig one_config;
    one_config.window_ms = 0.5f;
    one_config.pre_delay_ms = 1000.0f;
    one_config.pre_delay2_ms = 1000.0f;
    one_config.feedback = 0.9f;
    one_config.dry_wet = 1.0f;
    one_config.semitones = 0.0f;
    one_config.semitones2 = 0.0f;
    one_config.level2 = 0.0f;
    PitchShifterConfig two_config = one_config;
    two_config.level2 = 1.0f;

    PitchShifter one_voice(one_config);
    PitchShifter two_voices(two_config);
    one_voice.prepare(sample_rate, kPrepareBlock);
    two_voices.prepare(sample_rate, kPrepareBlock);
    const int one_tail = one_voice.tail_samples();
    const int two_tail = two_voices.tail_samples();
    CAPTURE(sample_rate, one_tail, two_tail);
    REQUIRE(two_tail > one_tail);
  }
}

TEST_CASE("pitch shifter output scale also covers anti-alias decay", "[boundary]") {
  for (const MixLaw law : {MixLaw::kCrossfade, MixLaw::kTwoRamps}) {
    for (const int sample_rate : {48000, 96000}) {
      PitchShifterConfig one_config;
      one_config.semitones = 7.0f;
      one_config.window_ms = 0.5f;
      one_config.pre_delay_ms = 5.0f;
      one_config.pre_delay2_ms = 5.0f;
      one_config.feedback = 0.0f;
      one_config.dry_wet = 1.0f;
      one_config.mix_law = law;
      one_config.anti_alias = true;
      PitchShifterConfig two_config = one_config;
      two_config.level2 = law == MixLaw::kCrossfade ? 1.0f : 0.5f;
      PitchShifter one_voice(one_config);
      PitchShifter two_voices(two_config);
      one_voice.prepare(sample_rate, kPrepareBlock);
      two_voices.prepare(sample_rate, kPrepareBlock);
      CAPTURE(static_cast<int>(law), sample_rate, one_voice.tail_samples(),
              two_voices.tail_samples());
      REQUIRE(two_voices.tail_samples() > one_voice.tail_samples());
    }
  }
}

TEST_CASE("pitch shifter tail accounts for Lagrange interpolation overshoot", "[boundary]") {
  PitchShifterConfig linear_config;
  linear_config.semitones = 7.0f;
  linear_config.level2 = 0.0f;
  linear_config.window_ms = 0.5f;
  linear_config.pre_delay_ms = 5.0f;
  linear_config.pre_delay2_ms = 5.0f;
  linear_config.feedback = 0.9f;
  linear_config.dry_wet = 1.0f;
  PitchShifterConfig lagrange_config = linear_config;
  lagrange_config.interpolation = sonare::effects::modulation::DelayInterpolation::kLagrange3;
  PitchShifter linear(linear_config);
  PitchShifter lagrange(lagrange_config);
  linear.prepare(48000, kPrepareBlock);
  lagrange.prepare(48000, kPrepareBlock);
  CAPTURE(linear.tail_samples(), lagrange.tail_samples());
  REQUIRE(lagrange.tail_samples() > linear.tail_samples());
}

TEST_CASE("pitch shifter preserves an integral unity Lagrange echo train", "[boundary]") {
  PitchShifterConfig linear_config;
  linear_config.window_ms = 0.5f;
  linear_config.pre_delay_ms = 22.5f;
  linear_config.feedback = 0.9f;
  linear_config.dry_wet = 1.0f;
  PitchShifterConfig lagrange_config = linear_config;
  lagrange_config.interpolation = sonare::effects::modulation::DelayInterpolation::kLagrange3;
  PitchShifter linear(linear_config);
  PitchShifter lagrange(lagrange_config);
  linear.prepare(48000, kPrepareBlock);
  lagrange.prepare(48000, kPrepareBlock);
  const int length = std::max(linear.tail_samples(), lagrange.tail_samples()) + 512;
  const std::vector<float> linear_output = render_impulse(linear, length, 0.3f);
  const std::vector<float> lagrange_output = render_impulse(lagrange, length, 0.3f);
  REQUIRE(max_difference(linear_output, lagrange_output) <= 1.0e-7f);
}

TEST_CASE("pitch shifter budgets fractional fixed Lagrange interpolation", "[boundary]") {
  PitchShifterConfig linear_config;
  linear_config.window_ms = 0.5f;
  linear_config.pre_delay_ms = 0.03125f;
  linear_config.feedback = 0.9f;
  linear_config.dry_wet = 1.0f;
  PitchShifterConfig lagrange_config = linear_config;
  lagrange_config.interpolation = sonare::effects::modulation::DelayInterpolation::kLagrange3;
  PitchShifter linear(linear_config);
  PitchShifter lagrange(lagrange_config);
  linear.prepare(48000, kPrepareBlock);
  lagrange.prepare(48000, kPrepareBlock);
  REQUIRE(lagrange.tail_samples() > linear.tail_samples());
}

TEST_CASE("pitch shifter clears an inactive right plane before stereo re-entry", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    PitchShifterConfig config;
    config.semitones = 7.0f;
    config.window_ms = 4.0f;
    config.dry_wet = 1.0f;
    PitchShifter subject(config);
    PitchShifter control(config);
    subject.prepare(sample_rate, 127);
    control.prepare(sample_rate, 127);

    const int warm = sample_rate / 10;
    std::vector<float> subject_left(static_cast<std::size_t>(warm));
    std::vector<float> subject_right(static_cast<std::size_t>(warm));
    std::vector<float> control_left = subject_left;
    std::vector<float> control_right = subject_right;
    for (int i = 0; i < warm; ++i) {
      const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
      subject_left[static_cast<std::size_t>(i)] =
          static_cast<float>(0.03 * std::sin(kTwoPiD * 731.0 * t));
      subject_right[static_cast<std::size_t>(i)] =
          static_cast<float>(0.2 * std::cos(kTwoPiD * 503.0 * t));
      control_left[static_cast<std::size_t>(i)] = subject_left[static_cast<std::size_t>(i)];
      control_right[static_cast<std::size_t>(i)] = subject_right[static_cast<std::size_t>(i)];
    }
    process_stereo_chunked(subject, subject_left, subject_right, 127);
    process_stereo_chunked(control, control_left, control_right, 127);

    const int gap = subject.tail_samples() + 512;
    std::vector<float> subject_gap_left = asymmetric_tone(gap, sample_rate);
    std::vector<float> control_gap_left = subject_gap_left;
    std::vector<float> control_gap_right(static_cast<std::size_t>(gap), 0.0f);
    process_mono_chunked(subject, subject_gap_left, 127);
    process_stereo_chunked(control, control_gap_left, control_gap_right, 127);
    REQUIRE(peak_abs(subject_gap_left) > 1.0e-4f);

    std::vector<float> subject_left_return(512, 0.0f);
    std::vector<float> subject_right_return(512, 0.0f);
    std::vector<float> control_left_return(512, 0.0f);
    std::vector<float> control_right_return(512, 0.0f);
    process_stereo_chunked(subject, subject_left_return, subject_right_return, 127);
    process_stereo_chunked(control, control_left_return, control_right_return, 127);

    CAPTURE(sample_rate, gap, peak_abs(subject_right_return), peak_abs(control_right_return));
    REQUIRE(peak_abs(subject_right_return) <= 1.0e-6f);
    REQUIRE(peak_abs(control_right_return) <= 1.0e-6f);
    REQUIRE(max_difference(subject_left_return, control_left_return) <= 1.0e-6f);
  }
}

namespace {

double rising_zero_crossing_hz(const std::vector<float>& samples, double sample_rate,
                               int first_sample) {
  std::vector<double> crossings;
  crossings.reserve(samples.size() / 20);
  for (int i = std::max(1, first_sample); i < static_cast<int>(samples.size()); ++i) {
    const float previous = samples[static_cast<std::size_t>(i - 1)];
    const float current = samples[static_cast<std::size_t>(i)];
    if (!(previous < 0.0f && current >= 0.0f)) continue;
    const double denominator = static_cast<double>(current) - previous;
    const double fraction = denominator != 0.0 ? -static_cast<double>(previous) / denominator : 0.0;
    crossings.push_back(static_cast<double>(i - 1) + fraction);
  }
  REQUIRE(crossings.size() > 100);
  return static_cast<double>(crossings.size() - 1) * sample_rate /
         (crossings.back() - crossings.front());
}

std::vector<float> pitch_tone(double sample_rate, int samples, double frequency_hz) {
  std::vector<float> output(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    output[static_cast<std::size_t>(i)] = static_cast<float>(
        0.2 * std::sin(kTwoPiD * frequency_hz * static_cast<double>(i) / sample_rate));
  }
  return output;
}

}  // namespace

TEST_CASE("pitch shifter keeps supported fine clocks moving at long grain windows", "[boundary]") {
  struct Case {
    int sample_rate;
    float cents;
    float window_ms;
    double expected_hz;
  };
  const std::vector<Case> cases = {
      {48000, 0.5f, 128.0f, 1000.0 * std::exp2(0.5 / 1200.0)},
      {96000, 1.0f, 128.0f, 1000.0 * std::exp2(1.0 / 1200.0)},
      {96000, -1.0f, 128.0f, 1000.0 * std::exp2(-1.0 / 1200.0)},
      {96000, 1.0f, kDefaultWindowMs, 1000.0 * std::exp2(1.0 / 1200.0)},
      {96000, 10.0f, 1000.0f, 1000.0 * std::exp2(10.0 / 1200.0)},
  };
  for (const Case& test : cases) {
    const int samples = test.sample_rate * 8;
    PitchShifterConfig config;
    config.dry_wet = 1.0f;
    config.window_ms = test.window_ms;
    config.cents = test.cents;
    PitchShifter shifter(config);
    shifter.prepare(test.sample_rate, kPrepareBlock);
    auto output = pitch_tone(test.sample_rate, samples, 1000.0);
    process_mono_chunked(shifter, output, 127);
    const double measured = rising_zero_crossing_hz(output, test.sample_rate, test.sample_rate * 2);
    CAPTURE(test.sample_rate, test.cents, test.window_ms, measured, test.expected_hz);
    REQUIRE(std::abs(measured - test.expected_hz) < 0.05);
  }
}

TEST_CASE("pitch shifter applies a fine live update to either voice clock", "[boundary]") {
  constexpr int sample_rate = 96000;
  constexpr int samples = sample_rate * 8;

  PitchShifter first_voice;
  first_voice.prepare(sample_rate, kPrepareBlock);
  auto first = pitch_tone(sample_rate, samples, 1000.0);
  process_mono_chunked(first_voice, first, 127);
  REQUIRE(first_voice.set_parameter(2, 1.0f));
  auto first_tail = pitch_tone(sample_rate, sample_rate * 7, 1000.0);
  process_mono_chunked(first_voice, first_tail, 127);
  const double first_measured = rising_zero_crossing_hz(first_tail, sample_rate, sample_rate * 2);
  const double expected = 1000.0 * std::exp2(1.0 / 1200.0);
  CAPTURE(first_measured, expected);
  REQUIRE(std::abs(first_measured - expected) < 0.05);

  PitchShifterConfig second_config;
  second_config.dry_wet = 1.0f;
  second_config.level2 = 1.0f;
  second_config.pan = 1.0f;
  second_config.pan2 = -1.0f;
  PitchShifter second_voice(second_config);
  second_voice.prepare(sample_rate, kPrepareBlock);
  std::vector<float> left = pitch_tone(sample_rate, samples, 1000.0);
  std::vector<float> right = left;
  process_stereo_chunked(second_voice, left, right, 127);
  REQUIRE(second_voice.set_parameter(5, 1.0f));
  left = pitch_tone(sample_rate, sample_rate * 7, 1000.0);
  right = left;
  process_stereo_chunked(second_voice, left, right, 127);
  const double second_measured = rising_zero_crossing_hz(left, sample_rate, sample_rate * 2);
  CAPTURE(second_measured);
  REQUIRE(std::abs(second_measured - expected) < 0.05);
}

TEST_CASE("pitch shifter re-prepared at a new rate rebuilds its anti-alias section", "[boundary]") {
  PitchShifterConfig config;
  config.semitones = 7.0f;
  config.dry_wet = 1.0f;
  config.anti_alias = true;

  PitchShifter reprepared(config);
  reprepared.prepare(44100, kPrepareBlock);
  std::vector<float> dirty = asymmetric_tone(4410, 44100);
  process_chunked(reprepared, dirty);
  reprepared.prepare(48000, kPrepareBlock);

  PitchShifter fresh(config);
  fresh.prepare(48000, kPrepareBlock);
  REQUIRE(reprepared.tail_samples() == fresh.tail_samples());
  const int length = fresh.tail_samples() + 2048;
  const std::vector<float> reprepared_output = render_impulse(reprepared, length, 0.3f);
  const std::vector<float> fresh_output = render_impulse(fresh, length, 0.3f);
  require_finite(reprepared_output);
  REQUIRE(max_difference(reprepared_output, fresh_output) == 0.0f);
}

namespace {

using sonare::ErrorCode;
using sonare::SonareException;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kDomainBlock = 2048;
constexpr double kDomainRate = 48000.0;

/// One numeric field of PitchShifterConfig with its automation id and closed domain.
struct DomainRow {
  const char* name;
  float PitchShifterConfig::*member;
  unsigned int id;
  float lo;
  float hi;
};

const DomainRow kDomainRows[] = {
    {"semitones", &PitchShifterConfig::semitones, 0, -24.0f, 24.0f},
    {"dryWet", &PitchShifterConfig::dry_wet, 1, 0.0f, 1.0f},
    {"cents", &PitchShifterConfig::cents, 2, -100.0f, 100.0f},
    {"pan", &PitchShifterConfig::pan, 3, -1.0f, 1.0f},
    {"semitones2", &PitchShifterConfig::semitones2, 4, -24.0f, 24.0f},
    {"cents2", &PitchShifterConfig::cents2, 5, -100.0f, 100.0f},
    {"level2", &PitchShifterConfig::level2, 6, 0.0f, 1.0f},
    {"pan2", &PitchShifterConfig::pan2, 7, -1.0f, 1.0f},
    {"feedback", &PitchShifterConfig::feedback, 8, -sonare::effects::common::kMaxFeedback,
     sonare::effects::common::kMaxFeedback},
    {"windowMs", &PitchShifterConfig::window_ms, 12, 0.5f, 1000.0f},
    {"preDelayMs", &PitchShifterConfig::pre_delay_ms, 13, 0.0f, 1000.0f},
    {"preDelay2Ms", &PitchShifterConfig::pre_delay2_ms, 14, 0.0f, 1000.0f},
};

std::vector<float> domain_refused(const DomainRow& row) {
  return {kNaN, kInf, -kInf, std::nextafter(row.lo, -kInf), std::nextafter(row.hi, kInf)};
}

PitchShifterConfig domain_base() {
  PitchShifterConfig config;
  config.semitones = 5.0f;
  config.semitones2 = -3.0f;
  config.level2 = 0.5f;
  config.dry_wet = 0.8f;
  return config;
}

std::vector<float> domain_render(PitchShifter& shifter) {
  std::vector<float> out;
  for (int block = 0; block < 4; ++block) {
    std::vector<float> left(static_cast<std::size_t>(kDomainBlock));
    std::vector<float> right(static_cast<std::size_t>(kDomainBlock));
    for (int i = 0; i < kDomainBlock; ++i) {
      const float t = static_cast<float>(block * kDomainBlock + i);
      left[static_cast<std::size_t>(i)] = 0.4f * std::sin(0.05f * t) + 0.1f * std::sin(0.31f * t);
      right[static_cast<std::size_t>(i)] = 0.3f * std::sin(0.07f * t + 1.0f);
    }
    float* channels[] = {left.data(), right.data()};
    shifter.process(channels, 2, kDomainBlock);
    out.insert(out.end(), left.begin(), left.end());
    out.insert(out.end(), right.begin(), right.end());
  }
  return out;
}

}  // namespace

TEST_CASE("PitchShifter construction refuses a value outside a field's domain", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : domain_refused(row)) {
      CAPTURE(row.name, bad);
      PitchShifterConfig config;
      config.*row.member = bad;
      try {
        PitchShifter shifter(config);
        FAIL("construction accepted " << row.name);
      } catch (const SonareException& error) {
        CHECK(error.code() == ErrorCode::InvalidParameter);
        CHECK(std::string(error.what()).find(row.name) != std::string::npos);
      }
      CHECK_THROWS_AS(PitchShifter::validate_config(config), SonareException);
    }
    for (const float good : {row.lo, row.hi}) {
      CAPTURE(row.name, good);
      PitchShifterConfig config;
      config.*row.member = good;
      CHECK_NOTHROW(PitchShifter::validate_config(config));
      CHECK_NOTHROW(PitchShifter(config));
    }
  }
}

TEST_CASE("PitchShifter live automation refuses an out-of-domain value and changes nothing",
          "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float bad : domain_refused(row)) {
      CAPTURE(row.name, bad);
      PitchShifter touched(domain_base());
      touched.prepare(kDomainRate, kDomainBlock);
      CHECK_FALSE(touched.set_parameter(row.id, bad));
      PitchShifter untouched(domain_base());
      untouched.prepare(kDomainRate, kDomainBlock);
      CHECK(domain_render(touched) == domain_render(untouched));
    }
  }
}

TEST_CASE("PitchShifter live automation applies a value on a bound exactly", "[effects]") {
  for (const DomainRow& row : kDomainRows) {
    for (const float good : {row.lo, row.hi}) {
      CAPTURE(row.name, good);
      PitchShifter live(domain_base());
      live.prepare(kDomainRate, kDomainBlock);
      REQUIRE(live.set_parameter(row.id, good));
      PitchShifterConfig config = domain_base();
      config.*row.member = good;
      PitchShifter constructed(config);
      constructed.prepare(kDomainRate, kDomainBlock);
      CHECK(domain_render(live) == domain_render(constructed));
    }
  }
}
