#include "playback/loudness_meter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
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

// push_interleaved is an offline (control-thread) entry point, so it holds the
// same empty/null/finite contract every other offline buffer-taking call in
// the library does.
TEST_CASE("playback loudness meter rejects an empty, null, or non-finite push",
          "[playback][loudness]") {
  PlaybackLoudnessMeter meter(2, 48000);
  CHECK_THROWS_AS(meter.push_interleaved(nullptr, 0), sonare::SonareException);
  std::vector<float> samples(4096 * 2, 0.0f);
  CHECK_THROWS_AS(meter.push_interleaved(nullptr, 4096), sonare::SonareException);
  CHECK_THROWS_AS(meter.push_interleaved(samples.data(), 0), sonare::SonareException);
  samples[9] = std::numeric_limits<float>::quiet_NaN();
  CHECK_THROWS_AS(meter.push_interleaved(samples.data(), 4096), sonare::SonareException);

  // Still usable afterwards: a refused push must not corrupt the running state.
  std::fill(samples.begin(), samples.end(), 0.0f);
  using sonare::constants::kFloorDb;
  meter.push_interleaved(samples.data(), 4096);
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

TEST_CASE("loudness stays finite at a rate whose Nyquist is below the K-weighting shelf",
          "[playback][loudness]") {
  // 1681 Hz is above 2 kHz audio's Nyquist, the rate the shelf used to diverge at.
  for (const int rate : {1000, 2000, 3000}) {
    constexpr int kChannels = 2;
    const size_t frames = static_cast<size_t>(rate) * 4;
    std::vector<float> interleaved(frames * kChannels);
    for (size_t i = 0; i < frames; ++i) {
      const float s = 0.25f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 200.0 *
                                                          static_cast<double>(i) / rate));
      interleaved[i * kChannels] = s;
      interleaved[i * kChannels + 1] = s;
    }
    INFO("rate " << rate);
    const auto offline =
        sonare::metering::lufs_interleaved(interleaved.data(), frames, kChannels, rate);
    REQUIRE(std::isfinite(offline.integrated_lufs));
    REQUIRE(std::isfinite(offline.momentary_lufs));
    REQUIRE(std::isfinite(offline.short_term_lufs));
    REQUIRE(std::isfinite(offline.max_momentary_lufs));
    REQUIRE(std::isfinite(offline.max_short_term_lufs));
    PlaybackLoudnessMeter meter(kChannels, rate);
    meter.push_interleaved(interleaved.data(), frames);
    REQUIRE(std::isfinite(meter.integrated_lufs()));
  }
}

TEST_CASE("playback loudness meter keeps material louder than the old histogram ceiling",
          "[playback][loudness]") {
  constexpr int kRate = 48000;
  constexpr size_t kFrames = static_cast<size_t>(kRate) * 3;
  for (const int channels : {6, 8}) {
    std::vector<float> interleaved(kFrames * static_cast<size_t>(channels));
    for (size_t frame = 0; frame < kFrames; ++frame) {
      // Near full scale where K-weighting lifts it: the planes together read well above +5 LUFS.
      const float sample =
          0.99f * static_cast<float>(std::sin(2.0 * sonare::constants::kPiD * 6000.0 *
                                              static_cast<double>(frame) / kRate));
      for (int ch = 0; ch < channels; ++ch) {
        interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(ch)] = sample;
      }
    }
    const float reference =
        sonare::metering::lufs_interleaved(interleaved.data(), kFrames, channels, kRate)
            .integrated_lufs;
    INFO(channels << " channels, offline " << reference);
    REQUIRE(reference > 5.0f);

    PlaybackLoudnessMeter meter(channels, kRate);
    meter.push_interleaved(interleaved.data(), kFrames);
    CHECK(std::abs(meter.integrated_lufs() - reference) <= 0.1f);
  }
}
