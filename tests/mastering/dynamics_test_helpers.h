/// @file dynamics_test_helpers.h
/// @brief Shared helpers for mastering dynamics tests.

#pragma once

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/ducking_processor.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::dynamics;

namespace {
using sonare::test::generate_sine_samples;
using sonare::test::max_abs_difference;
using sonare::test::peak_abs;
using sonare::test::process;
using sonare::test::process_stereo;
using sonare::test::rms_tail;

}  // namespace

namespace {

template <typename PublishFn, typename ProcessFn>
[[maybe_unused]] void run_rt_safe_race(PublishFn publish_one,
                                       ProcessFn process_one_block_returns_finite) {
  std::atomic<bool> stop{false};
  std::atomic<int> finite_blocks{0};
  std::atomic<int> nonfinite_blocks{0};
  auto total_blocks = [&] {
    return finite_blocks.load(std::memory_order_relaxed) +
           nonfinite_blocks.load(std::memory_order_relaxed);
  };
  std::thread audio_thread([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const bool finite = process_one_block_returns_finite();
      (finite ? finite_blocks : nonfinite_blocks).fetch_add(1, std::memory_order_relaxed);
    }
  });
  bool audio_thread_started = false;
  for (int spin = 0; spin < 10000; ++spin) {
    if (total_blocks() > 0) {
      audio_thread_started = true;
      break;
    }
    std::this_thread::yield();
  }
  for (int i = 0; i < 2000; ++i) {
    publish_one(i);
    if ((i & 0x0f) == 0) {
      std::this_thread::yield();
    }
  }
  bool observed_concurrent_blocks = false;
  for (int spin = 0; spin < 10000; ++spin) {
    if (total_blocks() > 1) {
      observed_concurrent_blocks = true;
      break;
    }
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  audio_thread.join();
  REQUIRE(audio_thread_started);
  REQUIRE(observed_concurrent_blocks);
  REQUIRE(finite_blocks.load() > 0);
  REQUIRE(nonfinite_blocks.load() == 0);
}

[[maybe_unused]] void fill_sine_block(std::vector<float>& block, float freq_hz, int sample_rate) {
  for (size_t k = 0; k < block.size(); ++k) {
    block[k] = 0.5f * static_cast<float>(
                          std::sin(sonare::constants::kTwoPiD * static_cast<double>(freq_hz) *
                                   static_cast<double>(k) / static_cast<double>(sample_rate)));
  }
}

[[maybe_unused]] bool all_finite(const std::vector<float>& block) {
  for (float s : block) {
    if (!std::isfinite(s)) return false;
  }
  return true;
}

}  // namespace
