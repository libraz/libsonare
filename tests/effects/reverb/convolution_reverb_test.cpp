#include "effects/reverb/convolution_reverb.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "util/exception.h"

using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kPrepareBlock = 256;
constexpr int kMaxCall = 127;
constexpr int kGapSamples = kSampleRate;
constexpr float kReplayFloor = 1.0e-6f;

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples) {
  for (int offset = 0; offset < static_cast<int>(samples.size());) {
    const int count = std::min(kMaxCall, static_cast<int>(samples.size()) - offset);
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
    offset += count;
  }
}

template <typename Processor>
void process_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right) {
  REQUIRE(left.size() == right.size());
  for (int offset = 0; offset < static_cast<int>(left.size());) {
    const int count = std::min(kMaxCall, static_cast<int>(left.size()) - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
    offset += count;
  }
}

template <typename Processor>
void prime_stereo(Processor& processor, int frames = kPrepareBlock) {
  std::vector<float> left(static_cast<size_t>(frames), 0.0f);
  std::vector<float> right(static_cast<size_t>(frames), 0.0f);
  if (frames <= 0) return;
  left[0] = 0.5f;
  right[0] = 0.25f;
  process_stereo(processor, left, right);
}

template <typename Processor>
void prime_mono(Processor& processor) {
  std::vector<float> mono(kPrepareBlock, 0.0f);
  mono[0] = 0.5f;
  process_mono(processor, mono);
}

double peak(const std::vector<float>& samples) {
  double result = 0.0;
  for (const float sample : samples)
    result = std::max(result, std::abs(static_cast<double>(sample)));
  return result;
}

double pair_peak(const std::vector<float>& left, const std::vector<float>& right) {
  return std::max(peak(left), peak(right));
}

double max_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double result = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    result = std::max(result, std::abs(static_cast<double>(a[i]) - b[i]));
  }
  return result;
}

bool finite(const std::vector<float>& samples) {
  return std::all_of(samples.begin(), samples.end(),
                     [](float sample) { return std::isfinite(sample); });
}

ConvolutionReverb make_pair_reverb() {
  ConvolutionReverbConfig config;
  config.dry_wet = 1.0f;
  ConvolutionReverb reverb(config);

  // Distinct IRs distinguish a stale path from the freshly selected one.
  reverb.load_ir_set_unit_energy({1.0f, 0.35f, -0.15f}, {0.8f, -0.25f}, {0.35f, 0.1f, -0.2f});
  reverb.prepare(kSampleRate, kPrepareBlock);
  return reverb;
}

ConvolutionReverb make_mono_reverb() {
  ConvolutionReverbConfig config;
  config.dry_wet = 1.0f;
  ConvolutionReverb reverb(config);
  const std::vector<float> mono{1.0f, 0.35f, -0.15f};
  reverb.load_ir_unit_energy(mono.data(), static_cast<int>(mono.size()));
  reverb.prepare(kSampleRate, kPrepareBlock);
  return reverb;
}

}  // namespace

TEST_CASE("ConvolutionReverb drops suspended pair history after mono silence",
          "[boundary][effects][reverb][convolution]") {
  auto subject = make_pair_reverb();
  auto continuous = make_pair_reverb();
  auto reset_control = make_pair_reverb();
  prime_stereo(subject);
  prime_stereo(continuous);
  prime_stereo(reset_control);

  std::vector<float> subject_gap(kGapSamples, 0.0f);
  process_mono(subject, subject_gap);

  // Nonzero due output in the stereo control proves that priming reached the IR.
  std::vector<float> continuous_gap_left(kGapSamples, 0.0f);
  std::vector<float> continuous_gap_right(kGapSamples, 0.0f);
  process_stereo(continuous, continuous_gap_left, continuous_gap_right);

  std::vector<float> reset_gap(kGapSamples, 0.0f);
  process_mono(reset_control, reset_gap);
  reset_control.reset();

  std::vector<float> subject_return_left(512, 0.0f);
  std::vector<float> subject_return_right(512, 0.0f);
  process_stereo(subject, subject_return_left, subject_return_right);
  std::vector<float> continuous_return_left(512, 0.0f);
  std::vector<float> continuous_return_right(512, 0.0f);
  process_stereo(continuous, continuous_return_left, continuous_return_right);
  std::vector<float> reset_return_left(512, 0.0f);
  std::vector<float> reset_return_right(512, 0.0f);
  process_stereo(reset_control, reset_return_left, reset_return_right);

  CHECK(finite(subject_gap));
  CHECK(finite(continuous_gap_left));
  CHECK(finite(continuous_gap_right));
  CHECK(finite(subject_return_left));
  CHECK(finite(subject_return_right));
  CHECK(finite(continuous_return_left));
  CHECK(finite(continuous_return_right));
  CHECK(finite(reset_return_left));
  CHECK(finite(reset_return_right));
  CHECK(peak(continuous_gap_left) > 1.0e-3);
  CHECK(peak(continuous_gap_right) > 1.0e-3);
  CHECK(peak(subject_gap) <= kReplayFloor);
  CHECK(pair_peak(subject_return_left, subject_return_right) <= kReplayFloor);
  CHECK(pair_peak(continuous_return_left, continuous_return_right) <= kReplayFloor);
  CHECK(pair_peak(reset_return_left, reset_return_right) <= kReplayFloor);
  CHECK(max_difference(subject_return_left, reset_return_left) <= kReplayFloor);
  CHECK(max_difference(subject_return_right, reset_return_right) <= kReplayFloor);
}

