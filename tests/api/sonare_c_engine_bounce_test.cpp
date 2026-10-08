/// @file sonare_c_engine_bounce_test.cpp
/// @brief Engine C ABI offline render, bounce and freeze validation.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "sonare_c_engine_test_helpers.h"

namespace {

std::vector<float> run_silent_dither_bounce(int num_channels, int dither_type) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 8, 8) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 256;
  options.block_size = 128;
  options.num_channels = num_channels;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.dither = dither_type;
  options.dither_bits = 16;
  options.dither_seed = 1234;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.interleaved != nullptr);
  REQUIRE(result.frames == options.total_frames);
  REQUIRE(result.num_channels == num_channels);
  REQUIRE(result.sample_count == static_cast<size_t>(result.frames * result.num_channels));

  std::vector<float> interleaved(result.interleaved, result.interleaved + result.sample_count);
  sonare_free_bounce_result(&result);
  sonare_engine_destroy(engine);
  return interleaved;
}

}  // namespace

TEST_CASE("offline bounce dither keeps channel zero invariant across widths",
          "[c_api][engine][dither]") {
  const int dither_type = GENERATE(2, 3);  // TPDF and noise-shaped.
  const int num_channels = GENERATE(2, 6, 8);

  const auto mono = run_silent_dither_bounce(1, dither_type);
  const auto interleaved = run_silent_dither_bounce(num_channels, dither_type);
  REQUIRE(mono.size() == 256);
  REQUIRE(interleaved.size() % static_cast<size_t>(num_channels) == 0);
  REQUIRE(interleaved.size() / static_cast<size_t>(num_channels) == mono.size());

  size_t channel_zero_mismatches = 0;
  size_t nonzero_samples = 0;
  size_t channel_divergences = 0;
  for (size_t frame = 0; frame < mono.size(); ++frame) {
    const float channel_zero = interleaved[frame * static_cast<size_t>(num_channels)];
    if (channel_zero != mono[frame]) ++channel_zero_mismatches;
    for (int channel = 0; channel < num_channels; ++channel) {
      const float sample =
          interleaved[frame * static_cast<size_t>(num_channels) + static_cast<size_t>(channel)];
      if (sample != 0.0f) ++nonzero_samples;
      if (channel > 0 && sample != channel_zero) ++channel_divergences;
    }
  }
  CAPTURE(dither_type, num_channels, channel_zero_mismatches, nonzero_samples, channel_divergences);
  REQUIRE(channel_zero_mismatches == 0);
  REQUIRE(nonzero_samples > 0);
  REQUIRE(channel_divergences > 0);
}

TEST_CASE("sonare_engine_bounce_offline validates the channel count against a layout",
          "[c_api][engine][surround]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 128;
  options.block_size = 128;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;

  // 3/4/5/7 have no speaker layout and must be rejected.
  for (int bad : {3, 4, 5, 7}) {
    options.num_channels = bad;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_bounce_result(&result);
  }

  // 6 (5.1) is a supported surround width.
  options.num_channels = 6;
  SonareEngineBounceResult ok{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &ok) == SONARE_OK);
  REQUIRE(ok.num_channels == 6);
  sonare_free_bounce_result(&ok);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline rejects an out-of-range dither type", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 2, 2) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 128;
  options.block_size = 128;
  options.num_channels = 2;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;

  // Only 0..3 are documented. A value outside that range mapped to "no dither"
  // would return SONARE_OK with undithered audio, leaving the caller no way to
  // tell the request was dropped.
  for (int bad : {-1, 4, 7}) {
    options.dither = bad;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_bounce_result(&result);
  }

  for (int good : {0, 1, 2, 3}) {
    options.dither = good;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
    sonare_free_bounce_result(&result);
  }

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline refuses dither bits before it renders",
          "[c_api][engine][dither]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 8, 8) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 256;
  options.block_size = 128;
  options.num_channels = 2;
  options.dither = 2;
  options.dither_seed = 1234;

  for (int bad : {1, 33}) {
    options.dither_bits = bad;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    SonareTransportState state{};
    REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
    REQUIRE(state.sample_position == 0);
    REQUIRE(state.playing == 0);
  }

  // The retry with a valid width renders exactly what an untouched engine renders.
  options.dither_bits = 16;
  SonareEngineBounceResult retried{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &retried) == SONARE_OK);
  const std::vector<float> after_refusals(retried.interleaved,
                                          retried.interleaved + retried.sample_count);
  sonare_free_bounce_result(&retried);
  sonare_engine_destroy(engine);
  REQUIRE(after_refusals == run_silent_dither_bounce(2, 2));
}

