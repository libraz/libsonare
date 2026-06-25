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

// ============================================================================
// H-1 regression: realtime automation of dynamics inserts must not allocate on
// the audio thread, and parameter_is_realtime_safe() must honestly report which
// parameters the mixing automation path may apply from the audio callback.
// ============================================================================

#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/vocal_rider.h"

TEST_CASE("Limiter set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The limiter reads derived scalars (threshold_db_ / release_coeff_) in its
  // per-sample loop, so set_parameter updates them in place with no shared_ptr
  // publish. Automating ceiling/release from the audio thread must not allocate,
  // and the parameters must report realtime-safe so the mixing path applies them.
  sonare::mastering::dynamics::Limiter limiter({-3.0f, 1.0f, 50.0f});
  limiter.prepare(48000.0, 128);
  REQUIRE(limiter.parameter_is_realtime_safe(0));
  REQUIRE(limiter.parameter_is_realtime_safe(1));
  {
    AllocationGuard guard;
    REQUIRE(limiter.set_parameter(0, -6.0f));
    REQUIRE(limiter.set_parameter(1, 80.0f));
    REQUIRE(guard.count() == 0);
  }
  // The in-place threshold update takes effect without a snapshot publish: a
  // full-scale block is limited toward the new -6 dB ceiling.
  std::array<float, 128> buf{};
  buf.fill(1.0f);
  float* chans[] = {buf.data()};
  limiter.process(chans, 1, 128);
  REQUIRE(buf[127] < 0.6f);  // ~ -6 dB ceiling (0.501) plus lookahead smoothing
}

TEST_CASE("Compressor set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The compressor reads a live working config (active_) in its per-sample loop;
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish, so audio-thread automation must not allocate, and every
  // parameter must report realtime-safe so the mixing path applies it.
  sonare::mastering::dynamics::Compressor compressor;
  compressor.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 12; ++id) {
    REQUIRE(compressor.parameter_is_realtime_safe(id));
  }
  {
    AllocationGuard guard;
    REQUIRE(compressor.set_parameter(0, -48.0f));  // threshold
    REQUIRE(compressor.set_parameter(1, 8.0f));    // ratio
    REQUIRE(compressor.set_parameter(7, 1.0f));    // detector mode
    REQUIRE(guard.count() == 0);
  }
  // In-place threshold/ratio drop takes effect without a publish: a loud tone is
  // compressed below its input level.
  std::array<float, 128> buf{};
  buf.fill(0.8f);
  float* chans[] = {buf.data()};
  compressor.process(chans, 1, 128);
  REQUIRE(std::abs(buf[127]) < 0.8f);
}

TEST_CASE("DeEsser set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The de-esser reads a live working config (active_) in process(); set_parameter
  // mutates active_ and re-derives coefficients in place with no snapshot publish,
  // so audio-thread automation must not allocate and every parameter must report
  // realtime-safe so the mixing path applies it.
  sonare::mastering::dynamics::DeEsser deesser;
  deesser.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 7; ++id) {
    REQUIRE(deesser.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  deesser.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(deesser.set_parameter(0, 7000.0f));  // frequency
    REQUIRE(deesser.set_parameter(1, -30.0f));   // threshold
    REQUIRE(deesser.set_parameter(2, 6.0f));     // ratio
    REQUIRE(guard.count() == 0);
  }
  // The in-place change is reflected in config() without a publish.
  REQUIRE(deesser.config().threshold_db == -30.0f);
}

