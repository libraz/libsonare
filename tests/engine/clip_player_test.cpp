#include "engine/clip_player.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "engine/tempo_sync.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {

class TestPagedProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  explicit TestPagedProvider(std::vector<float> samples, int64_t missing_sample = -1,
                             int64_t page_frames = 1)
      : samples_(std::move(samples)), missing_sample_(missing_sample), page_frames_(page_frames) {}

  int num_channels() const noexcept override { return 1; }
  int64_t num_samples() const noexcept override { return static_cast<int64_t>(samples_.size()); }
  int64_t page_frames() const noexcept override { return page_frames_; }

  bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
    if (channel != 0 || !out || sample < 0 || sample >= num_samples() ||
        sample == missing_sample_) {
      return false;
    }
    *out = samples_[static_cast<size_t>(sample)];
    return true;
  }

 private:
  std::vector<float> samples_;
  int64_t missing_sample_ = -1;
  int64_t page_frames_ = 1;
};

class TestPageRequestSink final : public sonare::engine::ClipPageRequestSink {
 public:
  void on_clip_page_miss(const sonare::engine::ClipPageRequest& request) noexcept override {
    if (count < requests.size()) {
      requests[count] = request;
    }
    ++count;
  }

  std::array<sonare::engine::ClipPageRequest, 8> requests{};
  size_t count = 0;
};

}  // namespace

TEST_CASE("ClipPlayer starts and stops on sample boundaries", "[engine][clip_player]") {
  std::array<float, 4> source_l{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> source_r{-1.0f, -2.0f, -3.0f, -4.0f};
  const float* source[] = {source_l.data(), source_r.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({{1, {source, 2, 4}, 0.0, 2, 0, 4, false, 1.0f, 0, 0}});

  std::array<float, 8> out_l{};
  std::array<float, 8> out_r{};
  float* out[] = {out_l.data(), out_r.data()};
  player.process_at(out, 2, 8, 0);

  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[1] == 0.0f);
  REQUIRE(out_l[2] == 1.0f);
  REQUIRE(out_l[5] == 4.0f);
  REQUIRE(out_l[6] == 0.0f);
  REQUIRE(out_r[2] == -1.0f);
}

TEST_CASE("ClipPlayer saturates clip and block ends at the int64 timeline boundary",
          "[engine][clip_player][validation]") {
  std::array<float, 4> source{1.0f, 2.0f, 3.0f, 4.0f};
  const float* source_channels[] = {source.data()};
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({{1, {source_channels, 1, 4}, 0.0, kMax - 2, 0, 10, false, 1.0f, 0, 0}});

  std::array<float, 8> output{};
  float* output_channels[] = {output.data()};
  player.process_at(output_channels, 1, 8, kMax - 4);
  CHECK(output[0] == 0.0f);
  CHECK(output[1] == 0.0f);
  CHECK(output[2] == 1.0f);
  CHECK(output[3] == 2.0f);
  CHECK(output[4] == 0.0f);

  sonare::engine::ClipBoundaryList boundaries;
  player.collect_boundaries(kMax - 4, 8, &boundaries);
  REQUIRE(boundaries.size == 2);
  CHECK(boundaries.offsets[0] == 2);
  CHECK(boundaries.offsets[1] == 4);
}

TEST_CASE("ClipPlayer can render tracks separately without changing summed output",
          "[engine][clip_player]") {
  std::array<float, 4> source_a{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> source_b{0.25f, 0.5f, 0.75f, 1.0f};
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipSchedule clip_a{1, {a, 1, 4}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip_a.track_id = 101;
  sonare::engine::ClipSchedule clip_b{2, {b, 1, 4}, 0.0, 1, 0, 4, false, 0.5f, 0, 0};
  clip_b.track_id = 202;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({clip_a, clip_b});

  std::array<float, 8> full{};
  float* full_channels[] = {full.data()};
  player.process_at(full_channels, 1, 8, 0);

  std::array<float, 8> lane_a{};
  std::array<float, 8> lane_b{};
  float* lane_a_channels[] = {lane_a.data()};
  float* lane_b_channels[] = {lane_b.data()};
  player.process_track_at(101, lane_a_channels, 1, 8, 0);
  player.process_track_at(202, lane_b_channels, 1, 8, 0);

  for (size_t i = 0; i < full.size(); ++i) {
    REQUIRE(full[i] == lane_a[i] + lane_b[i]);
  }
}

TEST_CASE("ClipPlayer mono live monitoring matches the mono bounce downmix",
          "[engine][clip_player]") {
  // A mono BOUNCE renders the panned stereo pair and downmixes 0.5*(L+R)
  // (project_bounce.cpp). Mono live monitoring must preview the same result so
  // a panned clip A/B'd between the two agrees in level and balance.
  std::array<float, 4> source{1.0f, 2.0f, 3.0f, 4.0f};
  const float* mono_src[] = {source.data()};

  auto stereo_downmix = [&](float pan) {
    sonare::engine::ClipSchedule clip{1, {mono_src, 1, 4}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
    clip.pan = pan;
    sonare::engine::ClipPlayer player;
    player.prepare(48000.0, 4);
    player.set_clips({clip});
    std::array<float, 4> l{};
    std::array<float, 4> r{};
    float* stereo[] = {l.data(), r.data()};
    player.process_at(stereo, 2, 4, 0);
    std::array<float, 4> down{};
    for (size_t i = 0; i < down.size(); ++i) down[i] = 0.5f * (l[i] + r[i]);
    return down;
  };

  auto mono_live = [&](float pan) {
    sonare::engine::ClipSchedule clip{1, {mono_src, 1, 4}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
    clip.pan = pan;
    sonare::engine::ClipPlayer player;
    player.prepare(48000.0, 4);
    player.set_clips({clip});
    std::array<float, 4> m{};
    float* mono[] = {m.data()};
    player.process_at(mono, 1, 4, 0);
    return m;
  };

  // Hard-right pan: bounce drops to -6 dB; live must follow, not stay at unity.
  const auto right_down = stereo_downmix(1.0f);
  const auto right_live = mono_live(1.0f);
  for (size_t i = 0; i < source.size(); ++i) {
    REQUIRE_THAT(right_live[i], WithinAbs(right_down[i], 1.0e-6f));
    REQUIRE_THAT(right_live[i], WithinAbs(0.5f * source[i], 1.0e-6f));
  }

  // Center pan stays at unity (the linear balance law leaves both channels at
  // 1.0, so 0.5*(L+R) collapses to the source read) — common case unchanged.
  const auto center_live = mono_live(0.0f);
  for (size_t i = 0; i < source.size(); ++i) {
    REQUIRE_THAT(center_live[i], WithinAbs(source[i], 1.0e-6f));
  }
}

TEST_CASE("ClipPlayer reads paged provider samples and silences page misses",
          "[engine][clip_player]") {
  auto provider = std::make_shared<TestPagedProvider>(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f},
                                                      /*missing_sample=*/2);

  sonare::engine::ClipSchedule clip{1, {}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.page_provider = provider;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 0);

  REQUIRE(out_l[0] == 1.0f);
  REQUIRE(out_l[1] == 2.0f);
  REQUIRE(out_l[2] == 0.0f);
  REQUIRE(out_l[3] == 4.0f);
}

TEST_CASE("ClipPlayer reports paged provider misses to a request sink", "[engine][clip_player]") {
  auto provider = std::make_shared<TestPagedProvider>(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f},
                                                      /*missing_sample=*/2);

  sonare::engine::ClipSchedule clip{77, {}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.page_provider = provider;

  TestPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_page_request_sink(&sink);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 0);

  REQUIRE(sink.count >= 1);
  REQUIRE(sink.requests[0].clip_id == 77);
  REQUIRE(sink.requests[0].channel == 0);
  REQUIRE(sink.requests[0].sample == 2);
}

TEST_CASE("ClipPlayer deduplicates paged provider misses across loop wraps",
          "[engine][clip_player]") {
  auto provider = std::make_shared<TestPagedProvider>(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f},
                                                      /*missing_sample=*/1);

  sonare::engine::ClipSchedule clip{88, {}, 0.0, 0, 0, 6, true, 1.0f, 0, 0};
  clip.loop_length_samples = 4;
  clip.page_provider = provider;

  TestPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 6);
  player.set_page_request_sink(&sink);
  player.set_clips({clip});

  std::array<float, 6> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 6, 0);

  REQUIRE(out_l[0] == 1.0f);
  REQUIRE(out_l[1] == 0.0f);
  REQUIRE(out_l[4] == 1.0f);
  REQUIRE(out_l[5] == 0.0f);
  REQUIRE(sink.count == 1);
  REQUIRE(sink.requests[0].clip_id == 88);
  REQUIRE(sink.requests[0].sample == 1);
}

