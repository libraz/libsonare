#include "mastering/dynamics/parallel_comp.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "util/db.h"

namespace {

using sonare::mastering::dynamics::ParallelComp;
using sonare::mastering::dynamics::ParallelCompConfig;

constexpr double kSampleRate = 48000.0;

uint32_t float_bits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

template <typename Processor>
void process_planar(Processor& processor, std::vector<std::vector<float>>& planes,
                    int block_size = 256) {
  REQUIRE(!planes.empty());
  const size_t sample_count = planes.front().size();
  for (const auto& plane : planes) REQUIRE(plane.size() == sample_count);
  for (size_t offset = 0; offset < sample_count; offset += static_cast<size_t>(block_size)) {
    const int count =
        static_cast<int>(std::min<size_t>(static_cast<size_t>(block_size), sample_count - offset));
    std::vector<float*> channels;
    channels.reserve(planes.size());
    for (auto& plane : planes) channels.push_back(plane.data() + offset);
    processor.process(channels.data(), static_cast<int>(channels.size()), count);
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

std::vector<std::vector<float>> make_signal(int num_channels, int num_samples) {
  std::vector<std::vector<float>> signal(static_cast<size_t>(num_channels),
                                         std::vector<float>(static_cast<size_t>(num_samples)));
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const float magnitude =
          0.08f + 0.03f * static_cast<float>(ch + 1) + 0.01f * static_cast<float>(i % 7);
      const float sign = ((i + ch) % 3 == 0) ? -1.0f : 1.0f;
      signal[static_cast<size_t>(ch)][static_cast<size_t>(i)] = sign * magnitude;
    }
  }
  if (num_channels > 1) {
    // Make the excluded LFE plane materially louder so detector exclusion is
    // observable in the linked path.
    signal.back()[3] = 1.5f;
  }
  return signal;
}

float reference_gain_reduction(float input_db, const ParallelCompConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) return 0.0f;
  const float over_db = input_db - config.threshold_db;
  return -over_db * (1.0f - 1.0f / config.ratio);
}

