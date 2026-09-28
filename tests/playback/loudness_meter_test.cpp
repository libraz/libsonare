#include "playback/loudness_meter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "metering/lufs.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare::playback;

TEST_CASE("playback loudness meter agrees with the whole-buffer measurement",
          "[playback][loudness]") {
  constexpr int kRate = 48000;
  constexpr int kChannels = 6;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 5;
  std::mt19937 rng(21u);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (float& s : interleaved) s = dist(rng);

  const float reference =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, kChannels, kRate)
          .integrated_lufs;
  REQUIRE(std::isfinite(reference));

  PlaybackLoudnessMeter meter(kChannels, kRate);
  // Uneven chunks so the internal block boundary never lines up with a push.
  size_t offset = 0;
  const size_t chunks[] = {1, 1000, 4095, 4097, 12345};
  size_t k = 0;
  while (offset < kFrames) {
    const size_t n = std::min(chunks[k++ % 5], kFrames - offset);
    meter.push_interleaved(interleaved.data() + offset * kChannels, n);
    offset += n;
  }
  CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
}

TEST_CASE("playback loudness meter with mono channel", "[playback][loudness]") {
  constexpr int kRate = 48000;
  constexpr int kChannels = 1;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 2;
  std::mt19937 rng(42u);
  std::normal_distribution<float> dist(0.0f, 0.15f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (float& s : interleaved) s = dist(rng);

  const float reference =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, kChannels, kRate)
          .integrated_lufs;
  REQUIRE(std::isfinite(reference));

  PlaybackLoudnessMeter meter(kChannels, kRate);
  size_t offset = 0;
  const size_t chunks[] = {1, 777, 5000};
  size_t k = 0;
  while (offset < kFrames) {
    const size_t n = std::min(chunks[k++ % 3], kFrames - offset);
    meter.push_interleaved(interleaved.data() + offset, n);
    offset += n;
  }
  CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
}

TEST_CASE("playback loudness meter with stereo channel", "[playback][loudness]") {
  constexpr int kRate = 44100;
  constexpr int kChannels = 2;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 3;
  std::mt19937 rng(123u);
  std::normal_distribution<float> dist(0.0f, 0.12f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (float& s : interleaved) s = dist(rng);

  const float reference =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, kChannels, kRate)
          .integrated_lufs;
  REQUIRE(std::isfinite(reference));

  PlaybackLoudnessMeter meter(kChannels, kRate);
  size_t offset = 0;
  const size_t chunks[] = {1, 500, 4095, 8191};
  size_t k = 0;
  while (offset < kFrames) {
    const size_t n = std::min(chunks[k++ % 4], kFrames - offset);
    meter.push_interleaved(interleaved.data() + offset * kChannels, n);
    offset += n;
  }
  CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
}

TEST_CASE("playback loudness meter with 8 channels", "[playback][loudness]") {
  constexpr int kRate = 48000;
  constexpr int kChannels = 8;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 2;
  std::mt19937 rng(99u);
  std::normal_distribution<float> dist(0.0f, 0.08f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (float& s : interleaved) s = dist(rng);

  const float reference =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, kChannels, kRate)
          .integrated_lufs;
  REQUIRE(std::isfinite(reference));

  PlaybackLoudnessMeter meter(kChannels, kRate);
  size_t offset = 0;
  const size_t chunks[] = {1, 777, 5000};
  size_t k = 0;
  while (offset < kFrames) {
    const size_t n = std::min(chunks[k++ % 3], kFrames - offset);
    meter.push_interleaved(interleaved.data() + offset * kChannels, n);
    offset += n;
  }
  CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
}

TEST_CASE("playback loudness meter chunking invariance", "[playback][loudness]") {
  constexpr int kRate = 48000;
  constexpr int kChannels = 6;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 2;
  std::mt19937 rng(77u);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (float& s : interleaved) s = dist(rng);

  // Reference: push in a single call.
  PlaybackLoudnessMeter meter1(kChannels, kRate);
  meter1.push_interleaved(interleaved.data(), kFrames);
  const float result1 = meter1.integrated_lufs();

  // Alternative: push in varied chunks.
  PlaybackLoudnessMeter meter2(kChannels, kRate);
  size_t offset = 0;
  const size_t chunks[] = {1, 1500, 3000, 5000};
  size_t k = 0;
  while (offset < kFrames) {
    const size_t n = std::min(chunks[k++ % 4], kFrames - offset);
    meter2.push_interleaved(interleaved.data() + offset * kChannels, n);
    offset += n;
  }
  const float result2 = meter2.integrated_lufs();

  CHECK(std::abs(result1 - result2) < 1e-5f);
}

TEST_CASE("playback loudness meter rejects invalid channel count", "[playback][loudness]") {
  CHECK_THROWS_AS(PlaybackLoudnessMeter(0, 48000), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(3, 48000), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(4, 48000), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(5, 48000), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(7, 48000), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(9, 48000), sonare::SonareException);
}

TEST_CASE("playback loudness meter rejects invalid sample rate", "[playback][loudness]") {
  CHECK_THROWS_AS(PlaybackLoudnessMeter(2, 0), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackLoudnessMeter(2, -48000), sonare::SonareException);
}

TEST_CASE("playback loudness meter channels() method", "[playback][loudness]") {
  PlaybackLoudnessMeter meter1(1, 48000);
  CHECK(meter1.channels() == 1);

  PlaybackLoudnessMeter meter2(2, 44100);
  CHECK(meter2.channels() == 2);

  PlaybackLoudnessMeter meter6(6, 48000);
  CHECK(meter6.channels() == 6);

  PlaybackLoudnessMeter meter8(8, 48000);
  CHECK(meter8.channels() == 8);
}

TEST_CASE("playback loudness meter empty push", "[playback][loudness]") {
  using sonare::constants::kFloorDb;
  PlaybackLoudnessMeter meter(2, 48000);
  // Empty push should not crash or affect state.
  meter.push_interleaved(nullptr, 0);
  // Should still work after an empty push.
  std::vector<float> samples(4096 * 2, 0.0f);
  meter.push_interleaved(samples.data(), 4096);
  // Should return floor (no signal pushed yet, just silence).
  CHECK(meter.integrated_lufs() <= kFloorDb + 1.0f);
}

TEST_CASE("playback loudness meter measures the frames after the last internal block",
          "[playback][loudness]") {
  // 96000 frames end on a complete 400 ms gating block, and 96000 is not a
  // multiple of the internal block, so the loud tail sits past the last full
  // internal block. A meter that held those frames back would read the quiet
  // level instead.
  constexpr int kRate = 48000;
  constexpr int kChannels = 2;
  constexpr size_t kFrames = 96000;
  constexpr size_t kLoudFrames = kFrames % kPlaybackLoudnessBlockFrames;
  static_assert(kLoudFrames > 0);
  std::mt19937 rng(7u);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  std::vector<float> interleaved(kFrames * kChannels);
  for (size_t i = 0; i < kFrames; ++i) {
    const float gain = i >= kFrames - kLoudFrames ? 0.5f : 0.005f;
    for (int c = 0; c < kChannels; ++c) interleaved[i * kChannels + c] = gain * dist(rng);
  }

  const float reference =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, kChannels, kRate)
          .integrated_lufs;
  REQUIRE(std::isfinite(reference));

  PlaybackLoudnessMeter meter(kChannels, kRate);
  meter.push_interleaved(interleaved.data(), kFrames);
  CAPTURE(reference, meter.integrated_lufs());
  CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
}