TEST_CASE("ClipPlayer holds available paged samples across interpolation misses",
          "[engine][clip_player]") {
  auto provider = std::make_shared<TestPagedProvider>(
      std::vector<float>{10.0f, 20.0f, 30.0f, 40.0f}, /*missing_sample=*/2,
      /*page_frames=*/2);

  sonare::engine::ClipSchedule clip{89, {}, 0.0, 0, 0, 1, false, 1.0f, 0, 0};
  clip.page_provider = provider;
  clip.warp_mode = sonare::engine::WarpMode::kRepitch;
  clip.warp_anchors = std::make_shared<const std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{{0.0, 1.5}, {1.0, 2.5}});

  TestPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 1);
  player.set_page_request_sink(&sink);
  player.set_clips({clip});

  std::array<float, 1> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 1, 0);

  REQUIRE(out_l[0] == 20.0f);
  REQUIRE(sink.count == 1);
  REQUIRE(sink.requests[0].clip_id == 89);
  REQUIRE(sink.requests[0].sample == 2);
}

TEST_CASE("ClipPlayer reports paged provider misses after a timeline seek",
          "[engine][clip_player]") {
  auto provider = std::make_shared<TestPagedProvider>(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f},
                                                      /*missing_sample=*/2);

  sonare::engine::ClipSchedule clip{99, {}, 0.0, 10, 0, 4, false, 1.0f, 0, 0};
  clip.page_provider = provider;

  TestPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 2);
  player.set_page_request_sink(&sink);
  player.set_clips({clip});

  std::array<float, 2> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 2, 12);

  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[1] == 4.0f);
  REQUIRE(sink.count >= 1);
  REQUIRE(sink.requests[0].clip_id == 99);
  REQUIRE(sink.requests[0].sample == 2);
}

