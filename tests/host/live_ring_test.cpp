#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

#include "host/live_input_ring.h"

namespace {

using sonare::host::LiveInputRing;

bool matches_sample(float actual, float expected) { return actual == Catch::Approx(expected); }

void push_stereo(LiveInputRing& ring, const float* left, const float* right, int frames) {
  const float* planes[] = {left, right};
  ring.push(planes, 2, frames);
}

void push_mono(LiveInputRing& ring, const float* mono, int frames) {
  const float* planes[] = {mono};
  ring.push(planes, 1, frames);
}

void push_stereo_in_blocks(LiveInputRing& ring, const float* left, const float* right, int frames,
                           int block_frames) {
  for (int offset = 0; offset < frames; offset += block_frames) {
    push_stereo(ring, left + offset, right + offset, std::min(block_frames, frames - offset));
  }
}

void push_mono_in_blocks(LiveInputRing& ring, const float* mono, int frames, int block_frames) {
  for (int offset = 0; offset < frames; offset += block_frames) {
    push_mono(ring, mono + offset, std::min(block_frames, frames - offset));
  }
}

void pop_stereo(LiveInputRing& ring, float* left, float* right, int frames) {
  float* planes[] = {left, right};
  ring.pop(planes, frames);
}

}  // namespace

TEST_CASE("Live input ring waits for its target and keeps planar blocks in order",
          "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, false);

  std::array<float, 12> left{};
  std::array<float, 12> right{};
  std::array<float, 4> ignored{};
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] = static_cast<float>(i);
    right[i] = static_cast<float>(100 + i);
  }
  ignored.fill(999.0f);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  const float* three_planes[] = {left.data(), right.data(), ignored.data()};
  ring.push(three_planes, 3, 4);
  out_left.fill(-999.0f);
  out_right.fill(-999.0f);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], 0.0f));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], 0.0f));
  }
  REQUIRE(ring.underrun_frames() == 0);
  REQUIRE(ring.starvation_frames() == 0);
  REQUIRE(ring.drift_repeat_frames() == 0);

  push_stereo(ring, left.data() + 4, right.data() + 4, 4);
  out_left.fill(-999.0f);
  out_right.fill(-999.0f);
  pop_stereo(ring, out_left.data(), out_right.data(), 2);
  REQUIRE(matches_sample(out_left[0], 0.0f));
  REQUIRE(matches_sample(out_right[1], 0.0f));

  push_stereo(ring, left.data() + 8, right.data() + 8, 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(100 + i)));
  }
  REQUIRE(ring.overrun_frames() == 0);
  REQUIRE(ring.underrun_frames() == 0);
}

TEST_CASE("Live input ring drops a whole block when space is insufficient", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, true);
  std::array<float, 40> left{};
  std::array<float, 40> right{};
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] = static_cast<float>(i);
    right[i] = static_cast<float>(100 + i);
  }
  push_stereo_in_blocks(ring, left.data(), right.data(), 32, 4);
  push_stereo(ring, left.data() + 32, right.data() + 32, 4);
  REQUIRE(ring.overrun_frames() == 4);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(100 + i)));
  }
  push_stereo(ring, left.data() + 36, right.data() + 36, 4);

  for (int offset = 4; offset < 32; offset += 4) {
    pop_stereo(ring, out_left.data(), out_right.data(), 4);
    for (int i = 0; i < 4; ++i) {
      REQUIRE(
          matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(offset + i)));
      REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)],
                             static_cast<float>(100 + offset + i)));
    }
  }
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(36 + i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(136 + i)));
  }
  REQUIRE(ring.overrun_frames() == 4);
}

TEST_CASE("Live input ring repeats the last frame at low water", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, false);

  std::array<float, 16> left{};
  std::array<float, 16> right{};
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] = static_cast<float>(i);
    right[i] = static_cast<float>(20 + i);
  }
  push_stereo_in_blocks(ring, left.data(), right.data(), 12, 4);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  REQUIRE(matches_sample(out_left[0], 7.0f));
  REQUIRE(matches_sample(out_left[1], 7.0f));
  REQUIRE(matches_sample(out_left[2], 7.0f));
  REQUIRE(matches_sample(out_left[3], 8.0f));
  REQUIRE(matches_sample(out_right[0], 27.0f));
  REQUIRE(matches_sample(out_right[1], 27.0f));
  REQUIRE(matches_sample(out_right[2], 27.0f));
  REQUIRE(matches_sample(out_right[3], 28.0f));
  REQUIRE(ring.underrun_frames() == 3);
  REQUIRE(ring.drift_repeat_frames() == 3);
  REQUIRE(ring.starvation_frames() == 0);

  push_stereo(ring, left.data() + 12, right.data() + 12, 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(28 + i)));
  }
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(8 + i)));
  }
  REQUIRE(ring.underrun_frames() == 4);
  REQUIRE(ring.drift_repeat_frames() == 4);
  REQUIRE(ring.starvation_frames() == 0);
}

