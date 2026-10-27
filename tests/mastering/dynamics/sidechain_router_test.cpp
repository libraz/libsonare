#include "mastering/dynamics/sidechain_router.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <memory>
#include <vector>

#include "mastering/api/insert_factory.h"
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
    // set_sidechain borrows the channel array, so it must outlive process().
    const float* key_channels[] = {key != nullptr ? key->data() + offset : nullptr};
    if (key != nullptr) router.set_sidechain(key_channels, 1, count);
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

namespace {

constexpr int kLongBlock = 512;
constexpr int kShortKey = 128;
constexpr int kIdleBlocks = 99;
constexpr float kBed = 0.5f;

// Runs a bed block under a loud key head of kShortKey frames, then `idle_blocks` blocks keyed by
// a single zero sample, then a block with a full zero key. `padded` supplies each key zero-padded
// to the block instead of short. Returns every output sample.
std::vector<float> run_short_key(const SidechainRouterConfig& config, bool padded,
                                 int idle_blocks) {
  SidechainRouter router(config);
  router.prepare(kRate, kLongBlock);
  std::vector<float> output;
  std::vector<float> head(kLongBlock, 0.0f);
  std::fill(head.begin(), head.begin() + kShortKey, kBed);
  const std::vector<float> zeros(kLongBlock, 0.0f);

  auto run_block = [&](const std::vector<float>& key, int key_frames) {
    std::vector<float> bed(kLongBlock, kBed);
    const float* key_channels[] = {key.data()};
    router.set_sidechain(key_channels, 1, key_frames);
    float* channels[] = {bed.data()};
    router.process(channels, 1, kLongBlock);
    output.insert(output.end(), bed.begin(), bed.end());
  };

  run_block(head, padded ? kLongBlock : kShortKey);
  for (int i = 0; i < idle_blocks; ++i) run_block(zeros, padded ? kLongBlock : 1);
  run_block(zeros, kLongBlock);
  return output;
}

}  // namespace

TEST_CASE("SidechainRouter a key shorter than the block behaves as a zero-padded key",
          "[mastering][dynamics][boundary]") {
  SidechainRouterConfig config = ducking_config();
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  const int idle_blocks = GENERATE(0, kIdleBlocks);
  const int mode = GENERATE(0, 1, 2, 3);
  CAPTURE(idle_blocks, mode);
  if (mode == 1) config.sidechain_hpf_enabled = false;
  if (mode == 2) config.key_listen = true;
  if (mode == 3) config.lookahead_ms = 2.0f;

  const auto short_key = run_short_key(config, false, idle_blocks);
  const auto padded_key = run_short_key(config, true, idle_blocks);
  REQUIRE(short_key.size() == padded_key.size());
  CHECK(max_difference(short_key, padded_key) < kBed * 1.0e-5f);
  if (mode == 0 || mode == 3) {
    // The loud head leaves the bed ducked only while the key is live: the closing block is clean.
    const std::vector<float> last_short(short_key.end() - kLongBlock, short_key.end());
    const std::vector<float> flat(kLongBlock, kBed);
    CHECK(max_difference(last_short, flat) < kBed * 1.0e-5f);
  }
}

TEST_CASE("Sidechain inserts leave the bed alone under a silent key at thresholds below the floor",
          "[mastering][dynamics][boundary]") {
  const std::string name =
      GENERATE(std::string("dynamics.sidechainRouter"), std::string("dynamics.duckingProcessor"));
  const float threshold = GENERATE(-120.0f, -120.1f, -130.0f, -144.0f);
  CAPTURE(name, threshold);
  const std::string params = "{\"thresholdDb\":" + std::to_string(threshold) +
                             ",\"ratio\":4,\"attackMs\":0,\"releaseMs\":0,\"rangeDb\":18}";

  auto run = [&](float key_level) {
    auto insert = sonare::mastering::api::make_insert(name, params);
    REQUIRE(insert != nullptr);
    insert->prepare(kRate, kLongBlock);
    const std::vector<float> key(kLongBlock, key_level);
    const float* key_channels[] = {key.data()};
    insert->set_sidechain(key_channels, 1, kLongBlock);
    std::vector<float> bed(kLongBlock, kBed);
    float* channels[] = {bed.data()};
    insert->process(channels, 1, kLongBlock);
    return bed;
  };

  const std::vector<float> flat(kLongBlock, kBed);
  CHECK(max_difference(run(0.0f), flat) < kBed * 1.0e-6f);
  const auto keyed = run(0.5f);
  CHECK(*std::min_element(keyed.begin(), keyed.end()) < kBed * 0.9f);
}

TEST_CASE("SidechainRouter an exhausted key stays bound and reads as silence",
          "[mastering][dynamics][boundary]") {
  const SidechainRouterConfig config = ducking_config();
  const std::vector<float> silence(kBlock, 0.0f);
  const float* key_channels[] = {silence.data()};

  auto run = [&](int key_samples, bool clear) {
    SidechainRouter router(config);
    router.prepare(kRate, kBlock);
    if (clear) {
      router.clear_sidechain();
    } else {
      router.set_sidechain(key_channels, 1, key_samples);
    }
    std::vector<float> bed(kBlock, kBed);
    float* channels[] = {bed.data()};
    router.process(channels, 1, kBlock);
    return bed;
  };

  const auto exhausted = run(0, false);
  const auto padded = run(kBlock, false);
  CHECK(max_difference(exhausted, padded) < kBed * 1.0e-5f);
  // A key with no channels unbinds: the bed is then detected against itself and ducks.
  const auto cleared = run(0, true);
  CHECK(*std::min_element(cleared.begin(), cleared.end()) < kBed * 0.9f);
  CHECK(*std::min_element(exhausted.begin(), exhausted.end()) > kBed * 0.99f);
}