TEST_CASE("ClipPlayer loops source material and mixes overlapping clips", "[engine][clip_player]") {
  std::array<float, 2> source_a{1.0f, 2.0f};
  std::array<float, 4> source_b{10.0f, 10.0f, 10.0f, 10.0f};
  const float* a[] = {source_a.data()};
  const float* b[] = {source_b.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({{1, {a, 1, 2}, 0.0, 0, 0, 6, true, 1.0f, 0, 0},
                    {2, {b, 1, 4}, 0.0, 2, 0, 4, false, 0.5f, 0, 0}});

  std::array<float, 8> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 8, 0);

  REQUIRE(out_l[0] == 1.0f);
  REQUIRE(out_l[1] == 2.0f);
  REQUIRE(out_l[2] == 6.0f);
  REQUIRE(out_l[3] == 7.0f);
  REQUIRE(out_l[4] == 6.0f);
  REQUIRE(out_l[5] == 7.0f);
}

TEST_CASE("ClipPlayer applies linear fade in and fade out", "[engine][clip_player]") {
  std::array<float, 6> source{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({{1, {channels, 1, 6}, 0.0, 0, 0, 6, false, 1.0f, 2, 2}});

  std::array<float, 6> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 6, 0);

  REQUIRE_THAT(out_l[0], WithinAbs(0.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[1], WithinAbs(0.5f, 1.0e-6f));
  REQUIRE_THAT(out_l[2], WithinAbs(1.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[4], WithinAbs(1.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[5], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("ClipPlayer applies independent exponential and logarithmic fade curves",
          "[engine][clip_player]") {
  std::array<float, 6> source{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({{1,
                     {channels, 1, 6},
                     0.0,
                     0,
                     0,
                     6,
                     false,
                     1.0f,
                     2,
                     2,
                     sonare::engine::FadeCurve::Exponential,
                     sonare::engine::FadeCurve::Logarithmic,
                     true}});

  std::array<float, 6> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 6, 0);

  REQUIRE_THAT(out_l[1], WithinAbs(0.25f, 1.0e-6f));
  REQUIRE_THAT(out_l[5], WithinAbs(0.70710678f, 1.0e-6f));
}

TEST_CASE("ClipPlayer collects clip start and stop boundaries", "[engine][clip_player]") {
  std::array<float, 8> source{};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 16);
  player.set_clips({{1, {channels, 1, 8}, 0.0, 4, 0, 8, false, 1.0f, 0, 0}});

  sonare::engine::ClipBoundaryList boundaries;
  player.collect_boundaries(0, 16, &boundaries);

  REQUIRE(boundaries.size == 2);
  REQUIRE(boundaries.offsets[0] == 4);
  REQUIRE(boundaries.offsets[1] == 12);
  REQUIRE_FALSE(boundaries.overflowed);
}

TEST_CASE("ClipPlayer loop wraps mid-block from the correct source positions",
          "[engine][clip_player]") {
  // Source of 4 distinct samples; the clip loops over an 8-sample length so the
  // loop wrap point falls exactly in the middle of the processed block.
  std::array<float, 4> source{10.0f, 20.0f, 30.0f, 40.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  // start_sample=0, offset=0, length=8, loop=true, gain=1, no fades.
  player.set_clips({{1, {channels, 1, 4}, 0.0, 0, 0, 8, true, 1.0f, 0, 0}});

  std::array<float, 8> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 8, 0);

  // Positions 0..3 read source[0..3]; the wrap occurs at position 4 (block
  // middle) and positions 4..7 read source[0..3] again. Continuity must hold
  // across the wrap with no out-of-bounds reads.
  REQUIRE(out_l[0] == 10.0f);
  REQUIRE(out_l[1] == 20.0f);
  REQUIRE(out_l[2] == 30.0f);
  REQUIRE(out_l[3] == 40.0f);
  REQUIRE(out_l[4] == 10.0f);
  REQUIRE(out_l[5] == 20.0f);
  REQUIRE(out_l[6] == 30.0f);
  REQUIRE(out_l[7] == 40.0f);
}

TEST_CASE("ClipPlayer honors explicit audio loop length", "[engine][clip_player]") {
  std::array<float, 4> source{10.0f, 20.0f, 30.0f, 40.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipSchedule clip{1, {channels, 1, 4}, 0.0, 0, 0, 8, true, 1.0f, 0, 0};
  clip.loop_length_samples = 2;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({clip});

  std::array<float, 8> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 8, 0);

  REQUIRE(out_l[0] == 10.0f);
  REQUIRE(out_l[1] == 20.0f);
  REQUIRE(out_l[2] == 10.0f);
  REQUIRE(out_l[3] == 20.0f);
  REQUIRE(out_l[4] == 10.0f);
  REQUIRE(out_l[5] == 20.0f);
}

TEST_CASE("ClipPlayer equal-power crossfades the loop seam with pre-roll material",
          "[engine][clip_player]") {
  // Source layout: pre-roll [0,4) = {10,11,12,13}, looped region [4,8) = {20,21,22,23}.
  // clip_offset=4 makes [4,8) the loop body and leaves [0,4) as pre-roll for the
  // seam crossfade.
  std::array<float, 8> source{10.0f, 11.0f, 12.0f, 13.0f, 20.0f, 21.0f, 22.0f, 23.0f};
  const float* channels[] = {source.data()};

  auto render = [&](int64_t crossfade) {
    sonare::engine::ClipSchedule clip{1, {channels, 1, 8}, 0.0, 0, 4, 8, true, 1.0f, 0, 0};
    clip.loop_length_samples = 4;
    clip.loop_crossfade_samples = crossfade;

    sonare::engine::ClipPlayer player;
    player.prepare(48000.0, 8);
    player.set_clips({clip});

    std::array<float, 8> out_l{};
    float* out[] = {out_l.data()};
    player.process_at(out, 1, 8, 0);
    return out_l;
  };

  // Hard loop (crossfade 0): exact integer-modulo wrap, behavior unchanged.
  const auto hard = render(0);
  REQUIRE_THAT(hard[3], WithinAbs(23.0f, 1.0e-6f));
  REQUIRE_THAT(hard[7], WithinAbs(23.0f, 1.0e-6f));

  // Crossfade 2: the last two samples of each loop blend the tail with the
  // pre-roll. frac=0 at local==2 leaves it untouched; at local==3 (frac=0.5) it
  // is the equal-power mix cos(pi/4)*src[7] + sin(pi/4)*src[3].
  const auto faded = render(2);
  const float w = std::sin(sonare::constants::kHalfPi * 0.5f);  // == cos at frac 0.5
  REQUIRE_THAT(faded[0], WithinAbs(20.0f, 1.0e-6f));
  REQUIRE_THAT(faded[1], WithinAbs(21.0f, 1.0e-6f));
  REQUIRE_THAT(faded[2], WithinAbs(22.0f, 1.0e-6f));  // frac 0 -> pure tail
  REQUIRE_THAT(faded[3], WithinAbs(w * (23.0f + 13.0f), 1.0e-5f));
  REQUIRE_THAT(faded[7], WithinAbs(w * (23.0f + 13.0f), 1.0e-5f));
}

TEST_CASE("ClipPlayer loop crossfade falls back to a hard loop without pre-roll",
          "[engine][clip_player]") {
  // clip_offset=0 leaves no pre-roll, so the crossfade clamps to zero and the
  // loop stays a hard integer-modulo wrap regardless of loop_crossfade_samples.
  std::array<float, 4> source{20.0f, 21.0f, 22.0f, 23.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipSchedule clip{1, {channels, 1, 4}, 0.0, 0, 0, 8, true, 1.0f, 0, 0};
  clip.loop_length_samples = 4;
  clip.loop_crossfade_samples = 2;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);
  player.set_clips({clip});

  std::array<float, 8> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 8, 0);

  REQUIRE_THAT(out_l[3], WithinAbs(23.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[7], WithinAbs(23.0f, 1.0e-6f));
}

TEST_CASE("ClipPlayer repitch warp maps warped positions to source positions",
          "[engine][clip_player]") {
  std::array<float, 4> source{0.0f, 10.0f, 20.0f, 30.0f};
  const float* channels[] = {source.data()};
  auto anchors = std::make_shared<std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{{0.0, 0.0}, {3.0, 1.5}});

  sonare::engine::ClipSchedule clip{1, {channels, 1, 4}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kRepitch;
  clip.warp_anchors = anchors;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 0);

  REQUIRE_THAT(out_l[0], WithinAbs(0.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[1], WithinAbs(5.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[2], WithinAbs(10.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[3], WithinAbs(15.0f, 1.0e-6f));
}

TEST_CASE("ClipPlayer repitch warp on a mid-clip comp part does not double-offset the source",
          "[engine][clip_player]") {
  // A comp part that starts partway into its clip carries both clip_offset_samples
  // (the part's source start) and warp_reference_offset_samples (the part's
  // timeline offset from clip start). Under an identity warp map the warped read
  // must match the warp-off read, otherwise the offset is counted twice.
  std::array<float, 8> source{0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f};
  const float* channels[] = {source.data()};
  auto identity = std::make_shared<std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{{0.0, 0.0}, {8.0, 8.0}});

  // Part starts at timeline sample 2, reads source from index 2, plays 4 samples.
  sonare::engine::ClipSchedule clip{1, {channels, 1, 8}, 0.0, 2, 2, 4, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kRepitch;
  clip.warp_reference_offset_samples = 2;
  clip.warp_anchors = identity;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 2);

  // Without the fix this reads source[4..7] (40,50,60,70): clip_offset 2 added on
  // top of the absolute warp position map_warp(warp_ref 2 + position). Under the
  // identity map the warp curve already resolves the part's timeline to source
  // index warp_ref + position, so the read is source[2..5] with no extra offset.
  REQUIRE_THAT(out_l[0], WithinAbs(20.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[1], WithinAbs(30.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[2], WithinAbs(40.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[3], WithinAbs(50.0f, 1.0e-6f));
}

TEST_CASE("ClipPlayer repitch warp does not silence a comp part with a large clip offset",
          "[engine][clip_player]") {
  // A comp part repitch-warped from a take with a large source offset must play
  // the warped audio, not silence. The source-length gate previously subtracted
  // clip_offset_samples even under warp, so clip_offset_samples >= source length
  // drove source_len <= 0 and the clip returned silence. Under warp the read is
  // resolved entirely by the warp map, so clip_offset_samples must not gate it.
  std::array<float, 8> source{0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f};
  const float* channels[] = {source.data()};
  auto identity = std::make_shared<std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{{0.0, 0.0}, {8.0, 8.0}});

  // clip_offset_samples == source length (8). Without the fix source_len = 0.
  sonare::engine::ClipSchedule clip{1, {channels, 1, 8}, 0.0, 0, 8, 4, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kRepitch;
  clip.warp_reference_offset_samples = 0;
  clip.warp_anchors = identity;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 0);

  // Warp map is identity, warp_ref 0: source_pos = position, reads source[0..3].
  REQUIRE_THAT(out_l[0], WithinAbs(0.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[1], WithinAbs(10.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[2], WithinAbs(20.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[3], WithinAbs(30.0f, 1.0e-6f));
}

TEST_CASE("ClipPlayer repitch warp on a mid-clip comp part follows the absolute warp curve",
          "[engine][clip_player]") {
  // Same comp scenario but with a 2:1 stretch (8 warp samples map to 4 source
  // samples). The warp map is authoritative for the absolute source position, so
  // the read is map_warp(warp_ref + position) and clip_offset is not re-applied.
  std::array<float, 16> source{};
  for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i) * 10.0f;
  const float* channels[] = {source.data()};
  auto half_speed = std::make_shared<std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{{0.0, 0.0}, {8.0, 4.0}});

  sonare::engine::ClipSchedule clip{1, {channels, 1, 16}, 0.0, 4, 4, 4, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kRepitch;
  clip.warp_reference_offset_samples = 4;
  clip.warp_anchors = half_speed;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 4);

  // source_pos = map_warp(4 + position) = 0.5 * (4 + position) -> 2, 2.5, 3, 3.5.
  REQUIRE_THAT(out_l[0], WithinAbs(20.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[1], WithinAbs(25.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[2], WithinAbs(30.0f, 1.0e-6f));
  REQUIRE_THAT(out_l[3], WithinAbs(35.0f, 1.0e-6f));
}

TEST_CASE("ClipPlayer does not silently identity-render unbaked tempo-sync clips",
          "[engine][clip_player]") {
  std::array<float, 4> source{1.0f, 2.0f, 3.0f, 4.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipSchedule clip{1, {channels, 1, 4}, 0.0, 0, 0, 4, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kTempoSync;

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 4);
  player.set_clips({clip});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 0);

  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[1] == 0.0f);
  REQUIRE(out_l[2] == 0.0f);
  REQUIRE(out_l[3] == 0.0f);
}

TEST_CASE(
    "ClipPlayer clip_count reflects published clips without touching the audio-thread acquire",
    "[engine][clip_player]") {
  std::array<float, 2> source{1.0f, 1.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);

  // Count is correct immediately after publish, with no process()/acquire()
  // call having run on the audio thread yet (host polling before playback).
  REQUIRE(player.clip_count() == 0);
  player.set_clips({{1, {channels, 1, 2}, 0.0, 0, 0, 2, false, 1.0f, 0, 0},
                    {2, {channels, 1, 2}, 0.0, 4, 0, 2, false, 1.0f, 0, 0}});
  REQUIRE(player.clip_count() == 2);

  // Polling clip_count() repeatedly (control thread) must NOT consume the
  // published snapshot: the audio thread's acquire_clips() still adopts it.
  for (int i = 0; i < 100; ++i) {
    REQUIRE(player.clip_count() == 2);
  }
  player.acquire_clips();
  std::array<float, 8> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 8, 0);
  REQUIRE(out_l[0] == 1.0f);
  REQUIRE(out_l[4] == 1.0f);

  // A subsequent publish updates the count straight away.
  player.set_clips({});
  REQUIRE(player.clip_count() == 0);
}

TEST_CASE("ClipPlayer clip_count follows coalesced publishes when the publish ring is full",
          "[engine][clip_player]") {
  std::array<float, 1> source{1.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 8);

  using ClipVector = std::vector<sonare::engine::ClipSchedule>;
  constexpr size_t kCapacity = sonare::rt::RtPublisher<ClipVector>::kCapacity;
  for (size_t i = 1; i <= kCapacity; ++i) {
    ClipVector clips;
    clips.reserve(i);
    for (size_t id = 1; id <= i; ++id) {
      clips.push_back(
          {static_cast<uint32_t>(id), {channels, 1, 1}, 0.0, 0, 0, 1, false, 1.0f, 0, 0});
    }
    player.set_clips(std::move(clips));
    REQUIRE(player.clip_count() == i);
  }

  player.set_clips({});
  REQUIRE(player.clip_count() == 0);

  player.acquire_clips();
  std::array<float, 1> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 1, 0);
  REQUIRE(out_l[0] == 0.0f);
}

TEST_CASE("ClipPlayer precomputes clip start from PPQ when tempo map is bound",
          "[engine][clip_player]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  std::array<float, 2> source{1.0f, 1.0f};
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 24008);
  player.set_tempo_map(&tempo);
  player.set_clips({{1, {channels, 1, 2}, 1.0, 0, 0, 2, false, 1.0f, 0, 0}});

  std::array<float, 4> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 4, 24000);

  REQUIRE(out_l[0] == 1.0f);
  REQUIRE(out_l[1] == 1.0f);
  REQUIRE(out_l[2] == 0.0f);
}

namespace {

// Paged provider that models residency: a page outside `resident` misses on
// read AND answers page_resident() false, so the look-ahead pass can see it.
class ResidencyProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  ResidencyProvider(int64_t samples, int64_t page_frames, std::vector<int64_t> resident)
      : samples_(samples), page_frames_(page_frames), resident_(std::move(resident)) {}

  int num_channels() const noexcept override { return 1; }
  int64_t num_samples() const noexcept override { return samples_; }
  int64_t page_frames() const noexcept override { return page_frames_; }

  bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
    if (channel != 0 || !out || sample < 0 || sample >= samples_) return false;
    if (!page_resident(sample / page_frames_)) return false;
    *out = 1.0f;
    return true;
  }

  bool page_resident(int64_t page_index) const noexcept override {
    for (int64_t page : resident_) {
      if (page == page_index) return true;
    }
    return false;
  }

 private:
  int64_t samples_ = 0;
  int64_t page_frames_ = 1;
  std::vector<int64_t> resident_;
};

class CountingPageRequestSink final : public sonare::engine::ClipPageRequestSink {
 public:
  void on_clip_page_miss(const sonare::engine::ClipPageRequest& request) noexcept override {
    requests.push_back(request);
  }
  std::vector<sonare::engine::ClipPageRequest> requests;
};

sonare::engine::ClipSchedule paged_clip(
    uint32_t id, std::shared_ptr<const sonare::engine::ClipPagedAudioProvider> provider,
    int64_t length) {
  sonare::engine::ClipSchedule clip{id, {}, 0.0, 0, 0, length, false, 1.0f, 0, 0};
  clip.page_provider = std::move(provider);
  return clip;
}

}  // namespace

TEST_CASE("ClipPlayer requests upcoming pages before the audio thread reads them",
          "[engine][clip_player]") {
  constexpr int64_t kPage = 512;
  constexpr int64_t kSamples = kPage * 40;
  // Only the first page is resident, so the first block reads cleanly and every
  // page the look-ahead window covers is still missing.
  auto provider = std::make_shared<ResidencyProvider>(kSamples, kPage, std::vector<int64_t>{0});

  CountingPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 128);
  player.set_page_request_sink(&sink);
  player.set_page_prefetch_frames(kPage * 4);
  player.set_clips({paged_clip(1, provider, kSamples)});

  std::array<float, 128> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 128, 0);

  // The block itself never missed (page 0 is resident), yet the pages it is
  // about to read were reported. Without the look-ahead this block would emit
  // nothing and the next page boundary would render a block of silence.
  REQUIRE_FALSE(sink.requests.empty());
  for (const auto& request : sink.requests) {
    REQUIRE(request.clip_id == 1u);
    REQUIRE(request.sample / kPage >= 1);
    REQUIRE(request.sample / kPage <= 4);
  }
  // One request per page, not per read: the pages in the window are distinct.
  std::vector<int64_t> pages;
  for (const auto& request : sink.requests) pages.push_back(request.sample / kPage);
  std::sort(pages.begin(), pages.end());
  REQUIRE(std::adjacent_find(pages.begin(), pages.end()) == pages.end());
}

TEST_CASE("ClipPlayer look-ahead is silent when every page is resident", "[engine][clip_player]") {
  constexpr int64_t kPage = 512;
  constexpr int64_t kSamples = kPage * 8;
  std::vector<int64_t> all;
  for (int64_t page = 0; page < 8; ++page) all.push_back(page);
  auto provider = std::make_shared<ResidencyProvider>(kSamples, kPage, all);

  CountingPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 128);
  player.set_page_request_sink(&sink);
  player.set_page_prefetch_frames(kPage * 4);
  player.set_clips({paged_clip(2, provider, kSamples)});

  std::array<float, 128> out_l{};
  float* out[] = {out_l.data()};
  for (int block = 0; block < 8; ++block) {
    out_l.fill(0.0f);
    player.process_at(out, 1, 128, static_cast<int64_t>(block) * 128);
  }
  REQUIRE(sink.requests.empty());
}

