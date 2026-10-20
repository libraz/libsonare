/// @file silence_boundaries_test.cpp
/// @brief Silence-boundary detection sees audio that lives past the last hop-aligned frame.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "effects/normalize.h"

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
