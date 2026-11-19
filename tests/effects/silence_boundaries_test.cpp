/// @file silence_boundaries_test.cpp
/// @brief Silence-boundary detection sees audio that lives past the last hop-aligned frame.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <limits>
#include <string>
#include <vector>

#include "effects/normalize.h"
#include "effects/silence.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr size_t kSamples = 1000;
constexpr size_t kSilentSamples = 600;
constexpr int kFrame = 512;
constexpr int kHop = 512;
constexpr float kThresholdDb = -60.0f;

/// Silence up to kSilentSamples, then full-scale samples: the only frame that reaches the
/// signal starts at kSamples - kFrame, which is not a multiple of the hop.
Audio tail_only_audio() {
  std::vector<float> samples(kSamples, 0.0f);
  for (size_t i = kSilentSamples; i < kSamples; ++i) samples[i] = 1.0f;
  return Audio::from_vector(std::move(samples), kSampleRate);
}

}  // namespace

TEST_CASE("detect_silence_boundaries finds audio past the last hop-aligned frame",
          "[normalize][silence]") {
  const auto [start, end] =
      detect_silence_boundaries(tail_only_audio(), kThresholdDb, kFrame, kHop);

  CHECK(start >= kSamples - kFrame);
  CHECK(start <= kSilentSamples);
  CHECK(end == kSamples);
}

TEST_CASE("trim_absolute removes leading silence the start scan cannot align to",
          "[normalize][silence]") {
  const Audio trimmed = trim_absolute(tail_only_audio(), kThresholdDb, kFrame, kHop);

  CHECK(trimmed.size() < kSamples);
  CHECK(trimmed.size() >= kSamples - kSilentSamples);
}

TEST_CASE("trim and split refusals read as core member names", "[silence]") {
  const std::vector<float> x(kSamples, 0.5f);
  const float* data = x.data();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  REQUIRE_THROWS_WITH(trim(data, 0, 60.0f, kFrame, kHop), "x must not be empty");
  REQUIRE_THROWS_WITH(trim(nullptr, kSamples, 60.0f, kFrame, kHop), "x must not be null");
  REQUIRE_THROWS_WITH(trim(data, kSamples, 60.0f, 0, kHop),
                      "frame_length must be an integer in [1, 2147483647]");
  REQUIRE_THROWS_WITH(split(data, kSamples, 60.0f, kFrame, 0),
                      "hop_length must be an integer in [1, 2147483647]");
  REQUIRE_THROWS_WITH(trim(data, kSamples, 0.0f, kFrame, kHop),
                      "top_db must be finite and greater than 0");
  std::vector<float> bad = x;
  bad[3] = nan;
  REQUIRE_THROWS_WITH(split(bad.data(), kSamples, 60.0f, kFrame, kHop),
                      "x contains a non-finite sample");

  const float* signals[1] = {data};
  const std::size_t lengths[1] = {kSamples};
  REQUIRE_THROWS_WITH(split_common_with_report(signals, lengths, 0, 60.0f, kFrame, kHop),
                      "signal_count must be an integer of at least 1");
  REQUIRE_THROWS_WITH(split_common_with_report(nullptr, lengths, 1, 60.0f, kFrame, kHop),
                      "signals must not be null");
  REQUIRE_THROWS_WITH(split_common_with_report(signals, nullptr, 1, 60.0f, kFrame, kHop),
                      "lengths must not be null");
  const float* with_empty[2] = {data, data};
  const std::size_t empty_lengths[2] = {kSamples, 0};
  REQUIRE_THROWS_WITH(split_common_with_report(with_empty, empty_lengths, 2, 60.0f, kFrame, kHop),
                      "signals[1] must not be empty");
}
