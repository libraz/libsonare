#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "mastering/api/chain.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::mastering::api::MasteringChain;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::api::StreamingLoudnessGain;
using sonare::mastering::api::StreamingMasteringChain;
using sonare::mastering::api::StreamingMasteringChainOptions;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr std::size_t kLength = 2 * kSampleRate;

// Two tones with a deterministic noise floor; @p amplitude sets the peak scale.
std::vector<float> make_program(float amplitude, float phase_offset) {
  std::vector<float> samples(kLength);
  std::uint32_t state = 12345u;
  for (std::size_t i = 0; i < kLength; ++i) {
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] =
        amplitude * (0.5f * std::sin(sonare::constants::kTwoPi * 220.0f * t + phase_offset) +
                     0.3f * std::sin(sonare::constants::kTwoPi * 1760.0f * t) + 0.05f * noise);
  }
  return samples;
}

// Scales so the largest sample sits at @p peak (linear).
std::vector<float> normalized_to_peak(std::vector<float> samples, float peak) {
  float maximum = 0.0f;
  for (const float value : samples) maximum = std::max(maximum, std::abs(value));
  for (float& value : samples) value *= peak / maximum;
  return samples;
}

float peak_of(const std::vector<std::vector<float>>& channels) {
  float maximum = 0.0f;
  for (const auto& channel : channels) {
    for (const float value : channel) maximum = std::max(maximum, std::abs(value));
  }
  return maximum;
}

// Runs the streaming chain over the whole input and returns the latency-aligned output.
std::vector<std::vector<float>> render_streaming(const MasteringChainConfig& config,
                                                 const StreamingLoudnessGain& gain,
                                                 std::vector<std::vector<float>> input) {
  StreamingMasteringChainOptions options;
  options.loudness_static_gain_db = gain.loudness_static_gain_db;
  options.loudness_static_gain_peak_db = gain.true_peak_db;
  StreamingMasteringChain chain(config, options);
  const int channel_count = static_cast<int>(input.size());
  chain.prepare(kSampleRate, kBlockSize, channel_count);

  const std::size_t length = input[0].size();
  std::vector<std::vector<float>> output(input.size());
  std::vector<float*> pointers(input.size());
  for (std::size_t offset = 0; offset < length; offset += kBlockSize) {
    const int count = static_cast<int>(std::min<std::size_t>(kBlockSize, length - offset));
    for (std::size_t c = 0; c < input.size(); ++c) pointers[c] = input[c].data() + offset;
    chain.process_block(pointers.data(), channel_count, count);
    for (std::size_t c = 0; c < input.size(); ++c) {
      output[c].insert(output[c].end(), pointers[c], pointers[c] + count);
    }
  }
  std::vector<std::vector<float>> tail(input.size(), std::vector<float>(kBlockSize));
  for (;;) {
    for (std::size_t c = 0; c < input.size(); ++c) pointers[c] = tail[c].data();
    const int written = chain.flush(pointers.data(), channel_count, kBlockSize);
    if (written == 0) break;
    for (std::size_t c = 0; c < input.size(); ++c) {
      output[c].insert(output[c].end(), tail[c].begin(), tail[c].begin() + written);
    }
  }
  const std::size_t latency = static_cast<std::size_t>(chain.latency_samples());
  for (auto& channel : output) {
    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(latency));
    channel.resize(length);
  }
  return output;
}

float max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float maximum = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) maximum = std::max(maximum, std::abs(a[i] - b[i]));
  return maximum;
}

// Streaming output matches the offline render to a fraction of the output's own
// peak: the two limiters see the same signal block-wise versus whole-signal.
constexpr float kRenderToleranceOfPeak = 0.02f;

MasteringChainConfig loudness_config(float target_lufs, float ceiling_db) {
  MasteringChainConfig config;
  config.loudness.enabled = true;
  config.loudness.target_lufs = target_lufs;
  config.loudness.ceiling_db = ceiling_db;
  return config;
}

MasteringChainConfig staged_config() {
  MasteringChainConfig config = loudness_config(-14.0f, -1.0f);
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 3.0f;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.threshold_db = -30.0f;
  config.dynamics.compressor.config.ratio = 4.0f;
  config.dynamics.compressor.config.makeup_gain_db = 6.0f;
  return config;
}

MasteringChainConfig clamped_config() {
  MasteringChainConfig config = loudness_config(-6.0f, -1.0f);
  config.loudness.max_limiter_gain_reduction_db = 0.0f;
  return config;
}

}  // namespace

