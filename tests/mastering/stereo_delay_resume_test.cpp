#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/stereo/binaural_panner.h"
#include "mastering/stereo/phase_align.h"

namespace {

using sonare::mastering::stereo::BinauralOutput;
using sonare::mastering::stereo::BinauralPanner;
using sonare::mastering::stereo::BinauralPannerConfig;
using sonare::mastering::stereo::PhaseAlign;
using sonare::mastering::stereo::PhaseAlignConfig;

template <typename Processor>
void process_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
}

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples) {
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
}

double magnitude(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float sample : samples) sum += std::abs(sample);
  return sum;
}

double distance(const std::vector<float>& left_a, const std::vector<float>& right_a,
                const std::vector<float>& left_b, const std::vector<float>& right_b) {
  double sum = 0.0;
  for (size_t i = 0; i < left_a.size(); ++i) {
    sum += std::abs(left_a[i] - left_b[i]) + std::abs(right_a[i] - right_b[i]);
  }
  return sum;
}

}  // namespace

TEST_CASE("PhaseAlign clears delayed history across a mono gap", "[stereo-delay-resume]") {
  for (bool delay_right : {false, true}) {
    for (float fraction : {0.0f, 0.5f}) {
      DYNAMIC_SECTION("delay " << (delay_right ? "right" : "left") << " by " << fraction) {
        const PhaseAlignConfig config{4, delay_right, fraction};
        PhaseAlign resumed(config);
        PhaseAlign fresh(config);
        PhaseAlign continuous(config);
        resumed.prepare(48000.0, 16);
        fresh.prepare(48000.0, 16);
        continuous.prepare(48000.0, 16);

        std::vector<float> warm_left{delay_right ? 0.0f : 1.0f};
        std::vector<float> warm_right{delay_right ? 1.0f : 0.0f};
        std::vector<float> control_warm_left = warm_left;
        std::vector<float> control_warm_right = warm_right;
        process_stereo(resumed, warm_left, warm_right);
        process_stereo(continuous, control_warm_left, control_warm_right);

        std::vector<float> mono_gap{0.0f};
        process_mono(resumed, mono_gap);
        std::vector<float> fresh_mono_gap{0.0f};
        process_mono(fresh, fresh_mono_gap);

        std::vector<float> control_gap_left{0.0f};
        std::vector<float> control_gap_right{0.0f};
        process_stereo(continuous, control_gap_left, control_gap_right);

        std::vector<float> resumed_left(8, 0.0f), resumed_right(8, 0.0f);
        std::vector<float> fresh_left(8, 0.0f), fresh_right(8, 0.0f);
        std::vector<float> control_left(8, 0.0f), control_right(8, 0.0f);
        process_stereo(resumed, resumed_left, resumed_right);
        process_stereo(fresh, fresh_left, fresh_right);
        process_stereo(continuous, control_left, control_right);

        const std::vector<float>& delayed_control = delay_right ? control_right : control_left;
        const double control_tail = magnitude(delayed_control);
        INFO("active control tail sum abs = " << control_tail);
        CHECK(control_tail > 0.01);
        CHECK(resumed_left == fresh_left);
        CHECK(resumed_right == fresh_right);
      }
    }
  }
}

