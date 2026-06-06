#include "engine/clip_player.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

class TestPagedProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  explicit TestPagedProvider(std::vector<float> samples, int64_t missing_sample = -1)
      : samples_(std::move(samples)), missing_sample_(missing_sample) {}

  int num_channels() const noexcept override { return 1; }
  int64_t num_samples() const noexcept override { return static_cast<int64_t>(samples_.size()); }

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

TEST_CASE("ClipPlayer reports paged provider misses across loop wraps", "[engine][clip_player]") {
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
  REQUIRE(sink.count >= 2);
  REQUIRE(sink.requests[0].clip_id == 88);
  REQUIRE(sink.requests[0].sample == 1);
  REQUIRE(sink.requests[1].clip_id == 88);
  REQUIRE(sink.requests[1].sample == 1);
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