TEST_CASE("streaming loudness gain equals the gain the offline chain applies (mono)",
          "[mastering][chain][streaming]") {
  struct Case {
    const char* name;
    MasteringChainConfig config;
    std::vector<float> samples;
    bool clamps;
  };
  std::vector<Case> cases;
  cases.push_back(
      {"loudness only", loudness_config(-14.0f, -1.0f), make_program(0.03f, 0.0f), false});
  cases.push_back({"stages before loudness", staged_config(), make_program(0.05f, 0.0f), false});
  cases.push_back({"clamp-hitting", clamped_config(),
                   normalized_to_peak(make_program(1.0f, 0.0f), 0.9f), true});

  for (const auto& test_case : cases) {
    DYNAMIC_SECTION(test_case.name) {
      MasteringChain offline(test_case.config);
      const auto rendered =
          offline.process_mono(test_case.samples.data(), test_case.samples.size(), kSampleRate);
      const StreamingLoudnessGain gain = sonare::mastering::api::streaming_loudness_gain_mono(
          test_case.config, test_case.samples.data(), test_case.samples.size(), kSampleRate);

      CHECK(gain.loudness_static_gain_db == rendered.applied_gain_db);
      CHECK(rendered.loudness_target_limited == test_case.clamps);
      CHECK(std::isfinite(gain.integrated_lufs));
      CHECK(std::isfinite(gain.true_peak_db));

      const auto streamed = render_streaming(test_case.config, gain, {test_case.samples});
      const float tolerance = kRenderToleranceOfPeak * peak_of({rendered.samples});
      CHECK(max_abs_difference(streamed[0], rendered.samples) <= tolerance);
    }
  }
}

TEST_CASE("streaming loudness gain measures after the stages that precede loudness",
          "[mastering][chain][streaming]") {
  const MasteringChainConfig config = staged_config();
  const auto samples = make_program(0.05f, 0.0f);
  const StreamingLoudnessGain gain = sonare::mastering::api::streaming_loudness_gain_mono(
      config, samples.data(), samples.size(), kSampleRate);

  MasteringChainConfig no_stages = loudness_config(-14.0f, -1.0f);
  const StreamingLoudnessGain source_gain = sonare::mastering::api::streaming_loudness_gain_mono(
      no_stages, samples.data(), samples.size(), kSampleRate);

  // The compressor's makeup gain moves the stage input, so the two measurements differ.
  CHECK(gain.integrated_lufs > source_gain.integrated_lufs + 1.0f);
  CHECK(gain.loudness_static_gain_db < source_gain.loudness_static_gain_db - 1.0f);
}

TEST_CASE("streaming loudness gain equals the gain the offline chain applies (stereo)",
          "[mastering][chain][streaming]") {
  struct Case {
    const char* name;
    MasteringChainConfig config;
    float amplitude;
    float peak;
  };
  const Case cases[] = {{"loudness only", loudness_config(-14.0f, -1.0f), 0.03f, 0.0f},
                        {"stages before loudness", staged_config(), 0.05f, 0.0f},
                        {"clamp-hitting", clamped_config(), 1.0f, 0.9f}};
  for (const auto& test_case : cases) {
    DYNAMIC_SECTION(test_case.name) {
      std::vector<float> left = make_program(test_case.amplitude, 0.0f);
      std::vector<float> right = make_program(test_case.amplitude, 1.3f);
      if (test_case.peak > 0.0f) {
        left = normalized_to_peak(left, test_case.peak);
        right = normalized_to_peak(right, test_case.peak);
      }
      MasteringChain offline(test_case.config);
      const auto rendered = offline.process_stereo(left.data(), right.data(), kLength, kSampleRate);
      const StreamingLoudnessGain gain = sonare::mastering::api::streaming_loudness_gain_stereo(
          test_case.config, left.data(), right.data(), kLength, kSampleRate);

      CHECK(gain.loudness_static_gain_db == rendered.applied_gain_db);

      const auto streamed = render_streaming(test_case.config, gain, {left, right});
      const float tolerance = kRenderToleranceOfPeak * peak_of({rendered.left, rendered.right});
      CHECK(max_abs_difference(streamed[0], rendered.left) <= tolerance);
      CHECK(max_abs_difference(streamed[1], rendered.right) <= tolerance);
    }
  }
}

TEST_CASE("streaming loudness gain of silence is 0 dB, as offline",
          "[mastering][chain][streaming]") {
  const MasteringChainConfig config = staged_config();
  const std::vector<float> silence(kLength, 0.0f);

  MasteringChain offline(config);
  const auto rendered = offline.process_mono(silence.data(), silence.size(), kSampleRate);
  const StreamingLoudnessGain mono = sonare::mastering::api::streaming_loudness_gain_mono(
      config, silence.data(), silence.size(), kSampleRate);
  CHECK(mono.loudness_static_gain_db == 0.0f);
  CHECK(rendered.applied_gain_db == 0.0f);
  CHECK_FALSE(std::isfinite(mono.integrated_lufs));
  CHECK(mono.true_peak_db == sonare::constants::kFloorDb);

  const StreamingLoudnessGain stereo = sonare::mastering::api::streaming_loudness_gain_stereo(
      config, silence.data(), silence.data(), silence.size(), kSampleRate);
  CHECK(stereo.loudness_static_gain_db == 0.0f);
  CHECK_FALSE(std::isfinite(stereo.integrated_lufs));

  // The helper's numbers construct a streaming chain without the not-finite throw.
  CHECK_NOTHROW(render_streaming(config, mono, {silence}));
}

TEST_CASE("streaming loudness gain rejects an invalid configuration",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config = loudness_config(-14.0f, -1.0f);
  config.loudness.true_peak_oversample = 3;
  const auto samples = make_program(0.03f, 0.0f);
  CHECK_THROWS_AS(sonare::mastering::api::streaming_loudness_gain_mono(config, samples.data(),
                                                                       samples.size(), kSampleRate),
                  sonare::SonareException);
}