TEST_CASE("Expander set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The expander reads a live working config (active_) in process(); set_parameter
  // mutates active_ and re-derives coefficients in place with no snapshot publish.
  sonare::mastering::dynamics::Expander expander;
  expander.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 5; ++id) {
    REQUIRE(expander.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  expander.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(expander.set_parameter(0, -36.0f));  // threshold
    REQUIRE(expander.set_parameter(1, 3.0f));    // ratio
    REQUIRE(expander.set_parameter(4, -48.0f));  // range
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(expander.config().threshold_db == -36.0f);
}

TEST_CASE("TransientShaper set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The transient shaper reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::TransientShaper shaper;
  shaper.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 9; ++id) {
    REQUIRE(shaper.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  shaper.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(shaper.set_parameter(0, 6.0f));   // attack gain
    REQUIRE(shaper.set_parameter(6, 0.5f));   // sensitivity
    REQUIRE(shaper.set_parameter(8, 10.0f));  // gain smoothing
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(shaper.config().attack_gain_db == 6.0f);
}

TEST_CASE("SidechainRouter set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The sidechain router reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::SidechainRouter router;
  router.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 5; ++id) {
    REQUIRE(router.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  router.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(router.set_parameter(0, -18.0f));  // threshold
    REQUIRE(router.set_parameter(1, 6.0f));    // ratio
    REQUIRE(router.set_parameter(4, 24.0f));   // range
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(router.config().threshold_db == -18.0f);
}

TEST_CASE("UpwardCompressor set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The upward compressor reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::UpwardCompressor compressor;
  compressor.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 5; ++id) {
    REQUIRE(compressor.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  compressor.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(compressor.set_parameter(0, -24.0f));  // threshold
    REQUIRE(compressor.set_parameter(1, 3.0f));    // ratio
    REQUIRE(compressor.set_parameter(4, 18.0f));   // range
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(compressor.config().threshold_db == -24.0f);
}

TEST_CASE("UpwardExpander set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The upward expander reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::UpwardExpander expander;
  expander.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 5; ++id) {
    REQUIRE(expander.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  expander.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(expander.set_parameter(0, -18.0f));  // threshold
    REQUIRE(expander.set_parameter(1, 2.0f));    // ratio
    REQUIRE(expander.set_parameter(4, 18.0f));   // range
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(expander.config().threshold_db == -18.0f);
}

TEST_CASE("VocalRider set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The vocal rider reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::VocalRider rider;
  rider.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 8; ++id) {
    REQUIRE(rider.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  rider.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(rider.set_parameter(0, -12.0f));  // target
    REQUIRE(rider.set_parameter(3, 30.0f));   // attack
    REQUIRE(rider.set_parameter(6, 50.0f));   // gain smoothing
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(rider.config().target_db == -12.0f);
}

TEST_CASE("ParallelComp set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // The parallel compressor reads a live working config (active_) in process();
  // set_parameter mutates active_ and re-derives coefficients in place with no
  // snapshot publish.
  sonare::mastering::dynamics::ParallelComp comp;
  comp.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 7; ++id) {
    REQUIRE(comp.parameter_is_realtime_safe(id));
  }
  std::array<float, 128> buf{};
  buf.fill(0.05f);
  float* chans[] = {buf.data()};
  comp.process(chans, 1, 128);  // adopt the initial snapshot before guarding
  {
    AllocationGuard guard;
    REQUIRE(comp.set_parameter(0, -24.0f));  // threshold
    REQUIRE(comp.set_parameter(1, 6.0f));    // ratio
    REQUIRE(comp.set_parameter(5, 0.7f));    // mix
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(comp.config().threshold_db == -24.0f);
}

TEST_CASE("Gate and BrickwallLimiter set_parameter automation performs no heap allocation",
          "[mastering][dynamics][rt]") {
  // Both read a live working config (active_) in process(); set_parameter mutates
  // it in place (gate re-derives coefficients; brickwall forwards ceiling/release
  // to the inner limiter's in-place setters) with no snapshot publish.
  sonare::mastering::dynamics::Gate gate({-50.0f, 2.0f, 80.0f, -80.0f, 0.0f, -50.0f, 120.0f});
  gate.prepare(48000.0, 128);
  for (unsigned int id = 0; id < 4; ++id) {
    REQUIRE(gate.parameter_is_realtime_safe(id));
  }
  sonare::mastering::dynamics::BrickwallLimiter brickwall;
  brickwall.prepare(48000.0, 128);
  REQUIRE(brickwall.parameter_is_realtime_safe(0));
  REQUIRE(brickwall.parameter_is_realtime_safe(1));
  {
    AllocationGuard guard;
    REQUIRE(gate.set_parameter(0, -40.0f));
    REQUIRE(gate.set_parameter(2, 120.0f));
    REQUIRE(brickwall.set_parameter(0, -3.0f));
    REQUIRE(brickwall.set_parameter(1, 100.0f));
    REQUIRE(guard.count() == 0);
  }
}
