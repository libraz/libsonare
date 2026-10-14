#include "mastering/saturation/exciter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "rt/aliasing_control.h"
#include "util/constants.h"

namespace {

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples, int block_size = 256) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(block_size)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(block_size), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
}

template <typename Processor>
void process_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right,
                    int block_size = 256) {
  REQUIRE(left.size() == right.size());
  for (size_t offset = 0; offset < left.size(); offset += static_cast<size_t>(block_size)) {
    const int count =
        static_cast<int>(std::min<size_t>(static_cast<size_t>(block_size), left.size() - offset));
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
  }
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float max_difference(const std::vector<float>& first, const std::vector<float>& second) {
  REQUIRE(first.size() == second.size());
  float difference = 0.0f;
  for (size_t i = 0; i < first.size(); ++i) {
    difference = std::max(difference, std::abs(first[i] - second[i]));
  }
  return difference;
}

void fill_left_signal(std::vector<float>& samples, size_t first_frame) {
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] =
        0.11f * static_cast<float>(std::sin(311.0 * sonare::constants::kTwoPiD *
                                            static_cast<double>(first_frame + i) / 48000.0));
  }
}

}  // namespace

TEST_CASE("Exciter does not replay an inactive right plane after mono processing",
          "[mastering][saturation][boundary]") {
  using sonare::mastering::saturation::Exciter;
  using sonare::mastering::saturation::ExciterConfig;
  for (const auto aliasing :
       {sonare::rt::AliasingControl::None, sonare::rt::AliasingControl::Oversample4x}) {
    ExciterConfig config;
    config.frequency_hz = 1000.0f;
    config.q = 1.1f;
    config.amount = 0.35f;
    config.drive_db = 9.0f;
    config.even_odd_mix = 0.65f;
    config.aliasing = aliasing;
    Exciter subject(config);
    Exciter control(config);
    subject.prepare(48000.0, 113);
    control.prepare(48000.0, 113);
    std::vector<float> warm_left(4093, 0.0f);
    fill_left_signal(warm_left, 0);
    std::vector<float> warm_right(4093, 0.0f);
    for (size_t i = 0; i < warm_right.size(); ++i) {
      warm_right[i] = 0.17f * static_cast<float>(std::sin(731.0 * 2.0 * 3.141592653589793 *
                                                          static_cast<double>(i) / 48000.0));
    }
    auto control_left = warm_left;
    auto control_right = warm_right;
    process_stereo(subject, warm_left, warm_right, 113);
    process_stereo(control, control_left, control_right, 113);

    std::vector<float> mono_gap(36017, 0.0f);
    fill_left_signal(mono_gap, warm_left.size());
    auto stereo_gap_left = mono_gap;
    std::vector<float> stereo_gap_right(mono_gap.size(), 0.0f);
    process_mono(subject, mono_gap, 113);
    process_stereo(control, stereo_gap_left, stereo_gap_right, 113);

    std::vector<float> reentry_left(6007, 0.0f);
    fill_left_signal(reentry_left, warm_left.size() + mono_gap.size());
    std::vector<float> reentry_right(6007, 0.0f);
    auto control_reentry_left = reentry_left;
    auto control_reentry_right = reentry_right;
    process_stereo(subject, reentry_left, reentry_right, 113);
    process_stereo(control, control_reentry_left, control_reentry_right, 113);
    INFO("aliasing=" << static_cast<int>(aliasing));
    REQUIRE(peak_abs(reentry_right) < 1.0e-5f);
    REQUIRE(peak_abs(reentry_left) > 0.01f);
    REQUIRE(max_difference(reentry_left, control_reentry_left) < 1.0e-5f);
    REQUIRE(subject.non_finite_discard_count() == 0u);
    REQUIRE(control.non_finite_discard_count() == 0u);
  }
}