std::vector<std::vector<float>> reference_without_limiter(
    const std::vector<std::vector<float>>& input, const ParallelCompConfig& config,
    int excluded_channel) {
  auto output = input;
  const int num_channels = static_cast<int>(input.size());
  const int num_samples = static_cast<int>(input.front().size());
  for (int i = 0; i < num_samples; ++i) {
    if (config.linked_detection) {
      float linked_level = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        if (ch != excluded_channel) {
          linked_level = std::max(linked_level,
                                  std::abs(input[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
        }
      }
      const float reduction_db =
          reference_gain_reduction(sonare::linear_to_db(linked_level), config);
      const float gain = sonare::db_to_linear(reduction_db + config.makeup_gain_db);
      for (int ch = 0; ch < num_channels; ++ch) {
        const float dry = input[static_cast<size_t>(ch)][static_cast<size_t>(i)];
        const float compressed = dry * gain;
        output[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
            dry * (1.0f - config.mix) + compressed * config.mix;
      }
    } else {
      for (int ch = 0; ch < num_channels; ++ch) {
        const float dry = input[static_cast<size_t>(ch)][static_cast<size_t>(i)];
        const float reduction_db =
            reference_gain_reduction(sonare::linear_to_db(std::abs(dry)), config);
        const float compressed = dry * sonare::db_to_linear(reduction_db + config.makeup_gain_db);
        output[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
            dry * (1.0f - config.mix) + compressed * config.mix;
      }
    }
  }
  return output;
}

// A hand-written reference rounds differently from the processor across compilers and
// architectures, so it agrees within a few ulps of the plane's peak, not bit for bit.
float reference_tolerance(const std::vector<float>& expected) {
  float scale = 0.0f;
  for (const float sample : expected) scale = std::max(scale, std::abs(sample));
  return 64.0f * std::numeric_limits<float>::epsilon() * std::max(scale, 1.0e-6f);
}

void check_matches_reference(const std::vector<float>& actual, const std::vector<float>& expected) {
  REQUIRE(actual.size() == expected.size());
  const float tolerance = reference_tolerance(expected);
  for (size_t i = 0; i < actual.size(); ++i) {
    INFO("sample=" << i << " actual=" << actual[i] << " expected=" << expected[i]);
    CHECK(std::abs(actual[i] - expected[i]) <= tolerance);
  }
}

void check_matches_reference(const std::vector<std::vector<float>>& actual,
                             const std::vector<std::vector<float>>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t ch = 0; ch < actual.size(); ++ch) {
    INFO("channel=" << ch);
    check_matches_reference(actual[ch], expected[ch]);
  }
}

std::vector<float> limiter_recovery_trace(int block_size, float release_ms,
                                          bool output_limiter = true) {
  ParallelCompConfig config;
  config.threshold_db = 100.0f;
  config.ratio = 1.0f;
  config.attack_ms = 0.0f;
  config.release_ms = release_ms;
  config.makeup_gain_db = 0.0f;
  config.mix = 1.0f;
  config.linked_detection = false;
  config.output_limiter = output_limiter;
  config.output_ceiling_db = 0.0f;

  ParallelComp processor(config);
  processor.prepare(kSampleRate, block_size);
  std::vector<float> samples = {2.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  std::vector<std::vector<float>> planes;
  planes.push_back(samples);
  process_planar(processor, planes, block_size);
  return planes.front();
}

}  // namespace

TEST_CASE("ParallelComp linked gain matches the reference across layouts and LFE exclusion",
          "[mastering][dynamics][parallel-comp]") {
  for (const int num_channels : {1, 2, 6, 8}) {
    for (const bool linked : {true, false}) {
      for (const float mix : {0.0f, 0.5f, 1.0f}) {
        ParallelCompConfig config;
        config.threshold_db = -24.0f;
        config.ratio = 4.0f;
        config.attack_ms = 0.0f;
        config.release_ms = 0.0f;
        config.makeup_gain_db = 3.0f;
        config.mix = mix;
        config.linked_detection = linked;
        config.output_limiter = false;

        ParallelComp processor(config);
        processor.prepare(kSampleRate, 64);
        processor.set_detector_excluded_channel(num_channels - 1);
        auto actual = make_signal(num_channels, 17);
        const auto expected =
            reference_without_limiter(actual, config, linked ? num_channels - 1 : -1);
        process_planar(processor, actual, 17);

        INFO("channels=" << num_channels << " linked=" << linked << " mix=" << mix);
        check_matches_reference(actual, expected);
        for (const auto& plane : actual) {
          CHECK(std::all_of(plane.begin(), plane.end(),
                            [](float sample) { return std::isfinite(sample); }));
        }
        CHECK(std::isfinite(processor.last_gain_reduction_db()));
        CHECK(processor.non_finite_discard_count() == 0u);
      }
    }
  }
}

TEST_CASE("ParallelComp limiter recovery follows the release recurrence",
          "[mastering][dynamics][parallel-comp]") {
  const auto actual = limiter_recovery_trace(32, 100.0f);
  auto expected = std::vector<float>{2.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  const float coeff = std::exp(-1.0f / (0.001f * 100.0f * static_cast<float>(kSampleRate)));
  float gain = 1.0f;
  for (float& sample : expected) {
    const float magnitude = std::abs(sample);
    const float target = magnitude > 1.0f ? 1.0f / magnitude : 1.0f;
    if (target <= gain) {
      gain = target;
    } else {
      gain = target - (target - gain) * coeff;
      if (gain > target) gain = target;
    }
    sample *= gain;
  }

  check_matches_reference(actual, expected);
  for (const float sample : actual) CHECK(std::isfinite(sample));
  CHECK(actual.front() == 1.0f);
  for (size_t i = 2; i < actual.size(); ++i) {
    INFO("sample=" << i);
    CHECK(actual[i] > actual[i - 1]);
  }

  const auto block_one = limiter_recovery_trace(1, 100.0f);
  REQUIRE(block_one.size() == actual.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    INFO("block-one sample=" << i);
    CHECK(float_bits(block_one[i]) == float_bits(actual[i]));
  }

  const auto instant = limiter_recovery_trace(32, 0.0f);
  CHECK(instant.front() == 1.0f);
  for (size_t i = 1; i < instant.size(); ++i) CHECK(instant[i] == 0.5f);
}

TEST_CASE("ParallelComp limiter off and parameter segments remain active controls",
          "[mastering][dynamics][parallel-comp]") {
  ParallelCompConfig config;
  config.threshold_db = 100.0f;
  config.ratio = 4.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.mix = 1.0f;
  config.linked_detection = false;
  config.output_limiter = false;
  ParallelComp processor(config);
  processor.prepare(kSampleRate, 8);

  std::vector<std::vector<float>> block(1, std::vector<float>(8, 0.75f));
  process_planar(processor, block, 8);
  const auto bypassed = block.front();
  CHECK(std::all_of(bypassed.begin(), bypassed.end(),
                    [](float sample) { return float_bits(sample) == float_bits(0.75f); }));

  REQUIRE(processor.set_parameter(0, -30.0f));
  block.assign(1, std::vector<float>(8, 0.75f));
  process_planar(processor, block, 8);
  const auto compressed = block.front();
  CHECK(compressed.front() != bypassed.front());
  CHECK(compressed.front() < bypassed.front());

  REQUIRE(processor.set_parameter(4, 6.0f));
  CHECK(processor.config().makeup_gain_db == 6.0f);
  block.assign(1, std::vector<float>(8, 0.75f));
  process_planar(processor, block, 8);
  CHECK(block.front().front() != compressed.front());

  REQUIRE(processor.set_parameter(3, 100.0f));
  CHECK(processor.config().release_ms == 100.0f);
  block.assign(1, std::vector<float>(8, 0.5f));
  process_planar(processor, block, 8);
  CHECK(std::all_of(block.front().begin(), block.front().end(),
                    [](float sample) { return std::isfinite(sample); }));

  auto limiter_config = processor.config();
  limiter_config.output_limiter = true;
  limiter_config.output_ceiling_db = -6.0f;
  processor.set_config(limiter_config);
  block.assign(1, std::vector<float>(8, 0.5f));
  process_planar(processor, block, 8);
  CHECK(std::all_of(block.front().begin(), block.front().end(),
                    [](float sample) { return std::isfinite(sample); }));

  limiter_config.linked_detection = true;
  processor.set_config(limiter_config);
  block.assign(1, std::vector<float>(8, 0.5f));
  process_planar(processor, block, 1);
  CHECK(std::all_of(block.front().begin(), block.front().end(),
                    [](float sample) { return std::isfinite(sample); }));
}

TEST_CASE("ParallelComp preserves limiter state for an accepted subnormal ceiling under FTZ",
          "[mastering][dynamics][parallel-comp]") {
  constexpr float kSubnormalCeilingDb = -800.0f;
  const float accepted_ceiling = sonare::db_to_linear(kSubnormalCeilingDb);
  REQUIRE(accepted_ceiling > 0.0f);
  CHECK(std::fpclassify(accepted_ceiling) == FP_SUBNORMAL);

  ParallelCompConfig unlinked_config;
  unlinked_config.threshold_db = 100.0f;
  unlinked_config.ratio = 1.0f;
  unlinked_config.attack_ms = 0.0f;
  unlinked_config.release_ms = 0.0f;
  unlinked_config.mix = 1.0f;
  unlinked_config.linked_detection = false;
  unlinked_config.output_limiter = true;
  unlinked_config.output_ceiling_db = kSubnormalCeilingDb;
  ParallelComp unlinked(unlinked_config);
  CHECK(unlinked.config().output_ceiling_db == kSubnormalCeilingDb);
  unlinked.prepare(kSampleRate, 1);
  unlinked.set_config(unlinked_config);
  CHECK(unlinked.config().output_ceiling_db == kSubnormalCeilingDb);

  std::vector<std::vector<float>> unlinked_block(1, std::vector<float>{0.5f});
  process_planar(unlinked, unlinked_block, 1);
  CHECK(float_bits(unlinked_block.front().front()) == float_bits(0.5f));

  auto normal_unlinked_config = unlinked_config;
  normal_unlinked_config.output_ceiling_db = 0.0f;
  unlinked.set_config(normal_unlinked_config);
  unlinked_block.front()[0] = 0.5f;
  process_planar(unlinked, unlinked_block, 1);
  CHECK(float_bits(unlinked_block.front().front()) == float_bits(0.5f));

  auto linked_config = unlinked_config;
  linked_config.linked_detection = true;
  linked_config.output_ceiling_db = 0.0f;
  ParallelComp linked(linked_config);
  linked.prepare(kSampleRate, 1);
  std::vector<std::vector<float>> linked_block(2, std::vector<float>{2.0f});
  process_planar(linked, linked_block, 1);
  CHECK(float_bits(linked_block[0][0]) == float_bits(1.0f));
  CHECK(float_bits(linked_block[1][0]) == float_bits(1.0f));

  linked_config.output_ceiling_db = kSubnormalCeilingDb;
  linked.set_config(linked_config);
  linked_block[0][0] = 0.5f;
  linked_block[1][0] = 0.5f;
  process_planar(linked, linked_block, 1);
  // The invalid-under-FTZ ceiling leaves the helper's gain untouched, while
  // the linked path still applies the existing shared gain multiplication.
  CHECK(float_bits(linked_block[0][0]) == float_bits(0.25f));
  CHECK(float_bits(linked_block[1][0]) == float_bits(0.25f));

  linked_config.output_ceiling_db = 0.0f;
  linked.set_config(linked_config);
  linked_block[0][0] = 0.5f;
  linked_block[1][0] = 0.5f;
  process_planar(linked, linked_block, 1);
  CHECK(float_bits(linked_block[0][0]) == float_bits(0.5f));
  CHECK(float_bits(linked_block[1][0]) == float_bits(0.5f));
}

TEST_CASE("ParallelComp non-finite follower state is discarded and recovers",
          "[mastering][dynamics][parallel-comp]") {
  ParallelCompConfig config;
  config.threshold_db = 100.0f;
  config.ratio = 1.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 100.0f;
  config.mix = 1.0f;
  config.linked_detection = false;
  config.output_limiter = false;
  ParallelComp processor(config);
  processor.prepare(kSampleRate, 1);

  std::vector<float> poisoned = {std::numeric_limits<float>::quiet_NaN()};
  std::vector<std::vector<float>> planes;
  planes.push_back(poisoned);
  process_planar(processor, planes, 1);
  CHECK(std::isnan(planes.front().front()));
  CHECK(processor.non_finite_discard_count() == 1u);

  planes.front()[0] = 0.25f;
  process_planar(processor, planes, 1);
  CHECK(std::isfinite(planes.front().front()));
  CHECK(processor.non_finite_discard_count() == 1u);
}

TEST_CASE("ParallelComp limiter re-entry clears linked and unlinked histories",
          "[mastering][dynamics][boundary]") {
  for (const bool linked : {true, false}) {
    sonare::mastering::dynamics::ParallelCompConfig config;
    config.ratio = 1.0f;
    config.attack_ms = 0.0f;
    config.release_ms = 60.0f;
    config.mix = 1.0f;
    config.linked_detection = linked;
    config.output_limiter = true;
    config.output_ceiling_db = -20.0f;
    sonare::mastering::dynamics::ParallelComp subject(config);
    sonare::mastering::dynamics::ParallelComp control(config);
    subject.prepare(48000.0, 256);
    control.prepare(48000.0, 256);

    std::vector<float> warm_left(256, 0.8f);
    std::vector<float> warm_right(256, linked ? 0.8f : 0.4f);
    auto control_warm_left = warm_left;
    auto control_warm_right = warm_right;
    process_stereo(subject, warm_left, warm_right);
    process_stereo(control, control_warm_left, control_warm_right);

    auto bypassed = config;
    bypassed.output_limiter = false;
    subject.set_config(bypassed);
    std::vector<float> gap_left(48000, 0.0f);
    auto gap_right = gap_left;
    auto control_gap_left = gap_left;
    auto control_gap_right = gap_right;
    process_stereo(subject, gap_left, gap_right);
    process_stereo(control, control_gap_left, control_gap_right);

    subject.set_config(config);
    std::vector<float> reentry_left(256, 0.05f);
    std::vector<float> reentry_right(256, linked ? 0.05f : 0.02f);
    auto control_reentry_left = reentry_left;
    auto control_reentry_right = reentry_right;
    process_stereo(subject, reentry_left, reentry_right);
    process_stereo(control, control_reentry_left, control_reentry_right);

    INFO("linked=" << linked);
    REQUIRE(std::isfinite(reentry_left.front()));
    REQUIRE(std::abs(reentry_left.front() - control_reentry_left.front()) < 1.0e-4f);
    REQUIRE(subject.non_finite_discard_count() == 0u);
    REQUIRE(control.non_finite_discard_count() == 0u);
  }
}