TEST_CASE("PhaseAlign zero-delay bypass can activate its fractional path",
          "[stereo-delay-resume]") {
  for (bool delay_right : {false, true}) {
    DYNAMIC_SECTION("fractional path re-enters on " << (delay_right ? "right" : "left")) {
      PhaseAlign resumed({0, delay_right, 0.5f});
      PhaseAlign fresh({0, delay_right, 0.5f});
      PhaseAlign active_control({0, delay_right, 0.5f});
      resumed.prepare(48000.0, 8);
      fresh.prepare(48000.0, 8);
      active_control.prepare(48000.0, 8);

      std::vector<float> warm_left{delay_right ? 0.0f : 1.0f};
      std::vector<float> warm_right{delay_right ? 1.0f : 0.0f};
      std::vector<float> control_warm_left = warm_left;
      std::vector<float> control_warm_right = warm_right;
      process_stereo(resumed, warm_left, warm_right);
      process_stereo(active_control, control_warm_left, control_warm_right);

      REQUIRE(resumed.set_parameter(0, 0.0f));
      std::vector<float> bypass_left{0.25f}, bypass_right{-0.5f};
      process_stereo(resumed, bypass_left, bypass_right);
      CHECK(bypass_left[0] == 0.25f);
      CHECK(bypass_right[0] == -0.5f);

      REQUIRE(resumed.set_parameter(0, 0.5f));
      std::vector<float> resumed_left(8, 0.0f), resumed_right(8, 0.0f);
      std::vector<float> fresh_left(8, 0.0f), fresh_right(8, 0.0f);
      std::vector<float> control_left(8, 0.0f), control_right(8, 0.0f);
      process_stereo(resumed, resumed_left, resumed_right);
      process_stereo(fresh, fresh_left, fresh_right);
      process_stereo(active_control, control_left, control_right);

      const std::vector<float>& delayed_control = delay_right ? control_right : control_left;
      const double control_tail = magnitude(delayed_control);
      INFO("active control tail sum abs = " << control_tail);
      CHECK(control_tail > 0.01);
      CHECK(resumed_left == fresh_left);
      CHECK(resumed_right == fresh_right);
    }
  }
}

TEST_CASE("PhaseAlign keeps history across adjacent active blocks", "[stereo-delay-resume]") {
  const PhaseAlignConfig config{4, true, 0.5f};
  PhaseAlign split(config);
  PhaseAlign contiguous(config);
  split.prepare(48000.0, 16);
  contiguous.prepare(48000.0, 16);

  std::vector<float> split_left{0.0f}, split_right{1.0f};
  std::vector<float> full_left{0.0f}, full_right{1.0f};
  process_stereo(split, split_left, split_right);
  process_stereo(contiguous, full_left, full_right);

  std::vector<float> first_left(1, 0.0f), first_right(1, 0.0f);
  process_stereo(split, first_left, first_right);
  std::vector<float> rest_left(8, 0.0f), rest_right(8, 0.0f);
  process_stereo(split, rest_left, rest_right);

  std::vector<float> contiguous_zeros(9, 0.0f), contiguous_right(9, 0.0f);
  process_stereo(contiguous, contiguous_zeros, contiguous_right);
  first_right.insert(first_right.end(), rest_right.begin(), rest_right.end());
  CHECK(first_right == contiguous_right);
  CHECK(magnitude(first_right) > 0.01);
}

TEST_CASE("BinauralPanner clears histories across a mono gap for every output mix",
          "[stereo-delay-resume]") {
  const std::array<BinauralOutput, 2> outputs{BinauralOutput::kPhones, BinauralOutput::kSpeakers};
  const std::array<float, 3> mixes{0.0f, 0.5f, 1.0f};
  for (BinauralOutput output : outputs) {
    for (float dry_wet : mixes) {
      DYNAMIC_SECTION("output " << static_cast<int>(output) << " dryWet " << dry_wet) {
        BinauralPannerConfig config;
        config.azimuth_deg = 55.0f;
        config.output = output;
        config.dry_wet = dry_wet;
        BinauralPanner resumed(config);
        BinauralPanner fresh(config);
        BinauralPanner continuous(config);
        resumed.prepare(48000.0, 512);
        fresh.prepare(48000.0, 512);
        continuous.prepare(48000.0, 512);

        std::vector<float> warm_left{0.5f}, warm_right{-0.25f};
        std::vector<float> control_warm_left = warm_left;
        std::vector<float> control_warm_right = warm_right;
        process_stereo(resumed, warm_left, warm_right);
        process_stereo(continuous, control_warm_left, control_warm_right);

        std::vector<float> mono_gap{0.0f};
        process_mono(resumed, mono_gap);
        std::vector<float> fresh_mono_gap{0.0f};
        process_mono(fresh, fresh_mono_gap);

        std::vector<float> control_gap_left{0.0f}, control_gap_right{0.0f};
        process_stereo(continuous, control_gap_left, control_gap_right);

        std::vector<float> resumed_left(256, 0.0f), resumed_right(256, 0.0f);
        std::vector<float> fresh_left(256, 0.0f), fresh_right(256, 0.0f);
        std::vector<float> control_left(256, 0.0f), control_right(256, 0.0f);
        process_stereo(resumed, resumed_left, resumed_right);
        process_stereo(fresh, fresh_left, fresh_right);
        process_stereo(continuous, control_left, control_right);

        const double control_tail = magnitude(control_left) + magnitude(control_right);
        INFO("active control output sum abs = "
             << control_tail << ", output=" << static_cast<int>(output) << ", dryWet=" << dry_wet);
        CHECK(control_tail > 1.0e-5);
        CHECK(resumed_left == fresh_left);
        CHECK(resumed_right == fresh_right);
      }
    }
  }
}

