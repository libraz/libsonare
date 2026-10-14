#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

namespace {

using sonare::mastering::dynamics::SidechainRouter;
using sonare::mastering::dynamics::SidechainRouterConfig;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

SidechainRouterConfig ducking_config() {
  SidechainRouterConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 10.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.range_db = 60.0f;
  config.sidechain_hpf_enabled = true;
  config.sidechain_hpf_hz = 80.0f;
  return config;
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

void process_mono(SidechainRouter& router, std::vector<float>& samples,
                  const std::vector<float>* key = nullptr) {
  for (size_t offset = 0; offset < samples.size(); offset += kBlock) {
    const int count = static_cast<int>(std::min<size_t>(kBlock, samples.size() - offset));
    if (key != nullptr) {
      const float* key_channels[] = {key->data() + offset};
      router.set_sidechain(key_channels, 1, count);
    }
    float* channels[] = {samples.data() + offset};
    router.process(channels, 1, count);
  }
}

void process_stereo(SidechainRouter& router, std::vector<float>& left, std::vector<float>& right) {
  REQUIRE(left.size() == right.size());
  for (size_t offset = 0; offset < left.size(); offset += kBlock) {
    const int count = static_cast<int>(std::min<size_t>(kBlock, left.size() - offset));
    float* channels[] = {left.data() + offset, right.data() + offset};
    router.process(channels, 2, count);
  }
}

float max_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  float difference = 0.0f;
  for (size_t i = 0; i < lhs.size(); ++i) {
    difference = std::max(difference, std::abs(lhs[i] - rhs[i]));
  }
  return difference;
}

// The re-entry probe sits 10 dB under the threshold, so any ducking is replayed history.
constexpr float kProbeAmplitude = 0.01f;
constexpr float kTolerance = kProbeAmplitude * 1.0e-4f;

}  // namespace

TEST_CASE("SidechainRouter sidechain HPF re-entry does not replay ordinary bypass history",
          "[mastering][dynamics][boundary]") {
  const SidechainRouterConfig config = ducking_config();
  SidechainRouter subject(config);
  SidechainRouter control(config);
  subject.prepare(kRate, kBlock);
  control.prepare(kRate, kBlock);

  auto warm = cosine(4800, kRate, 40.0, 0.5f);
  auto warm_control = warm;
  process_mono(subject, warm);
  process_mono(control, warm_control);
  SidechainRouterConfig disabled = config;
  disabled.sidechain_hpf_enabled = false;
  subject.set_config(disabled);

  std::vector<float> gap(48000, 0.0f);
  auto control_gap = gap;
  process_mono(subject, gap);
  process_mono(control, control_gap);
  subject.set_config(config);

  auto reentry = cosine(kBlock, kRate, 1000.0, kProbeAmplitude);
  auto control_reentry = reentry;
  process_mono(subject, reentry);
  process_mono(control, control_reentry);

  CHECK(max_difference(reentry, control_reentry) < kTolerance);
  CHECK(subject.non_finite_discard_count() == 0u);
  CHECK(control.non_finite_discard_count() == 0u);
}

TEST_CASE("SidechainRouter excluded detector plane re-entry does not replay HPF history",
          "[mastering][dynamics][boundary]") {
  const SidechainRouterConfig config = ducking_config();
  SidechainRouter subject(config);
  SidechainRouter control(config);
  subject.prepare(kRate, kBlock);
  control.prepare(kRate, kBlock);

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

  auto reentry_left = cosine(kBlock, kRate, 1000.0, kProbeAmplitude);
  std::vector<float> reentry_right(reentry_left.size(), 0.0f);
  auto control_reentry_left = reentry_left;
  auto control_reentry_right = reentry_right;
  process_stereo(subject, reentry_left, reentry_right);
  process_stereo(control, control_reentry_left, control_reentry_right);

  CHECK(max_difference(reentry_left, control_reentry_left) < kTolerance);
  CHECK(subject.non_finite_discard_count() == 0u);
  CHECK(control.non_finite_discard_count() == 0u);
}

TEST_CASE("SidechainRouter switching from the key to the internal detector clears HPF history",
          "[mastering][dynamics][boundary]") {
  const SidechainRouterConfig config = ducking_config();
  SidechainRouter subject(config);
  SidechainRouter control(config);
  subject.prepare(kRate, kBlock);
  control.prepare(kRate, kBlock);

  // The subject's key ends on a loud low cosine; the control never had a key.
  const auto key = cosine(4800, kRate, 40.0, 0.5f);
  std::vector<float> main(key.size(), 0.0f);
  auto control_main = main;
  process_mono(subject, main, &key);
  process_mono(control, control_main);
  subject.clear_sidechain();

  auto reentry = cosine(kBlock, kRate, 1000.0, kProbeAmplitude);
  auto control_reentry = reentry;
  process_mono(subject, reentry);
  process_mono(control, control_reentry);

  CHECK(max_difference(reentry, control_reentry) < kTolerance);
  CHECK(subject.non_finite_discard_count() == 0u);
  CHECK(control.non_finite_discard_count() == 0u);
}