TEST_CASE("Live input ring makes progress during an unlocked input stall", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, false);

  std::array<float, 12> mono{};
  for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = static_cast<float>(i);
  push_mono_in_blocks(ring, mono.data(), 12, 4);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);

  for (int block = 0; block < 4; ++block) {
    pop_stereo(ring, out_left.data(), out_right.data(), 4);
    const float repeated = static_cast<float>(7 + block);
    for (int i = 0; i < 3; ++i) {
      REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], repeated));
    }
    REQUIRE(matches_sample(out_left[3], static_cast<float>(8 + block)));
  }
  REQUIRE(ring.underrun_frames() == 12);
  REQUIRE(ring.drift_repeat_frames() == 12);
  REQUIRE(ring.starvation_frames() == 0);

  out_left.fill(-999.0f);
  out_right.fill(-999.0f);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], 0.0f));
  REQUIRE(ring.underrun_frames() == 16);
  REQUIRE(ring.starvation_frames() == 4);
  REQUIRE(ring.drift_repeat_frames() == 12);
}

TEST_CASE("Live input ring drops queued frames above high water", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, false);

  std::array<float, 24> left{};
  std::array<float, 24> right{};
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] = static_cast<float>(i);
    right[i] = static_cast<float>(40 + i);
  }
  push_stereo_in_blocks(ring, left.data(), right.data(), 12, 4);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  push_stereo_in_blocks(ring, left.data() + 12, right.data() + 12, 12, 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);

  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(8 + i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(48 + i)));
  }
  REQUIRE(ring.overrun_frames() == 4);
}

TEST_CASE("Live input ring leaves matched-clock frames untouched", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, true);

  std::array<float, 12> left{};
  std::array<float, 12> right{};
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] = static_cast<float>(i);
    right[i] = static_cast<float>(30 + i);
  }
  push_stereo_in_blocks(ring, left.data(), right.data(), 12, 4);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 3);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(7 + i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(37 + i)));
  }
  REQUIRE(ring.overrun_frames() == 0);
  REQUIRE(ring.underrun_frames() == 0);
}

TEST_CASE("Live input ring duplicates mono and handles physical full safely",
          "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, true);

  std::array<float, 34> mono{};
  for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = static_cast<float>(i);
  push_mono_in_blocks(ring, mono.data(), static_cast<int>(mono.size()), 4);
  REQUIRE(ring.overrun_frames() == 2);

  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  std::array<float, 4> extra{};
  for (int i = 0; i < 4; ++i) extra[static_cast<std::size_t>(i)] = static_cast<float>(34 + i);
  push_mono(ring, extra.data(), 4);
  push_mono(ring, extra.data(), 2);

  REQUIRE(ring.overrun_frames() == 4);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(4 + i)));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], static_cast<float>(4 + i)));
  }
}

TEST_CASE("Live input ring counts starvation after its first successful prime",
          "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(2, 2, true);

  std::array<float, 6> first{};
  for (std::size_t i = 0; i < first.size(); ++i) first[i] = static_cast<float>(i + 1);
  push_mono_in_blocks(ring, first.data(), 6, 2);

  std::array<float, 8> out_left{};
  std::array<float, 8> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 8);
  for (int i = 0; i < 6; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], static_cast<float>(i + 1)));
  }
  REQUIRE(matches_sample(out_left[6], 6.0f));
  REQUIRE(matches_sample(out_left[7], 6.0f));
  REQUIRE(ring.underrun_frames() == 2);
  REQUIRE(ring.starvation_frames() == 2);
  REQUIRE(ring.drift_repeat_frames() == 0);

  out_left.fill(-999.0f);
  out_right.fill(-999.0f);
  pop_stereo(ring, out_left.data(), out_right.data(), 2);
  REQUIRE(matches_sample(out_left[0], 0.0f));
  REQUIRE(matches_sample(out_left[1], 0.0f));
  REQUIRE(ring.underrun_frames() == 4);
  REQUIRE(ring.starvation_frames() == 4);

  std::array<float, 2> next{8.0f, 9.0f};
  std::array<float, 4> more{10.0f, 11.0f, 12.0f, 13.0f};
  push_mono(ring, next.data(), 2);
  pop_stereo(ring, out_left.data(), out_right.data(), 1);
  REQUIRE(matches_sample(out_left[0], 0.0f));
  REQUIRE(ring.underrun_frames() == 5);
  REQUIRE(ring.starvation_frames() == 5);
  push_mono_in_blocks(ring, more.data(), 4, 2);
  pop_stereo(ring, out_left.data(), out_right.data(), 2);
  REQUIRE(matches_sample(out_left[0], 8.0f));
  REQUIRE(matches_sample(out_left[1], 9.0f));
  REQUIRE(ring.underrun_frames() == 5);
  REQUIRE(ring.starvation_frames() == 5);
}