TEST_CASE("ClipPlayer look-ahead can be disabled", "[engine][clip_player]") {
  constexpr int64_t kPage = 512;
  constexpr int64_t kSamples = kPage * 8;
  auto provider = std::make_shared<ResidencyProvider>(kSamples, kPage, std::vector<int64_t>{0});

  CountingPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 128);
  player.set_page_request_sink(&sink);
  player.set_page_prefetch_frames(0);
  player.set_clips({paged_clip(3, provider, kSamples)});

  std::array<float, 128> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 128, 0);
  // Page 0 is resident, so with the look-ahead off nothing is reported at all --
  // exactly the read-then-miss behaviour that costs a block of silence at the
  // next page boundary.
  REQUIRE(sink.requests.empty());
}

TEST_CASE("A provider that cannot answer residency never produces look-ahead requests",
          "[engine][clip_player]") {
  // TestPagedProvider does not override page_resident(), so the default
  // "assume resident" keeps it out of the look-ahead path entirely.
  auto provider = std::make_shared<TestPagedProvider>(std::vector<float>(64, 1.0f),
                                                      /*missing_sample=*/-1,
                                                      /*page_frames=*/8);
  CountingPageRequestSink sink;
  sonare::engine::ClipPlayer player;
  player.prepare(48000.0, 16);
  player.set_page_request_sink(&sink);
  player.set_page_prefetch_frames(64);
  player.set_clips({paged_clip(4, provider, 64)});

  std::array<float, 16> out_l{};
  float* out[] = {out_l.data()};
  player.process_at(out, 1, 16, 0);
  REQUIRE(sink.requests.empty());
}