TEST_CASE("an offline render's last meter record holds the true peak of its final samples",
          "[c_api][engine][meter]") {
  constexpr int kFrames = 1024;
  // A quarter-rate tone sampled 45 degrees off its crests: every sample reads
  // 0.9/sqrt(2) while the waveform between them reaches 0.9, at the very end.
  std::vector<float> clip(kFrames, 0.0f);
  const float sample = 0.9f * 0.70710678f;
  for (int i = 0; i < 8; ++i) {
    clip[static_cast<size_t>(kFrames - 8 + i)] = (i / 2) % 2 == 0 ? sample : -sample;
  }
  const float* clip_channels[] = {clip.data()};

  const auto render = [&](bool chunked) {
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 16, 64, 1) == SONARE_OK);
    SonareEngineClip source{};
    source.id = 1;
    source.channels = clip_channels;
    source.num_channels = 1;
    source.num_samples = kFrames;
    source.gain = 1.0f;
    REQUIRE(sonare_engine_set_clips(engine, &source, 1) == SONARE_OK);
    std::vector<float> out(kFrames, 0.0f);
    float* io[] = {out.data()};
    REQUIRE(sonare_engine_render_offline_ex(engine, io, 1, kFrames, 128, chunked ? 0 : 1) ==
            SONARE_OK);
    const auto reported = [&] {
      float max_true_peak = -1000.0f;
      std::array<SonareMeterTelemetryRecordV2, 64> records{};
      size_t count = 0;
      do {
        REQUIRE(sonare_engine_drain_meter_telemetry_v2(engine, records.data(), records.size(),
                                                       &count) == SONARE_OK);
        for (size_t i = 0; i < count; ++i) {
          if (records[i].target_id == 0) {
            max_true_peak = std::max(max_true_peak, records[i].max_true_peak_db);
          }
        }
      } while (count > 0);
      return max_true_peak;
    };
    const float before_finish = reported();
    if (chunked) REQUIRE(sonare_engine_finish_offline_render(engine) == SONARE_OK);
    const float after = std::max(before_finish, reported());
    float measured = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(out.data(), out.size(), 48000, 4, &measured) == SONARE_OK);
    sonare_engine_destroy(engine);
    return std::array<float, 3>{before_finish, after, measured};
  };

  const auto one_shot = render(false);
  CHECK(one_shot[1] == Catch::Approx(one_shot[2]).margin(0.05));
  CHECK(one_shot[2] > 20.0f * std::log10(sample) + 1.0f);
  // A chunk that does not end the render has not yet read its last samples whole.
  const auto chunk = render(true);
  CHECK(chunk[0] < chunk[2] - 0.5f);
  CHECK(chunk[1] == Catch::Approx(chunk[2]).margin(0.05));
}

