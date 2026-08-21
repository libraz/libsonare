/// @file rt_boundary_test.cpp
/// @brief Fixes the realtime boundary the mixing assistant sits outside of.
///
/// @details The assistant is offline / control-thread only and is deliberately
///          **not** covered by the `no_alloc_*` suites: it allocates by design,
///          runs an STFT per track and evaluates every track pair, and no
///          audio-thread entry point reaches it. Adding it to a no-alloc suite
///          would be asserting a property it does not have and must not be made
///          to have.
///
///          What does need fixing is the other direction. A suggestion is only
///          usable if the scene it produces still runs on the audio thread, so
///          this file renders a suggested scene through the mixer and checks
///          that the steady-state block loop allocates nothing. That catches a
///          suggestion which inserts a processor that is not realtime-safe —
///          the one way an offline decision can break the realtime path.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "mix_eval.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/suggester.h"
#include "support/alloc_guard.h"

namespace {

using sonare::test::AllocationGuard;

constexpr int kBlockSize = 512;
// Enough blocks for every lazily-sized buffer inside the graph, the inserts and
// the meters to have been touched once. The steady state is what is under test;
// the first block legitimately allocates.
constexpr int kWarmupBlocks = 8;
constexpr int kMeasuredBlocks = 8;

}  // namespace

TEST_CASE("a suggested scene runs the audio thread without allocating",
          "[mixing][assistant][.][slow]") {
  const auto fixture = sonare::mixing::assistant::test::make_demo_tracks();
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);

  const std::string json = sonare::mixing::api::scene_to_json(result.scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), fixture.sample_rate, kBlockSize);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);

  std::size_t longest = 0;
  for (const auto& track : tracks) longest = std::max(longest, track.frame_count);
  REQUIRE(longest >= static_cast<std::size_t>(kBlockSize) *
                         static_cast<std::size_t>(kWarmupBlocks + kMeasuredBlocks));

  std::vector<std::vector<float>> padded_left(tracks.size());
  std::vector<std::vector<float>> padded_right(tracks.size());
  std::vector<const float*> left_ptrs(tracks.size(), nullptr);
  std::vector<const float*> right_ptrs(tracks.size(), nullptr);
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    padded_left[index].assign(longest, 0.0f);
    padded_right[index].assign(longest, 0.0f);
    const auto& track = tracks[index];
    for (std::size_t frame = 0; frame < track.frame_count; ++frame) {
      const float sample = track.left != nullptr ? track.left[frame] : 0.0f;
      padded_left[index][frame] = sample;
      padded_right[index][frame] = track.right != nullptr ? track.right[frame] : sample;
    }
  }

  std::vector<float> out_left(static_cast<std::size_t>(kBlockSize), 0.0f);
  std::vector<float> out_right(static_cast<std::size_t>(kBlockSize), 0.0f);

  auto run_block = [&](int block) {
    const std::size_t offset =
        static_cast<std::size_t>(block) * static_cast<std::size_t>(kBlockSize);
    for (std::size_t index = 0; index < tracks.size(); ++index) {
      left_ptrs[index] = padded_left[index].data() + offset;
      right_ptrs[index] = padded_right[index].data() + offset;
    }
    return sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(), tracks.size(),
                                       out_left.data(), out_right.data(),
                                       static_cast<std::size_t>(kBlockSize));
  };

  for (int block = 0; block < kWarmupBlocks; ++block) {
    REQUIRE(run_block(block) == SONARE_OK);
  }

  std::size_t allocations = 0;
  {
    AllocationGuard guard;
    for (int block = kWarmupBlocks; block < kWarmupBlocks + kMeasuredBlocks; ++block) {
      if (run_block(block) != SONARE_OK) {
        allocations = guard.count();
        break;
      }
    }
    allocations = guard.count();
  }
  sonare_mixer_destroy(mixer);

  INFO("allocations observed in the steady-state block loop: " << allocations);
  REQUIRE(allocations == 0);
}

TEST_CASE("the assistant itself is not claimed to be allocation-free", "[mixing][assistant]") {
  // A positive control for the file above it. The assistant allocates by
  // design; if this ever came back as zero the guard would not be measuring
  // anything and the no-alloc assertion next door would be vacuous.
  const auto fixture = sonare::mixing::assistant::test::make_demo_tracks();
  const auto tracks = fixture.inputs();

  std::size_t allocations = 0;
  {
    AllocationGuard guard;
    const auto result = sonare::mixing::assistant::suggest_scene(tracks);
    (void)result;
    allocations = guard.count();
  }
  REQUIRE(allocations > 0);
}