namespace {

constexpr int kStretchSampleRate = 48000;

/// Single-frequency power estimate (Goertzel), used to check that a stretched
/// signal keeps the source's pitch instead of transposing with the rate.
double tone_power(const std::vector<float>& x, size_t from, size_t to, double hz) {
  const double w = sonare::constants::kTwoPiD * hz / kStretchSampleRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  for (size_t i = from; i < to && i < x.size(); ++i) {
    const double s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double real = s1 - s2 * std::cos(w);
  const double imag = s2 * std::sin(w);
  return real * real + imag * imag;
}

std::vector<float> tone(double hz, size_t n) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(0.5 * std::sin(sonare::constants::kTwoPiD * hz *
                                             static_cast<double>(i) / kStretchSampleRate));
  }
  return x;
}

/// A clip whose warp map plays `source_samples` of source over
/// `output_samples` of timeline, i.e. a constant rate of source/output.
sonare::engine::ClipSchedule stretched_clip(uint32_t id, const float* const* channels,
                                            int64_t source_samples, int64_t output_samples,
                                            int64_t start_sample = 0) {
  sonare::engine::ClipSchedule clip{
      id, {channels, 1, source_samples}, 0.0, start_sample, 0, output_samples, false, 1.0f, 0, 0};
  clip.warp_mode = sonare::engine::WarpMode::kTimeStretch;
  clip.warp_anchors = std::make_shared<const std::vector<sonare::engine::WarpAnchor>>(
      std::vector<sonare::engine::WarpAnchor>{
          {0.0, 0.0}, {static_cast<double>(output_samples), static_cast<double>(source_samples)}});
  return clip;
}

}  // namespace