TEST_CASE("engine-owned offline results reject shapes above the allocation budget",
          "[c_api][engine]") {
  const sonare::resource::EngineOfflineLimits tiny_limits{/*max_total_samples=*/100,
                                                          /*max_peak_bytes=*/3000};
  REQUIRE(sonare::resource::engine_offline_shape_fits(10, 2, 3, tiny_limits));
  REQUIRE_FALSE(sonare::resource::engine_offline_shape_fits(51, 2, 1, tiny_limits));
  REQUIRE(sonare::resource::engine_bounce_shape_fits(20, 2, 10, 20, false, tiny_limits));
  REQUIRE_FALSE(sonare::resource::engine_bounce_shape_fits(60, 2, 10, 20, false, tiny_limits));

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions bounce{};
  REQUIRE(sonare_engine_bounce_options_default(&bounce) == SONARE_OK);
  bounce.total_frames = std::numeric_limits<int64_t>::max();
  SonareEngineBounceResult bounce_result{};
  bounce_result.interleaved = reinterpret_cast<float*>(0x1);
  bounce_result.sample_count = 123;
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce, &bounce_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(bounce_result.interleaved == nullptr);
  REQUIRE(bounce_result.sample_count == 0u);

  SonareEngineFreezeOptions freeze{};
  freeze.total_frames = std::numeric_limits<int64_t>::max();
  freeze.block_size = 128;
  freeze.num_channels = 2;
  freeze.gain = 1.0f;
  SonareEngineFreezeResult freeze_result{};
  freeze_result.clip_id = 123;
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze, &freeze_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(freeze_result.clip_id == 0u);
  REQUIRE(freeze_result.frames == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("the shipped bounce budget refuses a stereo span past its documented length",
          "[c_api][engine]") {
  // Frame counts spelled out rather than derived from the constants, so moving
  // either the copy count or kMaxEngineOfflinePeakBytes turns these red instead
  // of moving the expectation along with the policy. They are the caller-facing
  // lengths documented on sonare_engine_bounce_offline.
  constexpr int64_t kStereoMaxFrames = 44'739'242;        // 15 min 32 s at 48 kHz
  constexpr int64_t kStereoMaxDitherFrames = 33'554'432;  // 11 min 39 s at 48 kHz
  constexpr int64_t kFourMinutes = 4 * 60 * 48000;

  REQUIRE(sonare::resource::engine_bounce_shape_fits(kFourMinutes, 2, 48000, 48000, false));
  REQUIRE(sonare::resource::engine_bounce_shape_fits(kFourMinutes, 2, 48000, 48000, true));

  REQUIRE(sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames, 2, 48000, 48000, false));
  REQUIRE_FALSE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames + 1, 2, 48000, 48000, false));

  // Dither copies the interleaved buffer into a second one while the source is
  // still live, so its ceiling sits one full copy below the undithered one.
  REQUIRE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxDitherFrames, 2, 48000, 48000, true));
  REQUIRE_FALSE(sonare::resource::engine_bounce_shape_fits(kStereoMaxDitherFrames + 1, 2, 48000,
                                                           48000, true));
  // The same span the undithered branch admits is refused once dither is asked
  // for; that gap is the whole reason the count depends on the dither request.
  REQUIRE_FALSE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames, 2, 48000, 48000, true));

  // Resampling holds the source planar set beside the projected one, which the
  // two checks already bound, so it does not lower the ceiling: a ten-minute
  // 44.1 -> 48 kHz bounce fits on both sides of the conversion.
  constexpr int64_t kTenMinutesAt441 = 10 * 60 * 44100;
  REQUIRE(sonare::resource::engine_bounce_shape_fits(kTenMinutesAt441, 2, 44100, 48000, false));

  // Channel count is the only divisor of the frame ceiling: bytes per frame
  // scale with it, while the sample rate never enters the product.
  REQUIRE_FALSE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames, 6, 48000, 48000, false));
  // Same counts at 96 kHz, which is the documented claim that the cap is a
  // frame count and only its reading as a duration moves with the rate.
  REQUIRE(sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames, 2, 96000, 96000, false));
  REQUIRE_FALSE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxFrames + 1, 2, 96000, 96000, false));
  REQUIRE(
      sonare::resource::engine_bounce_shape_fits(kStereoMaxDitherFrames, 2, 96000, 96000, true));
  REQUIRE_FALSE(sonare::resource::engine_bounce_shape_fits(kStereoMaxDitherFrames + 1, 2, 96000,
                                                           96000, true));
}

TEST_CASE("the C bounce entry point charges the budget for the dither request", "[c_api][engine]") {
  // A span between the two ceilings: the undithered count admits it and the
  // dithered one does not, so the two branches must answer differently. The
  // engine is deliberately left unprepared, which makes the admitted branch stop
  // at the never-prepared check instead of rendering 40M frames -- a wiring
  // regression then reads as INVALID_STATE in under a millisecond.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 40'000'000;

  SonareEngineBounceResult result{};
  options.dither = 1;
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.interleaved == nullptr);

  options.dither = 0;
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_ERROR_INVALID_STATE);

  sonare_engine_destroy(engine);
}

