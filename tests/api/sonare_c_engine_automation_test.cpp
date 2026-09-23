/// @file sonare_c_engine_automation_test.cpp
/// @brief Engine C ABI parameter, automation and telemetry validation.

#include "sonare_c_engine_test_helpers.h"

TEST_CASE("engine degradation counters are reachable through the C ABI",
          "[c_api][engine][clip_pages]") {
  // Both counters record a silent degradation: page requests the bounded queue
  // could not retain, and time-stretched clips that fell back to resampling
  // because no stretcher voice was free. Neither had a C-ABI entry point, so no
  // host over the C ABI could detect either one.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  uint32_t pages = 0xDEADBEEFu;
  uint32_t stretch = 0xDEADBEEFu;
  REQUIRE(sonare_engine_clip_page_request_overflow_count(engine, &pages) == SONARE_OK);
  REQUIRE(sonare_engine_warp_stretch_overflow_count(engine, &stretch) == SONARE_OK);
  // prepare() resets both, so a freshly prepared engine reads zero on each -
  // which is also what makes a later non-zero reading meaningful.
  REQUIRE(pages == 0);
  REQUIRE(stretch == 0);

  // Both reject a null engine and a null out pointer, like every sibling getter.
  REQUIRE(sonare_engine_clip_page_request_overflow_count(nullptr, &pages) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_clip_page_request_overflow_count(engine, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_warp_stretch_overflow_count(nullptr, &stretch) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_warp_stretch_overflow_count(engine, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine validates realtime queue error classes", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 1, 1) == SONARE_OK);

  const uint8_t sysex[sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes + 1]{};
  REQUIRE(sonare_engine_push_midi_sysex(engine, 0, sysex, sizeof(sysex), -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, std::numeric_limits<double>::quiet_NaN(), 128, 1, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 7999.0, 128, 1, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare(engine, 384001.0, 128, 1, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 1, 1, 2) == SONARE_OK);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 1, 1, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Driven from the engine constant, so raising it moves the boundary here too.
  constexpr int max_channels = static_cast<int>(sonare::engine::RealtimeEngine::kMaxAudioChannels);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 1, 1, max_channels) ==
          SONARE_OK);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, 128, 1, 1, max_channels + 1) ==
          SONARE_ERROR_INVALID_PARAMETER);

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":0,\"band0.enabled\":1}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(
              engine, 10, 0, "band0.gainDb", std::numeric_limits<float>::quiet_NaN()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "unknown", 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "band0.gainDb", 1.0f) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "band0.gainDb", 2.0f) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "band0.gainDb", 3.0f) ==
          SONARE_ERROR_OUT_OF_MEMORY);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine reports prepared channel overflow through telemetry", "[c_api][engine]") {
  constexpr int kBlock = 64;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare_with_channels(engine, 48000.0, kBlock, 16, 16, 2) == SONARE_OK);

  std::array<float, kBlock * 2> left{};
  std::array<float, kBlock * 2> right{};
  std::array<float, kBlock * 2> extra{};
  float* too_many_channels[] = {left.data(), right.data(), extra.data()};
  left.fill(1.0f);
  right.fill(-1.0f);
  extra.fill(1.0f);

  // A block-size violation wins when both preconditions fail. C process still
  // returns SONARE_OK because the diagnostic is delivered asynchronously.
  REQUIRE(sonare_engine_process(engine, too_many_channels, 3, kBlock * 2) == SONARE_OK);
  for (float sample : left) REQUIRE(sample == 0.0f);
  for (float sample : right) REQUIRE(sample == 0.0f);
  for (float sample : extra) REQUIRE(sample == 0.0f);

  std::array<SonareEngineTelemetry, 4> telemetry{};
  size_t written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  REQUIRE(written == 1);
  REQUIRE(telemetry[0].type == 1);
  REQUIRE(telemetry[0].error == SONARE_ENGINE_TELEMETRY_ERROR_MAX_BLOCK_EXCEEDED);
  REQUIRE(telemetry[0].value == static_cast<uint32_t>(kBlock * 2));

  left.fill(1.0f);
  right.fill(-1.0f);
  extra.fill(1.0f);
  REQUIRE(sonare_engine_process(engine, too_many_channels, 3, kBlock) == SONARE_OK);
  for (int i = 0; i < kBlock; ++i) {
    REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(extra[static_cast<size_t>(i)] == 0.0f);
  }

  written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  REQUIRE(written == 1);
  REQUIRE(telemetry[0].type == 1);
  REQUIRE(telemetry[0].error == SONARE_ENGINE_TELEMETRY_ERROR_MAX_CHANNELS_EXCEEDED);
  REQUIRE(telemetry[0].value == 3);

  left.fill(0.25f);
  right.fill(-0.25f);
  float* prepared_channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, prepared_channels, 2, kBlock) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.25f));
  REQUIRE(right[0] == Catch::Approx(-0.25f));

  written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  REQUIRE(written == 1);
  REQUIRE(telemetry[0].type == 0);
  REQUIRE(telemetry[0].error == SONARE_ENGINE_TELEMETRY_ERROR_NONE);
  REQUIRE(telemetry[0].value == static_cast<uint32_t>(kBlock));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_flush_control_commands drains the command ring without process()",
          "[c_api][engine]") {
  // Mirrors the direct-engine test "RealtimeEngine control flush prevents an
  // offline mirror command ring from filling" (tests/engine/realtime_engine_test.cpp):
  // a control-only host that never calls sonare_engine_process must still be
  // able to drain queued commands through this C-ABI entry, or its bounded
  // command ring fills and push_command starts failing.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, /*command_capacity=*/4,
                                /*telemetry_capacity=*/4) == SONARE_OK);

  REQUIRE(sonare_engine_flush_control_commands(nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  for (int64_t sample = 0; sample < 1024; ++sample) {
    REQUIRE(sonare_engine_seek_sample(engine, sample, -1) == SONARE_OK);
    REQUIRE(sonare_engine_flush_control_commands(engine) == SONARE_OK);
  }

  SonareTransportState state{};
  REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
  REQUIRE(state.sample_position == 1023);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine rejects registered non realtime-safe automation targets",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareParameterInfo parameter{};
  parameter.id = 77;
  std::strncpy(parameter.name, "mode", sizeof(parameter.name) - 1);
  parameter.min_value = 0.0f;
  parameter.max_value = 3.0f;
  parameter.default_value = 0.0f;
  parameter.rt_safe = 0;
  parameter.default_curve = 0;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);

  const SonareAutomationPoint points[] = {{0.0, 1.0f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, 77, points, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  size_t lane_count = 999;
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 0);

  REQUIRE(sonare_engine_set_parameter(engine, 77, 1.0f, -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_parameter_smoothed(engine, 77, 1.0f, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_clear_parameters(engine) == SONARE_OK);
  REQUIRE(sonare_engine_set_automation_lane(engine, 77, points, 1) == SONARE_OK);
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 1);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine tempo and time-signature segments validate their input",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // Valid tempo ramp: constant 120 then ramp to 140.
  const SonareProjectTempoSegment tempo[] = {{0.0, 120.0, 0.0, 0.0}, {1920.0, 120.0, 0.0, 140.0}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, tempo, 2) == SONARE_OK);
  // Empty clears the map.
  REQUIRE(sonare_engine_set_tempo_segments(engine, nullptr, 0) == SONARE_OK);
  // Non-finite start_ppq, non-positive bpm, and negative end_bpm are rejected.
  const SonareProjectTempoSegment nan_start[] = {
      {std::numeric_limits<double>::quiet_NaN(), 120.0, 0.0, 0.0}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, nan_start, 1) == SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTempoSegment zero_bpm[] = {{0.0, 0.0, 0.0, 0.0}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, zero_bpm, 1) == SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTempoSegment huge_bpm[] = {{0.0, 100000.1, 0.0, 0.0}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, huge_bpm, 1) == SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTempoSegment huge_end_bpm[] = {{0.0, 120.0, 0.0, 100000.1}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, huge_end_bpm, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_tempo(engine, 100000.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 100000.1) == SONARE_ERROR_INVALID_PARAMETER);

  // Valid time-signature map, then invalid numerator.
  const SonareProjectTimeSignatureSegment sig[] = {{0.0, 4, 4}, {1920.0, 3, 4}};
  REQUIRE(sonare_engine_set_time_signature_segments(engine, sig, 2) == SONARE_OK);
  const SonareProjectTimeSignatureSegment bad_sig[] = {{0.0, 0, 4}};
  REQUIRE(sonare_engine_set_time_signature_segments(engine, bad_sig, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("tempo and time-signature segment setters keep exceptions inside the C ABI",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // An above-bound count is refused by the non-throwing guard, so the staging
  // vector is never sized from it.
  const SonareProjectTempoSegment one_tempo[] = {{0.0, 120.0, 0.0, 0.0}};
  REQUIRE(sonare_engine_set_tempo_segments(engine, one_tempo, std::numeric_limits<size_t>::max()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonareProjectTimeSignatureSegment one_sig[] = {{0.0, 4, 4}};
  REQUIRE(sonare_engine_set_time_signature_segments(engine, one_sig,
                                                    std::numeric_limits<size_t>::max()) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A count that really reaches reserve()/push_back(): the staging allocation
  // and the per-segment validation both run inside the try block, so a rejected
  // segment in the middle of a large list still leaves through a SonareError
  // return rather than an unwind.
  const size_t large_count = 100000;
  std::vector<SonareProjectTempoSegment> tempo(large_count);
  for (size_t i = 0; i < large_count; ++i) {
    tempo[i] = {static_cast<double>(i), 120.0, 0.0, 0.0};
  }
  REQUIRE(sonare_engine_set_tempo_segments(engine, tempo.data(), tempo.size()) == SONARE_OK);
  tempo[large_count / 2].bpm = -1.0;
  REQUIRE(sonare_engine_set_tempo_segments(engine, tempo.data(), tempo.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);

  std::vector<SonareProjectTimeSignatureSegment> sig(large_count);
  for (size_t i = 0; i < large_count; ++i) {
    sig[i] = {static_cast<double>(i), 4, 4};
  }
  REQUIRE(sonare_engine_set_time_signature_segments(engine, sig.data(), sig.size()) == SONARE_OK);
  sig[large_count / 2].numerator = 0;
  REQUIRE(sonare_engine_set_time_signature_segments(engine, sig.data(), sig.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine rejects out-of-range automation curve ordinals", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // An out-of-range default curve is rejected, not clamped.
  SonareParameterInfo parameter{};
  parameter.id = 88;
  parameter.min_value = 0.0f;
  parameter.max_value = 1.0f;
  parameter.rt_safe = 1;
  parameter.default_curve = 99;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_ERROR_INVALID_PARAMETER);
  parameter.default_curve = 3;  // In range now.
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);

  // An out-of-range breakpoint curve is rejected, matching the project path.
  const SonareAutomationPoint bad_curve[] = {{0.0, 0.5f, 99}};
  REQUIRE(sonare_engine_set_automation_lane(engine, 88, bad_curve, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonareAutomationPoint ok_curve[] = {{0.0, 0.5f, 2}};
  REQUIRE(sonare_engine_set_automation_lane(engine, 88, ok_curve, 1) == SONARE_OK);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine rejects engine-reserved automation parameter ids", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareParameterInfo parameter{};
  parameter.id = 0x4D580001u;
  std::strncpy(parameter.name, "reserved", sizeof(parameter.name) - 1);
  parameter.min_value = -60.0f;
  parameter.max_value = 6.0f;
  parameter.default_value = 0.0f;
  parameter.rt_safe = 1;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, parameter.id, -60.0f, 6.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine rejects the reserved invalid parameter id 0", "[c_api][engine]") {
  // The engine reserves 0 as "invalid / none": bind_target and the graph
  // parameter binding both refuse it, so a registration under it would be listed
  // and permanently inert. The refusal lives in ParameterRegistry::add, which
  // every surface's add path goes through, so this matches the WASM binding
  // instead of being the one surface that accepts it.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareParameterInfo parameter{};
  parameter.id = 0;
  std::strncpy(parameter.name, "gain", sizeof(parameter.name) - 1);
  parameter.min_value = 0.0f;
  parameter.max_value = 1.0f;
  parameter.rt_safe = 1;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_ERROR_INVALID_PARAMETER);

  size_t count = 1;
  REQUIRE(sonare_engine_parameter_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // The same descriptor under a usable id is still accepted.
  parameter.id = 7;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);
  REQUIRE(sonare_engine_parameter_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1u);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_add_parameter rejects a non-finite declared range", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SonareParameterInfo ordinary{};
  ordinary.id = 21;
  std::strncpy(ordinary.name, "gain", sizeof(ordinary.name) - 1);
  std::strncpy(ordinary.unit, "dB", sizeof(ordinary.unit) - 1);
  ordinary.min_value = -60.0f;
  ordinary.max_value = 6.0f;
  ordinary.default_value = 0.0f;
  ordinary.rt_safe = 1;
  ordinary.default_curve = 0;

  SECTION("each of min_value, max_value and default_value is refused on its own") {
    // Driven one field at a time: a descriptor carrying all three cannot say
    // which one the guard caught, and a NaN makes the ordering test false.
    const std::array<const char*, 3> names{"min_value", "max_value", "default_value"};
    const std::array<float SonareParameterInfo::*, 3> fields{&SonareParameterInfo::min_value,
                                                             &SonareParameterInfo::max_value,
                                                             &SonareParameterInfo::default_value};
    for (float bad : {nan, inf, -inf}) {
      CAPTURE(bad);
      for (size_t i = 0; i < fields.size(); ++i) {
        CAPTURE(names[i]);
        SonareParameterInfo parameter = ordinary;
        parameter.*(fields[i]) = bad;
        CHECK(sonare_engine_add_parameter(engine, &parameter) == SONARE_ERROR_INVALID_PARAMETER);
      }
    }
    size_t count = 1;
    REQUIRE(sonare_engine_parameter_count(engine, &count) == SONARE_OK);
    CHECK(count == 0u);
  }

  SECTION("an inverted finite range stays refused") {
    SonareParameterInfo inverted = ordinary;
    inverted.min_value = 1.0f;
    inverted.max_value = 0.0f;
    CHECK(sonare_engine_add_parameter(engine, &inverted) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("a degenerate but equal finite range still registers") {
    SonareParameterInfo degenerate = ordinary;
    degenerate.min_value = 0.5f;
    degenerate.max_value = 0.5f;
    degenerate.default_value = 0.5f;
    CHECK(sonare_engine_add_parameter(engine, &degenerate) == SONARE_OK);
  }

  SECTION("a finite default outside the declared range still registers") {
    // min_value/max_value are descriptive metadata the engine never clamps to,
    // so a default outside them is a caller's choice rather than an error.
    SonareParameterInfo outside = ordinary;
    outside.default_value = 12.0f;
    CHECK(sonare_engine_add_parameter(engine, &outside) == SONARE_OK);
  }

  SECTION("an ordinary parameter registers") {
    // Without this the refusals above are satisfied by an entry point that
    // rejects every descriptor.
    REQUIRE(sonare_engine_add_parameter(engine, &ordinary) == SONARE_OK);
    size_t count = 0;
    REQUIRE(sonare_engine_parameter_count(engine, &count) == SONARE_OK);
    CHECK(count == 1u);
  }

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine restores a lane fader's manual value once its automation lane empties",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  // 18 blocks are processed below (8 to let the fader settle, 1 to measure,
  // twice); size the clip generously past that so it never runs out.
  constexpr int kFrames = kBlock * 24;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);

  std::array<float, kFrames> clip_l{};
  clip_l.fill(1.0f);
  const float* clip_channels[] = {clip_l.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = clip_channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  const uint32_t fader_target = engine_lane_param_target(0, 1);  // TrackMixerRuntime::kFaderDb.
  // Manual value first: -24 dB.
  REQUIRE(sonare_engine_set_parameter(engine, fader_target, -24.0f, -1) == SONARE_OK);

  // An automation lane then drives the same fader to 0 dB (Hold, curve 2).
  SonareAutomationPoint points[] = {{0.0, 0.0f, 2}};
  REQUIRE(sonare_engine_set_automation_lane(engine, fader_target, points, 1) == SONARE_OK);

  std::array<float, kBlock> buf{};
  float* io[] = {buf.data()};
  for (int i = 0; i < 8; ++i) {
    buf.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);
  buf.fill(0.0f);
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(rms(buf) > 0.9);  // The lane drives the fader to ~0 dB (full amplitude).

  // Clear the lane: the fader must revert to the manual -24 dB value, not the
  // 0 dB the lane last drove it to.
  REQUIRE(sonare_engine_set_automation_lane(engine, fader_target, nullptr, 0) == SONARE_OK);
  for (int i = 0; i < 8; ++i) {
    buf.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);
  buf.fill(0.0f);
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  // -24 dB is roughly a 16x amplitude reduction (10^(-24/20) ~= 0.063).
  REQUIRE(rms(buf) < 0.15);
  REQUIRE(rms(buf) > 0.02);

  sonare_engine_destroy(engine);
}
#endif  // defined(SONARE_WITH_MIXING)
