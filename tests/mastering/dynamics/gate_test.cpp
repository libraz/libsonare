#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

namespace {

using sonare::mastering::dynamics::Gate;
using sonare::mastering::dynamics::GateConfig;

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples, int block_size = 137) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(block_size)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(block_size), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
}

template <typename Processor>
void process_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right,
                    int block_size = 137) {
  REQUIRE(left.size() == right.size());
  for (size_t offset = 0; offset < left.size(); offset += static_cast<size_t>(block_size)) {
    const int count =
        static_cast<int>(std::min<size_t>(static_cast<size_t>(block_size), left.size() - offset));
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
  }
}

std::vector<float> cosine(int samples, double sample_rate, double frequency, float amplitude) {
  std::vector<float> result(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    result[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::cos(sonare::constants::kTwoPiD * frequency * i / sample_rate));
  }
  return result;
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (const float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float max_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float difference = 0.0f;
  for (size_t i = 0; i < lhs.size(); ++i) {
    difference = std::max(difference, std::abs(lhs[i] - rhs[i]));
  }
  return difference;
}

}  // namespace

TEST_CASE("Gate key HPF re-entry clears history in both detector branches",
          "[mastering][dynamics][boundary]") {
  constexpr GateConfig enabled{-30.0f, 0.0f, 0.0f, -80.0f, 0.0f, -30.0f, 80.0f};

  for (const int sample_rate : {48000, 96000}) {
    for (const bool exclude_first_channel : {false, true}) {
      Gate subject(enabled);
      Gate continuous(enabled);
      Gate reset_control(enabled);
      subject.prepare(sample_rate, 256);
      continuous.prepare(sample_rate, 256);
      reset_control.prepare(sample_rate, 256);
      if (exclude_first_channel) {
        subject.set_detector_excluded_channel(0);
        continuous.set_detector_excluded_channel(0);
        reset_control.set_detector_excluded_channel(0);
      }

      auto warm = cosine(sample_rate / 10, sample_rate, 40.0, 0.5f);
      if (exclude_first_channel) {
        auto warm_right = warm;
        auto continuous_warm = warm;
        auto continuous_warm_right = warm;
        auto reset_warm = warm;
        auto reset_warm_right = warm;
        process_stereo(subject, warm, warm_right);
        process_stereo(continuous, continuous_warm, continuous_warm_right);
        process_stereo(reset_control, reset_warm, reset_warm_right);
      } else {
        auto continuous_warm = warm;
        auto reset_warm = warm;
        process_mono(subject, warm);
        process_mono(continuous, continuous_warm);
        process_mono(reset_control, reset_warm);
      }

      GateConfig disabled = enabled;
      disabled.key_hpf_hz = 0.0f;
      subject.set_config(disabled);
      std::vector<float> gap(static_cast<size_t>(sample_rate), 0.0f);
      if (exclude_first_channel) {
        auto gap_right = gap;
        auto continuous_gap = gap;
        auto continuous_gap_right = gap;
        auto reset_gap = gap;
        auto reset_gap_right = gap;
        process_stereo(subject, gap, gap_right);
        process_stereo(continuous, continuous_gap, continuous_gap_right);
        process_stereo(reset_control, reset_gap, reset_gap_right);
      } else {
        auto continuous_gap = gap;
        auto reset_gap = gap;
        process_mono(subject, gap);
        process_mono(continuous, continuous_gap);
        process_mono(reset_control, reset_gap);
      }

      reset_control.reset();
      subject.set_config(enabled);
      std::vector<float> reentry = cosine(256, sample_rate, 1000.0, 0.01f);
      if (exclude_first_channel) {
        std::vector<float> reentry_left(reentry.size(), 0.0f);
        auto continuous_left = reentry_left;
        auto continuous_right = reentry;
        auto reset_left = reentry_left;
        auto reset_right = reentry;
        process_stereo(subject, reentry_left, reentry);
        process_stereo(continuous, continuous_left, continuous_right);
        process_stereo(reset_control, reset_left, reset_right);
        CHECK(peak_abs(reentry) < 2.0e-6f);
        CHECK(peak_abs(continuous_right) < 2.0e-6f);
        CHECK(peak_abs(reset_right) < 2.0e-6f);
        CHECK(peak_abs(continuous_left) < 2.0e-6f);
        CHECK(peak_abs(reset_left) < 2.0e-6f);
        CHECK(max_difference(reentry, continuous_right) < 2.0e-6f);
        CHECK(max_difference(reentry, reset_right) < 2.0e-6f);
      } else {
        auto continuous_reentry = reentry;
        auto reset_reentry = reentry;
        process_mono(subject, reentry);
        process_mono(continuous, continuous_reentry);
        process_mono(reset_control, reset_reentry);
        CHECK(peak_abs(reentry) < 2.0e-6f);
        CHECK(peak_abs(continuous_reentry) < 2.0e-6f);
        CHECK(peak_abs(reset_reentry) < 2.0e-6f);
        CHECK(max_difference(reentry, continuous_reentry) < 2.0e-6f);
        CHECK(max_difference(reentry, reset_reentry) < 2.0e-6f);
      }

      CHECK(subject.non_finite_discard_count() == 0u);
      CHECK(continuous.non_finite_discard_count() == 0u);
      CHECK(reset_control.non_finite_discard_count() == 0u);
    }
  }
}

TEST_CASE("Gate excluded detector plane re-entry does not replay key HPF history",
          "[mastering][dynamics][boundary]") {
  constexpr int kRate = 48000;
  constexpr GateConfig config{-30.0f, 0.0f, 0.0f, -80.0f, 0.0f, -30.0f, 80.0f};
  Gate subject(config);
  Gate control(config);
  subject.prepare(kRate, 256);
  control.prepare(kRate, 256);

  auto warm_left = cosine(kRate / 10, kRate, 40.0, 0.5f);
  auto warm_right = warm_left;
  auto control_left = warm_left;
  auto control_right = warm_right;
  process_stereo(subject, warm_left, warm_right);
  process_stereo(control, control_left, control_right);
  subject.set_detector_excluded_channel(0);

  std::vector<float> gap_left(static_cast<size_t>(kRate), 0.0f);
  auto gap_right = gap_left;
  auto control_gap_left = gap_left;
  auto control_gap_right = gap_right;
  process_stereo(subject, gap_left, gap_right);
  process_stereo(control, control_gap_left, control_gap_right);
  subject.set_detector_excluded_channel(-1);

  auto reentry_left = cosine(256, kRate, 1000.0, 0.01f);
  std::vector<float> reentry_right(reentry_left.size(), 0.0f);
  auto control_reentry_left = reentry_left;
  auto control_reentry_right = reentry_right;
  process_stereo(subject, reentry_left, reentry_right);
  process_stereo(control, control_reentry_left, control_reentry_right);

  CHECK(peak_abs(control_reentry_left) < 2.0e-6f);
  CHECK(max_difference(reentry_left, control_reentry_left) < 2.0e-6f);
  CHECK(subject.non_finite_discard_count() == 0u);
  CHECK(control.non_finite_discard_count() == 0u);
}