TEST_CASE("Live input ring resets counters and priming on prepare", "[host][live][ring]") {
  LiveInputRing ring;
  ring.prepare(4, 4, true);

  std::array<float, 34> mono{};
  push_mono_in_blocks(ring, mono.data(), static_cast<int>(mono.size()), 4);
  std::array<float, 4> out_left{};
  std::array<float, 4> out_right{};
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  std::array<float, 4> extra{};
  push_mono(ring, extra.data(), 4);
  push_mono(ring, extra.data(), 2);
  REQUIRE(ring.overrun_frames() == 4);

  ring.prepare(4, 4, false);
  REQUIRE(ring.overrun_frames() == 0);
  REQUIRE(ring.underrun_frames() == 0);
  REQUIRE(ring.starvation_frames() == 0);
  REQUIRE(ring.drift_repeat_frames() == 0);
  out_left.fill(-999.0f);
  out_right.fill(-999.0f);
  pop_stereo(ring, out_left.data(), out_right.data(), 4);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(matches_sample(out_left[static_cast<std::size_t>(i)], 0.0f));
    REQUIRE(matches_sample(out_right[static_cast<std::size_t>(i)], 0.0f));
  }
}

TEST_CASE("Live input ring preserves frames across independent IO threads", "[host][live][ring]") {
  constexpr int kBlockFrames = 1024;
  constexpr int kSeedFrames = 4096;
  LiveInputRing ring;
  ring.prepare(kBlockFrames, kBlockFrames, true);

  std::array<float, kSeedFrames + kBlockFrames> seed_left{};
  std::array<float, kSeedFrames + kBlockFrames> seed_right{};
  for (int i = 0; i < kSeedFrames + kBlockFrames; ++i) {
    seed_left[static_cast<std::size_t>(i)] = static_cast<float>(i);
    seed_right[static_cast<std::size_t>(i)] = static_cast<float>(1000 + i);
  }
  push_stereo(ring, seed_left.data(), seed_right.data(), kSeedFrames);

  std::atomic<int> ready{0};
  std::array<float, kBlockFrames> output_left{};
  std::array<float, kBlockFrames> output_right{};

  std::thread producer([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    push_stereo(ring, seed_left.data() + kSeedFrames, seed_right.data() + kSeedFrames,
                kBlockFrames);
  });

  std::thread consumer([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    pop_stereo(ring, output_left.data(), output_right.data(), kBlockFrames);
  });

  producer.join();
  consumer.join();

  for (int i = 0; i < kBlockFrames; ++i) {
    REQUIRE(matches_sample(output_left[static_cast<std::size_t>(i)], static_cast<float>(i)));
    REQUIRE(
        matches_sample(output_right[static_cast<std::size_t>(i)], static_cast<float>(1000 + i)));
  }
  std::array<float, kBlockFrames> tail_left{};
  std::array<float, kBlockFrames> tail_right{};
  pop_stereo(ring, tail_left.data(), tail_right.data(), kBlockFrames);
  for (int i = 0; i < kBlockFrames; ++i) {
    const float sample = static_cast<float>(kBlockFrames + i);
    REQUIRE(matches_sample(tail_left[static_cast<std::size_t>(i)], sample));
    REQUIRE(matches_sample(tail_right[static_cast<std::size_t>(i)], 1000.0f + sample));
  }
  REQUIRE(ring.overrun_frames() == 0);
  REQUIRE(ring.underrun_frames() == 0);
}