TEST_CASE("BinauralPanner keeps history across adjacent active blocks", "[stereo-delay-resume]") {
  for (BinauralOutput output : {BinauralOutput::kPhones, BinauralOutput::kSpeakers}) {
    DYNAMIC_SECTION("output " << static_cast<int>(output)) {
      BinauralPannerConfig config;
      config.azimuth_deg = 55.0f;
      config.output = output;
      BinauralPanner split(config);
      BinauralPanner contiguous(config);
      split.prepare(48000.0, 512);
      contiguous.prepare(48000.0, 512);

      std::vector<float> split_left{0.5f}, split_right{-0.25f};
      std::vector<float> full_left = split_left, full_right = split_right;
      process_stereo(split, split_left, split_right);
      process_stereo(contiguous, full_left, full_right);

      std::vector<float> first_left(128, 0.0f), first_right(128, 0.0f);
      process_stereo(split, first_left, first_right);
      std::vector<float> rest_left(128, 0.0f), rest_right(128, 0.0f);
      process_stereo(split, rest_left, rest_right);
      std::vector<float> all_left(256, 0.0f), all_right(256, 0.0f);
      process_stereo(contiguous, all_left, all_right);

      first_left.insert(first_left.end(), rest_left.begin(), rest_left.end());
      first_right.insert(first_right.end(), rest_right.begin(), rest_right.end());
      CHECK(first_left == all_left);
      CHECK(first_right == all_right);
      CHECK(magnitude(all_left) + magnitude(all_right) > 1.0e-5);
    }
  }
}

TEST_CASE("Binaural mono gaps pause an azimuth glide without resetting its position",
          "[stereo-delay-resume]") {
  BinauralPannerConfig initial;
  initial.azimuth_deg = 0.0f;
  initial.output = BinauralOutput::kPhones;
  BinauralPanner resumed(initial);
  resumed.prepare(48000.0, 256);

  std::vector<float> prime_left{0.0f}, prime_right{0.0f};
  process_stereo(resumed, prime_left, prime_right);
  REQUIRE(resumed.set_parameter(0, 90.0f));
  std::vector<float> mono_gap{0.0f};
  process_mono(resumed, mono_gap);

  BinauralPanner at_start(initial);
  BinauralPanner at_target(BinauralPannerConfig{90.0f});
  at_start.prepare(48000.0, 256);
  at_target.prepare(48000.0, 256);
  std::vector<float> expected_start_left(256, 0.0f), expected_start_right(256, 0.0f);
  std::vector<float> expected_target_left(256, 0.0f), expected_target_right(256, 0.0f);
  expected_start_left[0] = expected_start_right[0] = 1.0f;
  expected_target_left[0] = expected_target_right[0] = 1.0f;
  process_stereo(at_start, expected_start_left, expected_start_right);
  process_stereo(at_target, expected_target_left, expected_target_right);

  std::vector<float> resumed_left(256, 0.0f), resumed_right(256, 0.0f);
  resumed_left[0] = resumed_right[0] = 1.0f;
  process_stereo(resumed, resumed_left, resumed_right);
  const double reference_distance = distance(expected_start_left, expected_start_right,
                                             expected_target_left, expected_target_right);
  const double start_distance =
      distance(resumed_left, resumed_right, expected_start_left, expected_start_right);
  const double target_distance =
      distance(resumed_left, resumed_right, expected_target_left, expected_target_right);
  INFO("distance to start, target, and between references = "
       << start_distance << " / " << target_distance << " / " << reference_distance);
  CHECK(reference_distance > 1.0e-5);
  CHECK(start_distance < target_distance);
  CHECK(target_distance > 1.0e-5);
}
