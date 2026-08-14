#include "engine/channel_delay.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "rt/delay_line.h"

namespace {

TEST_CASE("DelayLine zero delay has no physical storage", "[rt][delay]") {
  sonare::rt::DelayLine delay;
  REQUIRE(delay.capacity() == 0);

  delay.prepare(0);
  REQUIRE(delay.delay_samples() == 0);
  REQUIRE(delay.capacity() == 0);
  REQUIRE(delay.process(0.75f) == Catch::Approx(0.75f));

  delay.prepare(7);
  REQUIRE(delay.delay_samples() == 7);
  REQUIRE(delay.capacity() == 7);
  delay.prepare(0);
  REQUIRE(delay.capacity() == 0);
  REQUIRE(delay.process(-0.25f) == Catch::Approx(-0.25f));
}

TEST_CASE("ChannelDelay commits integer storage only for active lanes", "[engine][delay]") {
  sonare::engine::ChannelDelay<4> delay;
  REQUIRE(delay.configure(4, 5 << 8));
  REQUIRE(delay.prepared_channels() == 4);
  REQUIRE(delay.delay_q8() == (5 << 8));
  REQUIRE(delay.capacity() == 4u * 5u);

  std::array<float, 8> left{};
  std::array<float, 8> right{};
  float* channels[] = {left.data(), right.data()};
  left[0] = 1.0f;
  right[0] = -1.0f;
  delay.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(left[0] == 0.0f);
  REQUIRE(right[0] == 0.0f);
  REQUIRE(left[5] == Catch::Approx(1.0f));
  REQUIRE(right[5] == Catch::Approx(-1.0f));

  // Repreparing a narrower bank releases the superseded lanes physically.
  REQUIRE(delay.configure(2, 5 << 8));
  REQUIRE(delay.prepared_channels() == 2);
  REQUIRE(delay.capacity() == 2u * 5u);
}

TEST_CASE("ChannelDelay switches integer and fractional banks without retaining both",
          "[engine][delay]") {
  sonare::engine::ChannelDelay<4> delay;
  REQUIRE(delay.configure(2, 3 << 8));
  REQUIRE(delay.capacity() == 2u * 3u);

  constexpr int kFractionalDelay = (3 << 8) + 128;
  REQUIRE(delay.configure(2, kFractionalDelay));
  // The Lagrange stencil needs integer delay + four taps plus headroom; the
  // implementation's minimum is eight samples, so this request reserves 11.
  REQUIRE(delay.capacity() == 2u * 11u);

  REQUIRE(delay.configure(2, 0));
  REQUIRE(delay.delay_q8() == 0);
  REQUIRE(delay.capacity() == 0);

  std::array<float, 4> samples{1.0f, 0.0f, 0.0f, 0.0f};
  float* channels[] = {samples.data()};
  delay.process(channels, 1, static_cast<int>(samples.size()));
  REQUIRE(samples[0] == Catch::Approx(1.0f));
}

TEST_CASE("ChannelDelay releases inactive lanes and reset stays usable", "[engine][delay]") {
  sonare::engine::ChannelDelay<4> delay;
  REQUIRE(delay.configure(4, (2 << 8) + 64));
  REQUIRE(delay.capacity() == 4u * 10u);

  // A zero-channel commit releases every lane, including the fractional bank.
  REQUIRE(delay.configure(0, (2 << 8) + 64));
  REQUIRE(delay.prepared_channels() == 0);
  REQUIRE(delay.capacity() == 0);
  delay.reset();

  REQUIRE(delay.configure(1, 2 << 8));
  REQUIRE(delay.capacity() == 2);
  delay.reset();
}

TEST_CASE("ChannelDelay re-expands a reclaimed bank safely", "[engine][delay]") {
  constexpr int kChannels = 64;
  constexpr int kDelay = 3;
  sonare::engine::ChannelDelay<kChannels> delay;
  REQUIRE(delay.configure(2, kDelay << 8));
  REQUIRE(delay.capacity() == 2u * static_cast<size_t>(kDelay));

  // A later wide prepare must rebuild every lane that was released by the
  // narrow configuration; the audio path may then use all 64 planes safely.
  REQUIRE(delay.configure(kChannels, kDelay << 8));
  REQUIRE(delay.prepared_channels() == kChannels);
  REQUIRE(delay.capacity() == static_cast<size_t>(kChannels) * static_cast<size_t>(kDelay));

  std::array<std::array<float, 4>, kChannels> storage{};
  std::array<float*, kChannels> channels{};
  for (int ch = 0; ch < kChannels; ++ch) {
    storage[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    channels[static_cast<size_t>(ch)] = storage[static_cast<size_t>(ch)].data();
  }
  delay.process(channels.data(), kChannels, 4);
  for (int ch = 0; ch < kChannels; ++ch) {
    REQUIRE(storage[static_cast<size_t>(ch)][0] == 0.0f);
    REQUIRE(storage[static_cast<size_t>(ch)][3] == Catch::Approx(static_cast<float>(ch + 1)));
  }
}

}  // namespace
