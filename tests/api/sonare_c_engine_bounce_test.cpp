/// @file sonare_c_engine_bounce_test.cpp
/// @brief Engine C ABI offline render, bounce and freeze validation.

#include "sonare_c_engine_test_helpers.h"

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
