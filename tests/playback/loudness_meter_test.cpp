#include "playback/loudness_meter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "metering/lufs.h"

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