TEST_CASE("ConvolutionReverb clears an inactive right channel for a mono IR",
          "[boundary][effects][reverb][convolution]") {
  auto subject = make_mono_reverb();
  auto continuous = make_mono_reverb();
  auto reset_control = make_mono_reverb();
  prime_stereo(subject);
  prime_stereo(continuous);
  prime_stereo(reset_control);

  std::vector<float> subject_gap(kGapSamples, 0.0f);
  process_mono(subject, subject_gap);
  std::vector<float> continuous_gap_left(kGapSamples, 0.0f);
  std::vector<float> continuous_gap_right(kGapSamples, 0.0f);
  process_stereo(continuous, continuous_gap_left, continuous_gap_right);
  std::vector<float> reset_gap(kGapSamples, 0.0f);
  process_mono(reset_control, reset_gap);
  reset_control.reset();

  std::vector<float> subject_return_left(512, 0.0f);
  std::vector<float> subject_return_right(512, 0.0f);
  process_stereo(subject, subject_return_left, subject_return_right);
  std::vector<float> continuous_return_left(512, 0.0f);
  std::vector<float> continuous_return_right(512, 0.0f);
  process_stereo(continuous, continuous_return_left, continuous_return_right);
  std::vector<float> reset_return_left(512, 0.0f);
  std::vector<float> reset_return_right(512, 0.0f);
  process_stereo(reset_control, reset_return_left, reset_return_right);

  CHECK(finite(subject_gap));
  CHECK(finite(subject_return_left));
  CHECK(finite(subject_return_right));
  CHECK(finite(continuous_gap_left));
  CHECK(finite(continuous_gap_right));
  CHECK(finite(continuous_return_left));
  CHECK(finite(continuous_return_right));
  CHECK(finite(reset_return_left));
  CHECK(finite(reset_return_right));
  CHECK(peak(continuous_gap_left) > 1.0e-3);
  CHECK(peak(continuous_gap_right) > 1.0e-3);
  // Active left finishes the impulse; inactive right must return silent.
  CHECK(peak(subject_gap) > 1.0e-3);
  CHECK(peak(subject_return_left) <= kReplayFloor);
  CHECK(peak(subject_return_right) <= kReplayFloor);
  CHECK(pair_peak(continuous_return_left, continuous_return_right) <= kReplayFloor);
  CHECK(pair_peak(reset_return_left, reset_return_right) <= kReplayFloor);
}

TEST_CASE("ConvolutionReverb drops suspended mono history on stereo to mono reentry",
          "[boundary][effects][reverb][convolution]") {
  auto subject = make_pair_reverb();
  auto continuous = make_pair_reverb();
  auto reset_control = make_pair_reverb();
  prime_mono(subject);
  prime_mono(continuous);
  prime_mono(reset_control);

  std::vector<float> subject_gap_left(kGapSamples, 0.0f);
  std::vector<float> subject_gap_right(kGapSamples, 0.0f);
  process_stereo(subject, subject_gap_left, subject_gap_right);
  std::vector<float> continuous_gap(kGapSamples, 0.0f);
  process_mono(continuous, continuous_gap);
  std::vector<float> reset_gap_left(kGapSamples, 0.0f);
  std::vector<float> reset_gap_right(kGapSamples, 0.0f);
  process_stereo(reset_control, reset_gap_left, reset_gap_right);
  reset_control.reset();

  std::vector<float> subject_return(512, 0.0f);
  process_mono(subject, subject_return);
  std::vector<float> continuous_return(512, 0.0f);
  process_mono(continuous, continuous_return);
  std::vector<float> reset_return(512, 0.0f);
  process_mono(reset_control, reset_return);

  CHECK(finite(subject_gap_left));
  CHECK(finite(subject_gap_right));
  CHECK(finite(continuous_gap));
  CHECK(finite(reset_gap_left));
  CHECK(finite(reset_gap_right));
  CHECK(finite(subject_return));
  CHECK(finite(continuous_return));
  CHECK(finite(reset_return));
  CHECK(peak(continuous_gap) > 1.0e-3);
  CHECK(peak(subject_return) <= kReplayFloor);
  CHECK(peak(continuous_return) <= kReplayFloor);
  CHECK(peak(reset_return) <= kReplayFloor);
}