TEST_CASE("ClipPlayer time-stretch warp keeps the source pitch while halving the rate",
          "[engine][clip_player]") {
  constexpr size_t kSourceSamples = 24000;
  constexpr int kOutputSamples = 48000;
  const std::vector<float> source = tone(440.0, kSourceSamples);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kStretchSampleRate, kOutputSamples);
  player.set_clips({stretched_clip(1, channels, kSourceSamples, kOutputSamples)});

  std::vector<float> out(kOutputSamples, 0.0f);
  float* out_ptrs[] = {out.data()};
  player.process_at(out_ptrs, 1, kOutputSamples, 0);

  // Skip the first frame, where only one window has contributed yet.
  const size_t from = 4096;
  const size_t to = static_cast<size_t>(kOutputSamples) - 4096;
  const double at_source_pitch = tone_power(out, from, to, 440.0);
  const double at_halved_pitch = tone_power(out, from, to, 220.0);
  REQUIRE(at_source_pitch > 0.0);
  // Resampling at half rate would put all the energy at 220 Hz instead.
  REQUIRE(at_source_pitch > 100.0 * at_halved_pitch);

  double energy = 0.0;
  for (size_t i = from; i < to; ++i) energy += static_cast<double>(out[i]) * out[i];
  REQUIRE(energy / static_cast<double>(to - from) > 0.01);
}

