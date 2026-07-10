#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "engine/clip_player.h"

using Catch::Matchers::WithinAbs;

namespace {

// Renders a single clip in isolation and returns the per-sample output.
std::vector<float> render_clip(const sonare::engine::ClipSchedule& clip, int num_samples) {
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, num_samples);
  player.set_clips({clip});
  std::vector<float> out(static_cast<size_t>(num_samples), 0.0f);
  float* channels[] = {out.data()};
  player.process_at(channels, 1, num_samples, 0);
  return out;
}

}  // namespace

// An oversized fade-out must not drive the fade-out start negative. Without the
// clamp, fade_length - fade_out_samples < 0, so every sample falls inside the
// ramp and the whole clip attenuates. The player clamps the fade to the clip
// length, so the fade becomes a full-length fade-out (unity at the head) instead.
TEST_CASE("Oversized clip fade-out does not attenuate the whole clip", "[arrangement][clip_fade]") {
  constexpr int kLen = 8;
  std::array<float, kLen> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  // fade_out_samples (1000) far exceeds the clip length (8).
  sonare::engine::ClipSchedule clip{1, {channels, 1, kLen}, 0.0, 0, 0, kLen, false, 1.0f, 0, 1000};

  const std::vector<float> out = render_clip(clip, kLen);

  // The head of the clip stays at unity (the buggy path collapsed it to ~0.008,
  // i.e. 8/1000, because fade_start was -992 and every sample was in the ramp).
  REQUIRE_THAT(out[0], WithinAbs(1.0f, 1e-6f));
  // The fade is clamped to the clip, so it degrades to a full-length linear
  // fade-out: monotonically decreasing, still audible, ending near silence.
  for (int i = 1; i < kLen; ++i) {
    REQUIRE(out[i] < out[i - 1]);
    REQUIRE(out[i] > 0.0f);
  }
  REQUIRE_THAT(out[kLen - 1], WithinAbs(1.0f / static_cast<float>(kLen), 1e-6f));
}

// An oversized fade-in must likewise be clamped to the clip so the tail reaches
// its proper (clamped) level instead of the whole clip being held near silence.
TEST_CASE("Oversized clip fade-in is clamped to the clip length", "[arrangement][clip_fade]") {
  constexpr int kLen = 8;
  std::array<float, kLen> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  // fade_in_samples (1000) far exceeds the clip length (8).
  sonare::engine::ClipSchedule clip{1, {channels, 1, kLen}, 0.0, 0, 0, kLen, false, 1.0f, 1000, 0};

  const std::vector<float> out = render_clip(clip, kLen);

  // Clamped to the clip length the fade-in ramps up across the whole clip; the
  // last sample reaches (kLen-1)/kLen. The buggy path (denominator 1000) left it
  // near silence (~0.007).
  REQUIRE_THAT(out[0], WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(out[kLen - 1],
               WithinAbs(static_cast<float>(kLen - 1) / static_cast<float>(kLen), 1e-6f));
  for (int i = 1; i < kLen; ++i) {
    REQUIRE(out[i] > out[i - 1]);
  }
}

// A normal in-bounds fade is unchanged by the clamp. A 4-sample fade-out on an
// 8-sample clip leaves the first half at unity and ramps the rest.
TEST_CASE("In-bounds clip fade is unaffected by the length clamp", "[arrangement][clip_fade]") {
  constexpr int kLen = 8;
  std::array<float, kLen> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  sonare::engine::ClipSchedule clip{1, {channels, 1, kLen}, 0.0, 0, 0, kLen, false, 1.0f, 0, 4};

  const std::vector<float> out = render_clip(clip, kLen);

  // fade_start = 8 - 4 = 4: samples 0..3 are at unity, 4..7 ramp down.
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(out[i], WithinAbs(1.0f, 1e-6f));
  }
  for (int i = 5; i < kLen; ++i) {
    REQUIRE(out[i] < out[i - 1]);
  }
}
