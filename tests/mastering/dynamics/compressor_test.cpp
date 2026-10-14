#include "mastering/dynamics/compressor.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

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

std::vector<float> cosine(int samples, double rate, double frequency, float amplitude) {
  std::vector<float> result(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    result[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::cos(sonare::constants::kTwoPiD * frequency *
                                                static_cast<double>(i) / rate));
  }
  return result;
}

}  // namespace

TEST_CASE("Compressor sidechain HPF re-entry does not replay ordinary bypass history",
          "[mastering][dynamics][boundary]") {
  constexpr double kRate = 48000.0;
  sonare::mastering::dynamics::CompressorConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 10.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  config.sidechain_hpf_enabled = true;
  config.sidechain_hpf_hz = 80.0f;
  sonare::mastering::dynamics::Compressor subject(config);
  sonare::mastering::dynamics::Compressor control(config);
  subject.prepare(kRate, 256);
  control.prepare(kRate, 256);

  auto warm = cosine(4800, kRate, 40.0, 0.5f);
  auto warm_control = warm;
  process_mono(subject, warm);
  process_mono(control, warm_control);
  REQUIRE(subject.set_parameter(8, 0.0f));

  std::vector<float> gap(48000, 0.0f);
  auto control_gap = gap;
  process_mono(subject, gap);
  process_mono(control, control_gap);
  REQUIRE(subject.set_parameter(8, 1.0f));

  auto reentry = cosine(256, kRate, 1000.0, 0.01f);
  auto control_reentry = reentry;
  process_mono(subject, reentry);
  process_mono(control, control_reentry);

  REQUIRE(std::isfinite(reentry.front()));
  REQUIRE(std::abs(reentry.front() - control_reentry.front()) < 1.0e-4f);
  REQUIRE(subject.non_finite_discard_count() == 0u);
  REQUIRE(control.non_finite_discard_count() == 0u);
}

TEST_CASE("Compressor excluded detector plane re-entry does not replay HPF history",
          "[mastering][dynamics][boundary]") {
  constexpr double kRate = 48000.0;
  sonare::mastering::dynamics::CompressorConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 10.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  config.sidechain_hpf_enabled = true;
  config.sidechain_hpf_hz = 80.0f;
  sonare::mastering::dynamics::Compressor subject(config);
  sonare::mastering::dynamics::Compressor control(config);
  subject.prepare(kRate, 256);
  control.prepare(kRate, 256);

  auto warm_left = cosine(4800, kRate, 40.0, 0.5f);
  auto warm_right = warm_left;
  auto control_left = warm_left;
  auto control_right = warm_right;
  process_stereo(subject, warm_left, warm_right);
  process_stereo(control, control_left, control_right);
  subject.set_detector_excluded_channel(0);

  std::vector<float> gap_left(48000, 0.0f);
  auto gap_right = gap_left;
  auto control_gap_left = gap_left;
  auto control_gap_right = gap_right;
  process_stereo(subject, gap_left, gap_right);
  process_stereo(control, control_gap_left, control_gap_right);
  subject.set_detector_excluded_channel(-1);

  auto reentry_left = cosine(256, kRate, 1000.0, 0.01f);
  std::vector<float> reentry_right(256, 0.0f);
  auto control_reentry_left = reentry_left;
  auto control_reentry_right = reentry_right;
  process_stereo(subject, reentry_left, reentry_right);
  process_stereo(control, control_reentry_left, control_reentry_right);

  REQUIRE(std::isfinite(reentry_left.front()));
  REQUIRE(std::abs(reentry_left.front() - control_reentry_left.front()) < 1.0e-4f);
  REQUIRE(subject.non_finite_discard_count() == 0u);
  REQUIRE(control.non_finite_discard_count() == 0u);
}