TEST_CASE("ClipPlayer time-stretch warp is block-size independent while playback is contiguous",
          "[engine][clip_player]") {
  constexpr size_t kSourceSamples = 6000;
  constexpr int kOutputSamples = 8192;
  const std::vector<float> source = tone(330.0, kSourceSamples);
  const float* channels[] = {source.data()};
  const sonare::engine::ClipSchedule clip =
      stretched_clip(2, channels, kSourceSamples, kOutputSamples);

  sonare::engine::ClipPlayer whole;
  whole.prepare(kStretchSampleRate, kOutputSamples);
  whole.set_clips({clip});
  std::vector<float> single(kOutputSamples, 0.0f);
  float* single_ptrs[] = {single.data()};
  whole.process_at(single_ptrs, 1, kOutputSamples, 0);

  sonare::engine::ClipPlayer split;
  split.prepare(kStretchSampleRate, 512);
  split.set_clips({clip});
  std::vector<float> chunked(kOutputSamples, 0.0f);
  for (int offset = 0; offset < kOutputSamples; offset += 512) {
    float* chunk_ptrs[] = {chunked.data() + offset};
    split.process_at(chunk_ptrs, 1, 512, offset);
  }

  for (int i = 0; i < kOutputSamples; ++i) {
    REQUIRE_THAT(chunked[static_cast<size_t>(i)],
                 WithinAbs(single[static_cast<size_t>(i)], 1.0e-5f));
  }
}

TEST_CASE("ClipPlayer time-stretch warp falls back to resampling past the voice budget",
          "[engine][clip_player]") {
  constexpr size_t kSourceSamples = 4000;
  constexpr int kOutputSamples = 4096;
  const std::vector<float> source = tone(220.0, kSourceSamples);
  const float* channels[] = {source.data()};

  // One more simultaneous stretched clip than there are voices.
  std::vector<sonare::engine::ClipSchedule> clips;
  for (uint32_t id = 1; id <= 9; ++id) {
    clips.push_back(stretched_clip(id, channels, kSourceSamples, kOutputSamples));
  }

  sonare::engine::ClipPlayer player;
  player.prepare(kStretchSampleRate, kOutputSamples);
  player.set_clips(clips);

  std::vector<float> out(kOutputSamples, 0.0f);
  float* out_ptrs[] = {out.data()};
  player.process_at(out_ptrs, 1, kOutputSamples, 0);

  REQUIRE(player.warp_stretch_overflow_count() > 0);
  // The overflowing clip must still be audible through the resampling path.
  double energy = 0.0;
  for (float value : out) energy += static_cast<double>(value) * value;
  REQUIRE(energy > 0.0);
}

