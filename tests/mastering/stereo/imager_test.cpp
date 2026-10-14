#include "mastering/stereo/imager.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

namespace {

using sonare::mastering::stereo::Imager;
using sonare::mastering::stereo::ImagerConfig;

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

TEST_CASE("Imager mono interval closes allpass history before stereo re-entry",
          "[mastering][stereo][boundary]") {
  constexpr ImagerConfig config{2.0f, 0.0f, 1.0f, false};

  for (const int sample_rate : {48000, 96000}) {
    Imager subject(config);
    Imager continuous(config);
    Imager reset_control(config);
    Imager fresh(config);
    subject.prepare(sample_rate, 256);
    continuous.prepare(sample_rate, 256);
    reset_control.prepare(sample_rate, 256);
    fresh.prepare(sample_rate, 256);

    auto warm_left = cosine(256, sample_rate, 440.0, 0.2f);
    auto warm_right = warm_left;
    for (size_t i = 0; i < warm_right.size(); ++i) warm_right[i] = -warm_right[i];
    auto continuous_warm_left = warm_left;
    auto continuous_warm_right = warm_right;
    auto reset_warm_left = warm_left;
    auto reset_warm_right = warm_right;
    process_stereo(subject, warm_left, warm_right);
    process_stereo(continuous, continuous_warm_left, continuous_warm_right);
    process_stereo(reset_control, reset_warm_left, reset_warm_right);

    std::vector<float> mono_gap(static_cast<size_t>(sample_rate), 0.0f);
    process_mono(subject, mono_gap);
    auto continuous_gap_left = mono_gap;
    auto continuous_gap_right = mono_gap;
    process_stereo(continuous, continuous_gap_left, continuous_gap_right);
    auto reset_gap = mono_gap;
    process_mono(reset_control, reset_gap);
    reset_control.reset();

    std::vector<float> reentry_left(256, 0.0f);
    std::vector<float> reentry_right(256, 0.0f);
    auto continuous_reentry_left = reentry_left;
    auto continuous_reentry_right = reentry_right;
    auto reset_reentry_left = reentry_left;
    auto reset_reentry_right = reentry_right;
    process_stereo(subject, reentry_left, reentry_right);
    process_stereo(continuous, continuous_reentry_left, continuous_reentry_right);
    process_stereo(reset_control, reset_reentry_left, reset_reentry_right);

    CHECK(peak_abs(reentry_left) < 1.0e-6f);
    CHECK(peak_abs(reentry_right) < 1.0e-6f);
    CHECK(peak_abs(continuous_reentry_left) < 1.0e-6f);
    CHECK(peak_abs(continuous_reentry_right) < 1.0e-6f);
    CHECK(peak_abs(reset_reentry_left) < 1.0e-6f);
    CHECK(peak_abs(reset_reentry_right) < 1.0e-6f);

    std::vector<float> active_left(256, 0.0f);
    std::vector<float> active_right(256, 0.0f);
    active_left[0] = 0.2f;
    active_right[0] = -0.2f;
    auto fresh_left = active_left;
    auto fresh_right = active_right;
    process_stereo(subject, active_left, active_right);
    process_stereo(fresh, fresh_left, fresh_right);
    CHECK(max_difference(active_left, fresh_left) < 1.0e-6f);
    CHECK(max_difference(active_right, fresh_right) < 1.0e-6f);
    CHECK(subject.config().width == config.width);
    CHECK(subject.config().decorrelation_amount == config.decorrelation_amount);
    CHECK(subject.config().preserve_energy == config.preserve_energy);
    CHECK(subject.non_finite_discard_count() == 0u);
    CHECK(continuous.non_finite_discard_count() == 0u);
    CHECK(reset_control.non_finite_discard_count() == 0u);
    CHECK(fresh.non_finite_discard_count() == 0u);
  }
}
