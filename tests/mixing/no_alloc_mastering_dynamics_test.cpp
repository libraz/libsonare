/// @file no_alloc_mastering_dynamics_test.cpp
/// @brief Mastering dynamics no-allocation realtime tests.

#include "no_alloc_test_helpers.h"

TEST_CASE("Gate channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Gate gate({-50.0f, 2.0f, 80.0f, -80.0f, 0.0f, -50.0f, 120.0f});
  gate.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  gate.process(mono, 1, kBlock);
  AllocationGuard guard;
  gate.process(stereo, 2, kBlock);
  gate.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("TransientShaper channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::TransientShaper shaper;
  shaper.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.0f;
    right[static_cast<size_t>(i)] = i == 8 ? 0.5f : 0.0f;
  }
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  shaper.process(mono, 1, kBlock);
  AllocationGuard guard;
  shaper.process(stereo, 2, kBlock);
  shaper.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("DeEsser channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::DeEsser deesser;
  deesser.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  deesser.process(mono, 1, kBlock);
  AllocationGuard guard;
  deesser.process(stereo, 2, kBlock);
  deesser.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Compressor channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Compressor compressor;
  compressor.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  compressor.process(mono, 1, kBlock);
  AllocationGuard guard;
  compressor.process(stereo, 2, kBlock);
  compressor.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Expander channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::Expander expander;
  expander.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  expander.process(mono, 1, kBlock);
  AllocationGuard guard;
  expander.process(stereo, 2, kBlock);
  expander.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("UpwardCompressor channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::UpwardCompressor compressor;
  compressor.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  compressor.process(mono, 1, kBlock);
  AllocationGuard guard;
  compressor.process(stereo, 2, kBlock);
  compressor.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("UpwardExpander channel-count changes perform no heap allocation after prepare",
          "[mastering][dynamics][rt]") {
  constexpr int kBlock = 128;
  sonare::mastering::dynamics::UpwardExpander expander;
  expander.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.02f);
  float* mono[] = {left.data()};
  float* stereo[] = {left.data(), right.data()};

  expander.process(mono, 1, kBlock);
  AllocationGuard guard;
  expander.process(stereo, 2, kBlock);
  expander.process(mono, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

// ============================================================================
// P0-D regression tests: ChannelStrip insert/automation cap enforcement
// ============================================================================

TEST_CASE("ChannelStrip schedule_insert_automation enforces kMaxInsertAutomationLanes cap",
          "[mixing][rt-safety]") {
  // Regression guard for P0-D: insert_automation_ is reserved in the
  // constructor for kMaxInsertAutomationLanes entries so that control-thread
  // push_back never triggers a reallocation that would race with the audio
  // thread iterating the same vector.  This test exhausts the cap via the
  // public control-thread API and confirms that:
  //   (a) all lanes up to the cap succeed (return true), and
  //   (b) the N+1th lane returns false instead of reallocating.
  //
  // schedule_insert_automation requires an actual insert at the given
  // insert_index.  We add one ScaleProcessor so index 0 resolves.
  // Each distinct param_id maps to an independent AutomationTarget, creating
  // a new InsertAutomationLane entry (the de-dup path only merges identical
  // (insert_index, param_id) pairs).
  sonare::mixing::ChannelStrip strip;
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f));

  const size_t cap = sonare::mixing::ChannelStrip::kMaxInsertAutomationLanes;
  size_t success_count = 0;
  for (size_t i = 0; i < cap; ++i) {
    const unsigned int param_id = static_cast<unsigned int>(i);
    const bool ok = strip.schedule_insert_automation(0, param_id, 0, 0.5f);
    if (ok) {
      ++success_count;
    }
  }
  REQUIRE(success_count == cap);

  // The cap+1th call must be rejected (return false).
  const bool overflow =
      strip.schedule_insert_automation(0, static_cast<unsigned int>(cap), 0, 0.5f);
  REQUIRE_FALSE(overflow);
}

TEST_CASE("ChannelStrip add_insert enforces kMaxInserts cap", "[mixing][rt-safety]") {
  // Regression guard for P0-D: pre_inserts_ and post_inserts_ are reserved
  // to kMaxInserts in the constructor.  add_pre_insert / add_post_insert
  // must throw SonareException(InvalidState) on the N+1th insert rather than triggering
  // a push_back that reallocates behind the audio thread.
  sonare::mixing::ChannelStrip strip;

  const size_t cap = sonare::mixing::ChannelStrip::kMaxInserts;

  // Fill the strip with ScaleProcessor inserts split evenly across pre/post
  // to reach exactly kMaxInserts total (all pre for simplicity).
  for (size_t i = 0; i < cap; ++i) {
    REQUIRE_NOTHROW(strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f)));
  }
  REQUIRE(strip.num_pre_inserts() + strip.num_post_inserts() == cap);

  // The N+1th insert (pre or post) must throw.
  REQUIRE_THROWS_AS(strip.add_pre_insert(std::make_unique<ScaleProcessor>(1.0f)),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(strip.add_post_insert(std::make_unique<ScaleProcessor>(1.0f)),
                    sonare::SonareException);
}