TEST_CASE("offline render/bounce/freeze reject a never-prepared engine", "[c_api][engine]") {
  // A freshly created engine has never been prepared: max_block_size_ is 0, so
  // render_offline renders nothing and its only diagnostic path (a kNotPrepared
  // telemetry record) targets an unreserved SPSC ring. The offline entry points
  // must report this synchronously as INVALID_STATE instead of handing back a
  // silent buffer (and, in debug builds, without tripping the push-before-reserve
  // assert inside the telemetry ring).
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);

  constexpr int64_t kFrames = 128;
  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  std::array<float*, 2> channels{left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 2, kFrames, 128) ==
          SONARE_ERROR_INVALID_STATE);

  SonareEngineBounceOptions bounce{};
  REQUIRE(sonare_engine_bounce_options_default(&bounce) == SONARE_OK);
  bounce.total_frames = kFrames;
  SonareEngineBounceResult bounce_result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce, &bounce_result) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(bounce_result.interleaved == nullptr);
  REQUIRE(bounce_result.sample_count == 0u);
  sonare_free_bounce_result(&bounce_result);

  SonareEngineFreezeOptions freeze{};
  freeze.total_frames = kFrames;
  freeze.block_size = 128;
  freeze.num_channels = 2;
  freeze.gain = 1.0f;
  SonareEngineFreezeResult freeze_result{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze, &freeze_result) ==
          SONARE_ERROR_INVALID_STATE);

  // After prepare() the telemetry ring is reserved and the same render succeeds.
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 2, kFrames, 128) == SONARE_OK);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_prepare bounds the command and telemetry capacities", "[c_api][engine]") {
  // telemetry_capacity is not a queue depth the caller pays for one-for-one:
  // prepare() reserves that many meter records per metered lane, so a host
  // passing a merely generous number drives an allocation two orders of
  // magnitude larger than it asked for. Both capacities are refused above the
  // documented maximum instead, and a refused prepare leaves the engine exactly
  // as unprepared as it was.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);

  REQUIRE(
      sonare_engine_prepare(engine, 48000.0, 128, 16, SONARE_ENGINE_MAX_TELEMETRY_CAPACITY + 1) ==
      SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, SONARE_ENGINE_MAX_COMMAND_CAPACITY + 1, 16) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 1000000) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 16, 1000000, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  constexpr int64_t kFrames = 128;
  std::array<float, kFrames> left{};
  std::array<float, kFrames> right{};
  std::array<float*, 2> channels{left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 2, kFrames, 128) ==
          SONARE_ERROR_INVALID_STATE);

  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 2, kFrames, 128) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("offline render/bounce/freeze reject more channels than the prepared bound",
          "[c_api][engine]") {
  // sonare_engine_prepare_with_channels bounds capture/instrument/PDC/monitor
  // scratch to max_channels; rendering/bouncing/freezing more planes than that
  // must not silently succeed with zeros written past the reserved scratch.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 16, 16, /*max_channels=*/2) ==
          SONARE_OK);

  constexpr int64_t kFrames = 128;
  std::array<float, kFrames> ch0{};
  std::array<float, kFrames> ch1{};
  std::array<float, kFrames> ch2{};
  std::array<float, kFrames> ch3{};
  std::array<float, kFrames> ch4{};
  std::array<float, kFrames> ch5{};
  std::array<float*, 6> channels{ch0.data(), ch1.data(), ch2.data(),
                                 ch3.data(), ch4.data(), ch5.data()};

  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 6, kFrames, 128) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Exactly at the prepared bound still succeeds.
  REQUIRE(sonare_engine_render_offline(engine, channels.data(), 2, kFrames, 128) == SONARE_OK);

  SonareEngineBounceOptions bounce{};
  REQUIRE(sonare_engine_bounce_options_default(&bounce) == SONARE_OK);
  bounce.total_frames = kFrames;
  bounce.num_channels = 6;  // valid 5.1 speaker layout, but above the prepared bound
  SonareEngineBounceResult bounce_result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce, &bounce_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(bounce_result.interleaved == nullptr);
  sonare_free_bounce_result(&bounce_result);

  SonareEngineFreezeOptions freeze{};
  freeze.total_frames = kFrames;
  freeze.block_size = 128;
  freeze.num_channels = 6;
  freeze.gain = 1.0f;
  SonareEngineFreezeResult freeze_result{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze, &freeze_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(freeze_result.clip_id == 0u);

  sonare_engine_destroy(engine);
}

TEST_CASE("offline render/bounce/freeze/prime reject a block size above the prepared block",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 8, 8) == SONARE_OK);

  constexpr int64_t kFrames = 256;
  std::vector<float> left(kFrames), right(kFrames);
  float* channels[] = {left.data(), right.data()};

  REQUIRE(sonare_engine_render_offline(engine, channels, 2, kFrames, 129) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_render_offline_ex(engine, channels, 2, kFrames, 4096, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prime_offline_parameters(engine, 2, 129) == SONARE_ERROR_INVALID_PARAMETER);
  // The prepared block itself, and anything smaller, still renders.
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, kFrames, 128) == SONARE_OK);
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, kFrames, 64) == SONARE_OK);

  SonareEngineBounceOptions bounce{};
  REQUIRE(sonare_engine_bounce_options_default(&bounce) == SONARE_OK);
  bounce.total_frames = kFrames;
  bounce.block_size = 129;
  SonareEngineBounceResult bounce_result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce, &bounce_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(bounce_result.interleaved == nullptr);

  SonareEngineFreezeOptions freeze{};
  freeze.total_frames = kFrames;
  freeze.block_size = 129;
  freeze.num_channels = 2;
  freeze.gain = 1.0f;
  SonareEngineFreezeResult freeze_result{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze, &freeze_result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(freeze_result.clip_id == 0u);

  sonare_engine_destroy(engine);
}

