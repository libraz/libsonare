#include "effects/reverb/room_reverb.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using sonare::effects::reverb::RoomReverb;
using sonare::effects::reverb::RoomReverbConfig;

namespace {

constexpr int kPrepareBlock = 256;
constexpr int kMaxCall = 127;
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
void prime_stereo(Processor& processor) {
  std::vector<float> left(kPrepareBlock, 0.0f);
  std::vector<float> right(kPrepareBlock, 0.0f);
  left[0] = 0.5f;
  right[0] = 0.25f;
  process_stereo(processor, left, right);
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

bool finite(const std::vector<float>& samples) {
  return std::all_of(samples.begin(), samples.end(),
                     [](float sample) { return std::isfinite(sample); });
}

RoomReverbConfig boundary_room() {
  RoomReverbConfig config;
  config.dims = {4.0f, 3.0f, 2.5f};
  config.source = {1.0f, 1.0f, 1.0f};
  config.listener = {2.0f, 1.5f, 1.0f};
  config.absorption = 0.8f;
  config.ism_order = 0;
  config.seed = 77u;
  config.max_seconds = 0.05f;
  config.receiver_spacing_m = 0.2f;
  config.dry_wet = 1.0f;
  return config;
}

}  // namespace

TEST_CASE("RoomReverb drops a suspended stereo tail after a one-second mono gap",
          "[boundary][effects][reverb][room]") {
  const RoomReverbConfig config = boundary_room();

  for (const int sample_rate : {8000, 48000}) {
    INFO("sample rate " << sample_rate);
    RoomReverb subject(config);
    RoomReverb continuous(config);
    RoomReverb reset_control(config);
    subject.prepare(sample_rate, kPrepareBlock);
    continuous.prepare(sample_rate, kPrepareBlock);
    reset_control.prepare(sample_rate, kPrepareBlock);

    const int gap_samples = sample_rate;
    REQUIRE(subject.latency_samples() > 0);
    REQUIRE(subject.tail_samples() > 0);
    REQUIRE(subject.latency_samples() + subject.tail_samples() < gap_samples);
    prime_stereo(subject);
    prime_stereo(continuous);
    prime_stereo(reset_control);

    std::vector<float> subject_gap(gap_samples, 0.0f);
    process_mono(subject, subject_gap);
    std::vector<float> continuous_gap_left(gap_samples, 0.0f);
    std::vector<float> continuous_gap_right(gap_samples, 0.0f);
    process_stereo(continuous, continuous_gap_left, continuous_gap_right);
    std::vector<float> reset_gap(gap_samples, 0.0f);
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
    // Nonzero due output proves that priming reached the synthesized RIR.
    CHECK(pair_peak(continuous_gap_left, continuous_gap_right) > 1.0e-3);
    CHECK(peak(subject_gap) <= kReplayFloor);
    CHECK(pair_peak(subject_return_left, subject_return_right) <= kReplayFloor);
    CHECK(pair_peak(continuous_return_left, continuous_return_right) <= kReplayFloor);
    CHECK(pair_peak(reset_return_left, reset_return_right) <= kReplayFloor);
  }
}