TEST_CASE("ConvolutionReverb preserves the active left history across a width change",
          "[boundary][effects][reverb][convolution]") {
  auto reverb = make_mono_reverb();
  prime_mono(reverb);

  std::vector<float> left(127, 0.0f);
  std::vector<float> right(127, 0.0f);
  process_stereo(reverb, left, right);

  // Active left retains pending output while newly active right starts empty.
  CHECK(finite(left));
  CHECK(finite(right));
  CHECK(peak(left) > 1.0e-2);
  CHECK(peak(right) <= kReplayFloor);
}

TEST_CASE("ConvolutionReverb reload clears staged output without an intervening process call",
          "[boundary][effects][reverb][convolution]") {
  const std::vector<float> impulse_response{1.0f, 0.35f, -0.15f};
  for (const int reload_mode : {0, 1, 2}) {
    CAPTURE(reload_mode);
    auto subject = make_mono_reverb();
    auto continuous = make_mono_reverb();
    prime_stereo(subject);
    prime_stereo(continuous);

    std::vector<float> due_left(512, 0.0f);
    std::vector<float> due_right(512, 0.0f);
    process_stereo(continuous, due_left, due_right);
    REQUIRE(pair_peak(due_left, due_right) > 1.0e-3);

    if (reload_mode != 0) subject.load_ir(nullptr, 0);
    if (reload_mode == 2) {
      std::vector<float> empty_left(1024, 0.0f);
      std::vector<float> empty_right(1024, 0.0f);
      process_stereo(subject, empty_left, empty_right);
    }
    subject.load_ir(impulse_response);
    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    process_stereo(subject, left, right);
    CHECK(finite(left));
    CHECK(finite(right));
    CHECK(pair_peak(left, right) <= kReplayFloor);
  }
}

TEST_CASE("ConvolutionReverb handles partial partition boundaries on path reentry",
          "[boundary][effects][reverb][convolution]") {
  for (const int partial : {255, 256, 257}) {
    INFO("partial partition length " << partial);
    auto subject = make_pair_reverb();
    prime_stereo(subject, partial);

    std::vector<float> gap(kGapSamples, 0.0f);
    process_mono(subject, gap);
    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    process_stereo(subject, left, right);

    CHECK(finite(gap));
    CHECK(finite(left));
    CHECK(finite(right));
    CHECK(peak(gap) <= kReplayFloor);
    CHECK(pair_peak(left, right) <= kReplayFloor);
  }
}

TEST_CASE("ConvolutionReverb reset and reprepare discard dirty path history",
          "[boundary][effects][reverb][convolution]") {
  auto reset_case = make_pair_reverb();
  prime_stereo(reset_case);
  std::vector<float> dirty_gap(kGapSamples, 0.0f);
  process_mono(reset_case, dirty_gap);
  reset_case.reset();

  std::vector<float> reset_left(512, 0.0f);
  std::vector<float> reset_right(512, 0.0f);
  process_stereo(reset_case, reset_left, reset_right);
  CHECK(finite(reset_left));
  CHECK(finite(reset_right));
  CHECK(pair_peak(reset_left, reset_right) <= kReplayFloor);

  for (const int sample_rate : {kSampleRate, 44100}) {
    INFO("reprepare sample rate " << sample_rate);
    auto reprepare_case = make_pair_reverb();
    prime_stereo(reprepare_case);
    std::vector<float> gap(kGapSamples, 0.0f);
    process_mono(reprepare_case, gap);
    reprepare_case.prepare(sample_rate, kPrepareBlock);

    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    process_stereo(reprepare_case, left, right);
    CHECK(finite(gap));
    CHECK(finite(left));
    CHECK(finite(right));
    CHECK(pair_peak(left, right) <= kReplayFloor);
  }
}