TEST_CASE(
    "engine bounce rejects a source rate different from the prepared rate without moving transport",
    "[c_api][engine][sample_rate]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 44100.0, 128, 16, 16) == SONARE_OK);

  SonareTransportState before{};
  REQUIRE(sonare_engine_get_transport_state(engine, &before) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 256;
  options.block_size = 128;
  options.num_channels = 2;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.interleaved == nullptr);
  REQUIRE(result.sample_count == 0u);
  REQUIRE(result.frames == 0);
  REQUIRE(result.num_channels == 0);
  REQUIRE(result.sample_rate == 0);

  SonareTransportState after{};
  REQUIRE(sonare_engine_get_transport_state(engine, &after) == SONARE_OK);
  REQUIRE(after.playing == before.playing);
  REQUIRE(after.looping == before.looping);
  REQUIRE(after.render_frame == before.render_frame);
  REQUIRE(after.sample_position == before.sample_position);
  REQUIRE(after.ppq_position == before.ppq_position);
  REQUIRE(after.bpm == before.bpm);
  REQUIRE(after.loop_start_ppq == before.loop_start_ppq);
  REQUIRE(after.loop_end_ppq == before.loop_end_ppq);
  REQUIRE(after.sample_rate == before.sample_rate);
  REQUIRE(after.bar_start_ppq == before.bar_start_ppq);
  REQUIRE(after.bar_count == before.bar_count);
  REQUIRE(after.time_signature.numerator == before.time_signature.numerator);
  REQUIRE(after.time_signature.denominator == before.time_signature.denominator);
  REQUIRE(after.time_signature.confidence == before.time_signature.confidence);
  REQUIRE(after.beat == before.beat);
  REQUIRE(after.beat_fraction == before.beat_fraction);

  sonare_free_bounce_result(&result);
  sonare_engine_destroy(engine);
}

TEST_CASE("engine bounce resamples from the matching prepared source rate",
          "[c_api][engine][sample_rate]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 44100.0, 128, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 44100;
  options.block_size = 128;
  options.num_channels = 2;
  options.source_sample_rate = 44100;
  options.target_sample_rate = 48000;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.interleaved != nullptr);
  REQUIRE(result.frames == 48000);
  REQUIRE(result.sample_count == static_cast<size_t>(48000 * 2));
  REQUIRE(result.num_channels == 2);
  REQUIRE(result.sample_rate == 48000);

  sonare_free_bounce_result(&result);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline defaults both rates to the prepared rate",
          "[c_api][engine][bounce_rate]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 44100.0, 128, 8, 8) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  REQUIRE(options.source_sample_rate == 0);
  REQUIRE(options.target_sample_rate == 0);
  options.total_frames = 441;
  options.block_size = 128;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.sample_rate == 44100);
  REQUIRE(result.frames == 441);
  sonare_free_bounce_result(&result);

  // An explicit source naming the prepared rate behaves the same, and a target left at 0
  // keeps that rate instead of resampling.
  options.source_sample_rate = 44100;
  SonareEngineBounceResult explicit_source{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &explicit_source) == SONARE_OK);
  REQUIRE(explicit_source.sample_rate == 44100);
  REQUIRE(explicit_source.frames == 441);
  sonare_free_bounce_result(&explicit_source);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline refuses a source rate other than the prepared rate",
          "[c_api][engine][bounce_rate]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 44100.0, 128, 8, 8) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 441;
  options.block_size = 128;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.interleaved == nullptr);

  options.source_sample_rate = -1;
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline resamples when the target differs from the source",
          "[c_api][engine][bounce_rate]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 44100.0, 128, 8, 8) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 44100;
  options.block_size = 128;
  options.target_sample_rate = 48000;

  // source left at 0 resolves to the prepared 44.1 kHz and the output is resampled.
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.sample_rate == 48000);
  REQUIRE(result.frames == 48000);
  sonare_free_bounce_result(&result);

  sonare_engine_destroy(engine);
}