TEST_CASE("ClipPlayer time-stretch warp under an identity map reproduces the source",
          "[engine][clip_player]") {
  constexpr size_t kSamples = 8192;
  const std::vector<float> source = tone(300.0, kSamples);
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kStretchSampleRate, static_cast<int>(kSamples));
  player.set_clips({stretched_clip(3, channels, kSamples, kSamples)});

  std::vector<float> out(kSamples, 0.0f);
  float* out_ptrs[] = {out.data()};
  player.process_at(out_ptrs, 1, static_cast<int>(kSamples), 0);

  // At unity rate the similarity search should land on the aligned segment, so
  // the overlap-add reconstructs the source rather than smearing it.
  const size_t from = 2048;
  const size_t to = kSamples - 2048;
  double error = 0.0;
  double reference = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double diff = static_cast<double>(out[i]) - source[i];
    error += diff * diff;
    reference += static_cast<double>(source[i]) * source[i];
  }
  REQUIRE(reference > 0.0);
  REQUIRE(std::sqrt(error / reference) < 0.05);
}

TEST_CASE("ClipPlayer time-stretch warp is audibly equivalent to the offline warp bake",
          "[engine][clip_player]") {
  // The offline path bakes a warped clip with an FFT phase vocoder
  // (bake_tempo_sync_warp_channel); the realtime path overlap-adds in the time
  // domain. They are different algorithms, so this compares what a listener
  // would compare: the pitch stays put and the amplitude envelope lands at the
  // same times. A tremolo carries that timing — a steady tone would not.
  constexpr size_t kSourceSamples = 24000;
  constexpr int kOutputSamples = 48000;
  // Unevenly spaced bursts, so the envelope carries a signature that only lines
  // up when both stretchers agree on WHERE in the timeline each burst lands.
  const std::array<std::pair<double, double>, 3> bursts{
      std::make_pair(0.06, 0.12), std::make_pair(0.24, 0.27), std::make_pair(0.36, 0.48)};
  std::vector<float> source(kSourceSamples);
  for (size_t i = 0; i < kSourceSamples; ++i) {
    const double t = static_cast<double>(i) / kStretchSampleRate;
    double amplitude = 0.0;
    for (const auto& burst : bursts) {
      if (t >= burst.first && t < burst.second) amplitude = 0.5;
    }
    source[i] = static_cast<float>(amplitude * std::sin(sonare::constants::kTwoPiD * 440.0 * t));
  }
  const float* channels[] = {source.data()};

  sonare::engine::ClipPlayer player;
  player.prepare(kStretchSampleRate, kOutputSamples);
  player.set_clips({stretched_clip(4, channels, kSourceSamples, kOutputSamples)});
  std::vector<float> realtime(kOutputSamples, 0.0f);
  float* out_ptrs[] = {realtime.data()};
  player.process_at(out_ptrs, 1, kOutputSamples, 0);

  sonare::engine::TempoSyncWarpBakeConfig bake_config;
  bake_config.sample_rate = kStretchSampleRate;
  const std::vector<sonare::engine::TempoSyncWarpSegment> segments{
      {0, kSourceSamples, static_cast<size_t>(kOutputSamples)}};
  const std::vector<float> baked = sonare::engine::bake_tempo_sync_warp_channel(
      source.data(), kSourceSamples, segments, bake_config);
  REQUIRE(baked.size() >= static_cast<size_t>(kOutputSamples));

  const size_t from = 4096;
  const size_t to = static_cast<size_t>(kOutputSamples) - 4096;
  REQUIRE(tone_power(realtime, from, to, 440.0) > 100.0 * tone_power(realtime, from, to, 220.0));

  // Short-time RMS envelopes, correlated. A timing drift between the two
  // stretchers would decorrelate the tremolo peaks.
  constexpr size_t kEnvelopeWindow = 512;
  std::vector<double> realtime_env;
  std::vector<double> baked_env;
  for (size_t start = from; start + kEnvelopeWindow <= to; start += kEnvelopeWindow) {
    double a = 0.0;
    double b = 0.0;
    for (size_t i = 0; i < kEnvelopeWindow; ++i) {
      a += static_cast<double>(realtime[start + i]) * realtime[start + i];
      b += static_cast<double>(baked[start + i]) * baked[start + i];
    }
    realtime_env.push_back(std::sqrt(a / kEnvelopeWindow));
    baked_env.push_back(std::sqrt(b / kEnvelopeWindow));
  }
  REQUIRE(realtime_env.size() > 16);
  const auto correlate = [](const std::vector<double>& a, const std::vector<double>& b) {
    const auto mean = [](const std::vector<double>& v) {
      double sum = 0.0;
      for (double value : v) sum += value;
      return sum / static_cast<double>(v.size());
    };
    const double mean_a = mean(a);
    const double mean_b = mean(b);
    double covariance = 0.0;
    double var_a = 0.0;
    double var_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
      const double da = a[i] - mean_a;
      const double db = b[i] - mean_b;
      covariance += da * db;
      var_a += da * da;
      var_b += db * db;
    }
    return var_a > 0.0 && var_b > 0.0 ? covariance / std::sqrt(var_a * var_b) : 0.0;
  };
  const double aligned = correlate(realtime_env, baked_env);
  // Control: the same envelopes with one reversed. Two stretchers that put the
  // bursts in different places would score no better than this, so requiring a
  // clear margin over it keeps the threshold from being an arbitrary number
  // that a smeared or drifting stretcher could still clear.
  std::vector<double> reversed_env(baked_env.rbegin(), baked_env.rend());
  const double control = correlate(realtime_env, reversed_env);
  REQUIRE(aligned > 0.8);
  REQUIRE(aligned > control + 0.15);
}
