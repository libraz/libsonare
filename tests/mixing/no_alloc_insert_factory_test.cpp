/// @file no_alloc_insert_factory_test.cpp
/// @brief No-allocation sweep over every processor the insert factory builds.
///
/// The hand-written no-alloc cases in this directory each name one processor,
/// so they cover whichever processors somebody thought to add and say nothing
/// about the rest. This file inverts that: it asks the insert factory for its
/// own list of names and drives every one of them, so a processor added to the
/// factory is covered the moment it can be constructed, and a processor that
/// starts allocating in process() is caught wherever it lives.
///
/// What it checks is the steady state: after prepare() and a warm-up block on
/// each channel count, no further process() call may reach the heap — including
/// the mono/stereo transition, which is where lazily sized per-channel scratch
/// has historically allocated. Construction and prepare() are control-thread
/// operations and allocate freely; only the audio-thread path is guarded.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "no_alloc_test_helpers.h"
#include "rt/processor_base.h"
#include "util/exception.h"

namespace {

constexpr int kBlock = 128;
constexpr double kSampleRate = 48000.0;

/// Musical-ish input: a mid band tone plus a quieter offset partial, so
/// detectors, crossovers and pitch-tracking inserts see energy rather than
/// silence and take their normal branches instead of an early-out.
void fill_signal(std::array<float, kBlock>& left, std::array<float, kBlock>& right) {
  for (int i = 0; i < kBlock; ++i) {
    const auto t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    const float tone = 0.25f * std::sin(6.2831853f * 440.0f * t);
    const float partial = 0.08f * std::sin(6.2831853f * 1330.0f * t);
    left[static_cast<size_t>(i)] = tone + partial;
    right[static_cast<size_t>(i)] = tone - partial;
  }
}

}  // namespace

TEST_CASE("Every insert-factory processor is allocation-free after prepare",
          "[mastering][mixing][rt][insert]") {
  const std::vector<std::string> names = sonare::mastering::api::insert_factory_names();
  // A build that dropped the factory would otherwise pass this test vacuously.
  REQUIRE(names.size() > 40);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  for (const std::string& name : names) {
    INFO("insert " << name);
    std::unique_ptr<sonare::rt::ProcessorBase> processor =
        sonare::mastering::api::make_insert(name, "{}");
    // Every name the factory publishes must be buildable in this configuration;
    // the list is already gated on the feature options that decide it.
    REQUIRE(processor != nullptr);

    processor->prepare(kSampleRate, kBlock, 2);

    // Warm-up outside the guard: a processor may size lazily on the first block
    // it sees at a given channel count, which is a control-plane cost, not an
    // audio-thread one. Both counts are warmed so the guarded pass below
    // measures the transition rather than first use.
    fill_signal(left, right);
    processor->process(stereo, 2, kBlock);

    // A processor that rejects mono (mid/side works on a stereo pair by
    // definition) never sees it on the audio thread either, so the transition
    // is not part of its contract. Probing here rather than listing names keeps
    // the sweep from encoding which processors those are.
    bool supports_mono = true;
    try {
      fill_signal(left, right);
      processor->process(mono, 1, kBlock);
    } catch (const sonare::SonareException&) {
      supports_mono = false;
    }

    {
      AllocationGuard guard;
      processor->process(stereo, 2, kBlock);
      if (supports_mono) {
        processor->process(mono, 1, kBlock);
      }
      processor->process(stereo, 2, kBlock);
      CHECK(guard.count() == 0);
    }
  }
}

TEST_CASE("Every insert-factory processor is allocation-free after reset",
          "[mastering][mixing][rt][insert]") {
  // reset() is issued from the audio thread on a transport stop / bypass edge
  // (rt::ProcessorBase::set_bypassed calls it directly), so it has the same
  // no-allocation obligation as process(): a processor that reset by dropping
  // and rebuilding its buffers would be RT-unsafe in exactly the place the
  // steady-state sweep above cannot see.
  const std::vector<std::string> names = sonare::mastering::api::insert_factory_names();
  REQUIRE(names.size() > 40);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* stereo[] = {left.data(), right.data()};

  for (const std::string& name : names) {
    INFO("insert " << name);
    std::unique_ptr<sonare::rt::ProcessorBase> processor =
        sonare::mastering::api::make_insert(name, "{}");
    REQUIRE(processor != nullptr);

    processor->prepare(kSampleRate, kBlock, 2);
    fill_signal(left, right);
    processor->process(stereo, 2, kBlock);
    processor->reset();
    processor->process(stereo, 2, kBlock);

    {
      AllocationGuard guard;
      processor->reset();
      processor->process(stereo, 2, kBlock);
      CHECK(guard.count() == 0);
    }
  }
}