TEST_CASE("ConvolutionReverb rejected normalized IR preserves response and metadata",
          "[boundary][effects][reverb][convolution]") {
  for (const int rate : {48000, 96000}) {
    for (const int lifecycle : {0, 1, 2}) {
      CAPTURE(rate, lifecycle);
      ConvolutionReverb subject;
      ConvolutionReverb control;
      std::vector<float> accepted(1024, 0.0f);
      accepted[1000] = 0.5f;
      subject.load_ir(accepted);
      control.load_ir(accepted);
      subject.prepare(rate, kPrepareBlock);
      control.prepare(rate, kPrepareBlock);
      prime_mono(subject);
      prime_mono(control);
      const auto old_tail = subject.tail_samples();
      const float rejected = 0.0f;
      REQUIRE_THROWS_AS(subject.load_ir_unit_energy(&rejected, 1), sonare::SonareException);
      CHECK(subject.ir_size() == 1024);
      CHECK(subject.tail_samples() == old_tail);
      CHECK(subject.latency_samples() == control.latency_samples());
      if (lifecycle == 1) {
        subject.reset();
        control.reset();
      } else if (lifecycle == 2) {
        subject.prepare(rate, kPrepareBlock);
        control.prepare(rate, kPrepareBlock);
      }
      std::vector<float> actual(4096, 0.0f);
      std::vector<float> expected(4096, 0.0f);
      if (lifecycle != 0) actual[0] = expected[0] = 0.5f;
      process_mono(subject, actual);
      process_mono(control, expected);
      REQUIRE(peak(expected) > 0.2);
      CHECK(finite(actual));
      CHECK(max_difference(actual, expected) == 0.0);
    }
  }
}

TEST_CASE("ConvolutionReverb invalid IR set leaves stereo response intact",
          "[boundary][effects][reverb][convolution]") {
  for (const float invalid :
       {0.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    CAPTURE(invalid);
    auto subject = make_pair_reverb();
    auto control = make_pair_reverb();
    const std::vector<float> bad{invalid};
    REQUIRE_THROWS_AS(subject.load_ir_unit_energy(bad.data(), 1), sonare::SonareException);
    REQUIRE_THROWS_AS(subject.load_ir_set_unit_energy({1.0f}, {1.0f}, bad),
                      sonare::SonareException);
    if (!std::isfinite(invalid)) {
      REQUIRE_THROWS_AS(subject.load_ir(bad), sonare::SonareException);
    }
    CHECK(subject.ir_size() == control.ir_size());
    CHECK(subject.tail_samples() == control.tail_samples());
    subject.prepare(kSampleRate, kPrepareBlock);
    control.prepare(kSampleRate, kPrepareBlock);
    std::vector<float> left(1024, 0.0f), right(1024, 0.0f);
    std::vector<float> expected_left(1024, 0.0f), expected_right(1024, 0.0f);
    left[0] = expected_left[0] = 0.5f;
    right[0] = expected_right[0] = 0.25f;
    process_stereo(subject, left, right);
    process_stereo(control, expected_left, expected_right);
    REQUIRE(pair_peak(expected_left, expected_right) > 0.1);
    CHECK(finite(left));
    CHECK(finite(right));
    CHECK(max_difference(left, expected_left) == 0.0);
    CHECK(max_difference(right, expected_right) == 0.0);
  }
}

TEST_CASE("ConvolutionReverb rejection before prepare preserves implicit default IR",
          "[boundary][effects][reverb][convolution]") {
  ConvolutionReverb subject;
  ConvolutionReverb control;
  const float zero = 0.0f;
  REQUIRE_THROWS_AS(subject.load_ir_unit_energy(&zero, 1), sonare::SonareException);
  subject.prepare(kSampleRate, kPrepareBlock);
  control.prepare(kSampleRate, kPrepareBlock);
  CHECK(subject.ir_size() == control.ir_size());
  std::vector<float> actual(4096, 0.0f), expected(4096, 0.0f);
  actual[0] = expected[0] = 0.5f;
  process_mono(subject, actual);
  process_mono(control, expected);
  REQUIRE(peak(expected) > 1.0e-4);
  CHECK(finite(actual));
  CHECK(max_difference(actual, expected) == 0.0);
}

TEST_CASE("ConvolutionReverb normalizes finite IR amplitude boundaries",
          "[boundary][effects][reverb][convolution]") {
  for (const float amplitude : {std::numeric_limits<float>::denorm_min(), 1.0e-20f, 0.001f,
                                std::numeric_limits<float>::max()}) {
    CAPTURE(amplitude);
    ConvolutionReverb reverb;
    REQUIRE_NOTHROW(reverb.load_ir_unit_energy(&amplitude, 1));
    reverb.prepare(kSampleRate, kPrepareBlock);
    std::vector<float> output(1024, 0.0f);
    output[0] = 0.5f;
    process_mono(reverb, output);
    CHECK(finite(output));
    CHECK(std::abs(peak(output) - 0.5) < 1.0e-6);
  }
}
