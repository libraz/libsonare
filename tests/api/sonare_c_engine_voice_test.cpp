/// @file sonare_c_engine_voice_test.cpp
/// @brief Engine and voice C API tests.

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

#include "sonare_c_test_helpers.h"

namespace {

#if defined(SONARE_WITH_MIXING)
constexpr uint32_t engine_lane_param_target(uint32_t lane_index, uint32_t param_kind) {
  return 0x4D580000u | (lane_index << 8u) | param_kind;
}

constexpr uint32_t engine_bus_param_target(uint32_t bus_index, uint32_t param_kind) {
  return 0x4D580000u | ((0xFEu - bus_index) << 8u) | param_kind;
}

constexpr uint32_t engine_master_param_target(uint32_t param_kind) {
  return 0x4D580000u | (0xFFu << 8u) | param_kind;
}
#endif

float peak_abs(const std::vector<float>& data) {
  float peak = 0.0f;
  for (float value : data) peak = std::max(peak, std::abs(value));
  return peak;
}

double rms(const std::array<float, 256>& data) {
  double sum = 0.0;
  for (float value : data) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum / static_cast<double>(data.size()));
}

uint32_t midi1_word(uint8_t status, uint8_t channel, uint8_t data0, uint8_t data1) {
  return (0x2u << 28u) | (static_cast<uint32_t>(status & 0x0f) << 20u) |
         (static_cast<uint32_t>(channel & 0x0f) << 16u) | (static_cast<uint32_t>(data0) << 8u) |
         static_cast<uint32_t>(data1);
}

}  // namespace

TEST_CASE("sonare_error_message", "[c_api]") {
  SECTION("returns messages for all error codes") {
    REQUIRE(std::strcmp(sonare_error_message(SONARE_OK), "OK") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_FILE_NOT_FOUND), "File not found") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_FORMAT), "Invalid format") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_DECODE_FAILED), "Decode failed") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_INVALID_PARAMETER),
                        "Invalid parameter") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_OUT_OF_MEMORY), "Out of memory") == 0);
    REQUIRE(std::strcmp(sonare_error_message(SONARE_ERROR_UNKNOWN), "Unknown error") == 0);
  }
}

TEST_CASE("sonare_version", "[c_api]") {
  SECTION("returns version string") {
    const char* ver = sonare_version();
    REQUIRE(ver != nullptr);
    REQUIRE(std::strlen(ver) > 0);
  }

  SECTION("returns engine ABI version") { REQUIRE(sonare_engine_abi_version() > 0); }
}

TEST_CASE("sonare_engine MIDI scalar commands respect arrangement feature flag", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_OK);
#else
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 100, -1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_panic(engine, -1) == SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 16, 0, 74, 100, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

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

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine bounce scatters a strip's surround pan into a 5.1 master end-to-end",
          "[c_api][engine][surround]") {
  // Full wire path in one go: scene JSON surroundPan -> Strip.surround_pan ->
  // engine ChannelStrip -> apply_lane_to_mix_surround -> 5.1 bounce buffer.
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);

  std::array<float, kFrames> clip_l{};
  std::array<float, kFrames> clip_r{};
  clip_l.fill(1.0f);
  clip_r.fill(1.0f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

  // Azimuth -110 is the left-surround speaker (Ls) of the 5.1 bed (plane 4).
  const char* strip_json = R"({"version":1,"buses":[{"id":"master","role":"master"}],)"
                           R"("strips":[{"id":"s","surroundPan":{"azimuth":-110}}]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, strip_json) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = kFrames;
  options.block_size = kBlock;
  options.num_channels = 6;  // 5.1
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.normalize_lufs = 0;
  options.dither = 0;

  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.num_channels == 6);
  REQUIRE(result.frames == kFrames);
  REQUIRE(result.interleaved != nullptr);

  std::array<double, 6> energy{};
  for (int64_t f = 0; f < result.frames; ++f) {
    for (int ch = 0; ch < 6; ++ch) {
      const float v = result.interleaved[f * 6 + ch];
      energy[static_cast<size_t>(ch)] += static_cast<double>(v) * v;
    }
  }
  // The Ls plane (index 4) carries the lane; the front L/R/C planes stay near
  // silent and LFE (no lfe send) is exactly zero.
  REQUIRE(energy[4] > 1.0);
  REQUIRE(energy[0] < energy[4] * 1e-3);  // L
  REQUIRE(energy[1] < energy[4] * 1e-3);  // R
  REQUIRE(energy[2] < energy[4] * 1e-3);  // C
  REQUIRE(energy[3] == 0.0);              // LFE

  sonare_free_bounce_result(&result);
  sonare_engine_destroy(engine);
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("sonare_engine track lanes route clips and accept lane commands", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 10;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);

  std::array<float, kFrames> a_l{};
  std::array<float, kFrames> a_r{};
  std::array<float, kFrames> b_l{};
  std::array<float, kFrames> b_r{};
  a_l.fill(1.0f);
  a_r.fill(1.0f);
  b_l.fill(1.0f);
  b_r.fill(1.0f);
  const float* a_channels[] = {a_l.data(), a_r.data()};
  const float* b_channels[] = {b_l.data(), b_r.data()};

  SonareEngineClip clips[2]{};
  clips[0].id = 1;
  clips[0].track_id = 10;
  clips[0].channels = a_channels;
  clips[0].num_channels = 2;
  clips[0].num_samples = kFrames;
  clips[0].length_samples = kFrames;
  clips[0].gain = 1.0f;
  clips[1].id = 2;
  clips[1].track_id = 20;
  clips[1].channels = b_channels;
  clips[1].num_channels = 2;
  clips[1].num_samples = kFrames;
  clips[1].length_samples = kFrames;
  clips[1].gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, clips, 2) == SONARE_OK);

  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
#if defined(SONARE_WITH_MIXING)
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_lanes(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineTrackLane duplicate_lanes[] = {{10, nullptr, 0, 0, 1}, {10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, duplicate_lanes, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);
#else
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  REQUIRE(left.back() == 2.0f);
  REQUIRE(sonare_engine_set_solo_mute(engine, 0, 1, 0, -1) == SONARE_OK);
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);
  }
  REQUIRE(left.back() < 1.25f);
  REQUIRE(left.back() > 0.75f);

  REQUIRE(sonare_engine_set_parameter_smoothed(engine, engine_lane_param_target(0, 1), -12.0f,
                                               -1) == SONARE_OK);
  for (int block = 0; block < 30; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);
  }
  REQUIRE(left.back() < 0.45f);
  REQUIRE(right.back() < 0.45f);
#else
  REQUIRE(sonare_engine_set_solo_mute(engine, 0, 1, 0, -1) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine track buses route lane sends", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 40;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineBus buses[] = {{1, 0.0f, 1}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  SonareEngineBus duplicate_buses[] = {{1, 0.0f, 1}, {1, 0.0f, 1}};
  REQUIRE(sonare_engine_set_track_buses(engine, duplicate_buses, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A surround bus layout (5.1 = 2) is accepted; an out-of-range layout value
  // is rejected (the field is validated even though it is inert in phase 1).
  SonareEngineBus surround_bus[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_5_1}};
  REQUIRE(sonare_engine_set_track_buses(engine, surround_bus, 1) == SONARE_OK);
  SonareEngineBus bad_layout_bus[] = {{1, 0.0f, 99}};
  REQUIRE(sonare_engine_set_track_buses(engine, bad_layout_bus, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareEngineTrackSend send[] = {{1, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane lane[] = {{10, send, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  SonareEngineTrackLane bad_layout_lane[] = {{10, send, 1, 0, 99}};
  REQUIRE(sonare_engine_set_track_lanes(engine, bad_layout_lane, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineTrackLane null_send_lane[] = {{10, nullptr, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, null_send_lane, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineTrackSend bad_bus_send[] = {{99, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane bad_bus_lane[] = {{10, bad_bus_send, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, bad_bus_lane, 1) == SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineTrackSend duplicate_send[] = {{1, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER},
                                            {1, -6.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane duplicate_send_lane[] = {{10, duplicate_send, 2, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, duplicate_send_lane, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareEngineTrackSend bad_level_send[] = {{1, 99.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane bad_level_lane[] = {{10, bad_level_send, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, bad_level_lane, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(out.back() > 2.82f);
  REQUIRE(out.back() < 2.84f);
  std::array<SonareMeterTelemetryRecord, 8> meters{};
  size_t meter_count = 0;
  REQUIRE(sonare_engine_drain_meter_telemetry(engine, meters.data(), meters.size(), &meter_count) ==
          SONARE_OK);
  bool found_lane_meter = false;
  bool found_bus_meter = false;
  bool found_master_meter = false;
  for (size_t i = 0; i < meter_count; ++i) {
    found_lane_meter = found_lane_meter || meters[i].target_id == 1;
    found_bus_meter = found_bus_meter || meters[i].target_id == 33;
    found_master_meter = found_master_meter || meters[i].target_id == 0;
  }
  REQUIRE(found_lane_meter);
  REQUIRE(found_bus_meter);
  REQUIRE(found_master_meter);

  send[0].level_db = -6.0206f;
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  out.fill(0.0f);
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(out.back() > 2.11f);
  REQUIRE(out.back() < 2.13f);

  send[0].enabled = 0;
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  out.fill(0.0f);
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(out.back() > 1.41f);
  REQUIRE(out.back() < 1.42f);

  send[0].enabled = 1;
  send[0].level_db = 0.0f;
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_parameter_smoothed(engine, engine_bus_param_target(0, 1), -6.0206f,
                                               -1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  for (int block = 0; block < 30; ++block) {
    out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(out.back() > 2.11f);
  REQUIRE(out.back() < 2.13f);

  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, "{bad json") == SONARE_ERROR_INVALID_FORMAT);
  const char* bus_strip_json =
      R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":12,\"band0.enabled\":1}"}]}],"connections":[]})";
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_strip_json) == SONARE_OK);
#else
  SonareEngineBus buses[] = {{1, 0.0f, 1}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_track_strip_json processes lane strip", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 4;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> a{};
  std::array<float, kFrames> b{};
  a.fill(1.0f);
  b.fill(1.0f);
  const float* a_channels[] = {a.data()};
  const float* b_channels[] = {b.data()};

  SonareEngineClip clips[2]{};
  clips[0].id = 1;
  clips[0].track_id = 10;
  clips[0].channels = a_channels;
  clips[0].num_channels = 1;
  clips[0].num_samples = kFrames;
  clips[0].length_samples = kFrames;
  clips[0].gain = 1.0f;
  clips[1].id = 2;
  clips[1].track_id = 20;
  clips[1].channels = b_channels;
  clips[1].num_channels = 1;
  clips[1].num_samples = kFrames;
  clips[1].length_samples = kFrames;
  clips[1].gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, clips, 2) == SONARE_OK);

  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
#if defined(SONARE_WITH_MIXING)
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"track-10","faderDb":-12,"panLaw":3}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 0, scene_json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, "{bad json") ==
          SONARE_ERROR_INVALID_FORMAT);
  const char* unknown_processor_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"missing.processor","params":"{}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, unknown_processor_json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const char* bad_param_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.gainDb\":\"loud\"}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, bad_param_json) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(left.back() > 1.20f);
  REQUIRE(left.back() < 1.40f);
#else
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_track_strip_insert_bypassed toggles track insert", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":12,\"band0.enabled\":1}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_insert_bypassed(engine, 10, 7, 1, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> eq_out{};
  float* io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }

  REQUIRE(sonare_engine_set_track_strip_insert_bypassed(engine, 10, 0, 1, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, kBlock> bypassed_out{};
  io[0] = bypassed_out.data();
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(rms(eq_out) > rms(bypassed_out) * 1.5);
#else
  REQUIRE(sonare_engine_set_track_strip_insert_bypassed(engine, 10, 0, 1, 0) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_track_strip_eq_band_json updates embedded lane EQ",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 10,
              R"({"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]})") ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(engine, 99, 0,
                                                     R"({"type":"Peak","enabled":true})") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(engine, 10, 99,
                                                     R"({"type":"Peak","enabled":true})") ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> flat_out{};
  float* io[] = {flat_out.data()};
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(
              engine, 10, 0,
              R"({"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true})") ==
          SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, kBlock> eq_out{};
  io[0] = eq_out.data();
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(rms(eq_out) > rms(flat_out) * 1.5);
#else
  REQUIRE(sonare_engine_set_track_strip_eq_band_json(engine, 10, 0, "{}") ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_master_strip_json processes master strip", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 40;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> a{};
  std::array<float, kFrames> b{};
  a.fill(1.0f);
  b.fill(1.0f);
  const float* a_channels[] = {a.data()};
  const float* b_channels[] = {b.data()};

  SonareEngineClip clips[2]{};
  clips[0].id = 1;
  clips[0].channels = a_channels;
  clips[0].num_channels = 1;
  clips[0].num_samples = kFrames;
  clips[0].length_samples = kFrames;
  clips[0].gain = 1.0f;
  clips[1].id = 2;
  clips[1].channels = b_channels;
  clips[1].num_channels = 1;
  clips[1].num_samples = kFrames;
  clips[1].length_samples = kFrames;
  clips[1].gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, clips, 2) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"master","faderDb":-12,"panLaw":3}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, scene_json) == SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_json(engine, "{bad json") == SONARE_ERROR_INVALID_FORMAT);
  const char* unknown_processor_json =
      R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"missing.processor","params":"{}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, unknown_processor_json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const char* bad_param_json =
      R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.gainDb\":\"loud\"}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, bad_param_json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_master_strip_insert_bypassed(engine, 0, 1, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  float* io[] = {left.data()};
  for (int block = 0; block < 20; ++block) {
    left.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(left.back() > 0.65f);
  REQUIRE(left.back() < 0.80f);
  REQUIRE(sonare_engine_set_parameter_smoothed(engine, engine_master_param_target(1), -24.0f, -1) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_parameter(engine, engine_master_param_target(2), 0.25f, -1) ==
          SONARE_OK);
  for (int block = 0; block < 8; ++block) {
    left.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(left.back() > 0.05f);
  REQUIRE(left.back() < 0.25f);
#else
  REQUIRE(sonare_engine_set_master_strip_json(engine, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_master_strip_eq_band_json updates embedded master EQ",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  REQUIRE(sonare_engine_set_master_strip_json(
              engine, R"({"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]})") ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_eq_band_json(
              engine, 99, R"({"type":"Peak","enabled":true})") == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> flat_out{};
  float* io[] = {flat_out.data()};
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(
      sonare_engine_set_master_strip_eq_band_json(
          engine, 0, R"({"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true})") ==
      SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, kBlock> eq_out{};
  io[0] = eq_out.data();
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(rms(eq_out) > rms(flat_out) * 1.5);
#else
  REQUIRE(sonare_engine_set_master_strip_eq_band_json(engine, 0, "{}") ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine MIDI CC binding drives engine parameter", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 32, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_GRAPH)
  SonareParameterInfo parameter{};
  parameter.id = 7;
  std::strncpy(parameter.name, "gain", sizeof(parameter.name) - 1);
  parameter.min_value = -60.0f;
  parameter.max_value = 0.0f;
  parameter.default_value = 0.0f;
  parameter.rt_safe = 1;
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);

  SonareEngineGraphNode nodes[3]{};
  std::strncpy(nodes[0].id, "in", sizeof(nodes[0].id) - 1);
  nodes[0].type = 0;
  nodes[0].num_ports = 1;
  std::strncpy(nodes[1].id, "gain", sizeof(nodes[1].id) - 1);
  nodes[1].type = 1;
  nodes[1].gain_db = 0.0f;
  nodes[1].num_ports = 1;
  std::strncpy(nodes[2].id, "out", sizeof(nodes[2].id) - 1);
  nodes[2].type = 0;
  nodes[2].num_ports = 1;

  SonareEngineGraphConnection connections[2]{};
  std::strncpy(connections[0].source_node, "in", sizeof(connections[0].source_node) - 1);
  std::strncpy(connections[0].dest_node, "gain", sizeof(connections[0].dest_node) - 1);
  connections[0].mix = 1.0f;
  std::strncpy(connections[1].source_node, "gain", sizeof(connections[1].source_node) - 1);
  std::strncpy(connections[1].dest_node, "out", sizeof(connections[1].dest_node) - 1);
  connections[1].mix = 1.0f;

  SonareEngineGraphParameterBinding binding{};
  binding.param_id = 7;
  std::strncpy(binding.node_id, "gain", sizeof(binding.node_id) - 1);

  SonareEngineGraphSpec graph{};
  graph.nodes = nodes;
  graph.node_count = 3;
  graph.connections = connections;
  graph.connection_count = 2;
  graph.parameter_bindings = &binding;
  graph.parameter_binding_count = 1;
  std::strncpy(graph.input_node, "in", sizeof(graph.input_node) - 1);
  std::strncpy(graph.output_node, "out", sizeof(graph.output_node) - 1);
  graph.num_channels = 1;
  REQUIRE(sonare_engine_set_graph(engine, &graph) == SONARE_OK);

  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, 7, -60.0f, 0.0f) == SONARE_OK);
  size_t binding_count = 0;
  REQUIRE(sonare_engine_midi_cc_binding_count(engine, &binding_count) == SONARE_OK);
  REQUIRE(binding_count == 1);

  std::array<float, 64> audio{};
  audio.fill(1.0f);
  float* channels[] = {audio.data()};
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 74, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_process(engine, channels, 1, 64) == SONARE_OK);
  REQUIRE(audio[0] < 0.01f);

  REQUIRE(sonare_engine_clear_midi_cc_bindings(engine) == SONARE_OK);
  REQUIRE(sonare_engine_midi_cc_binding_count(engine, &binding_count) == SONARE_OK);
  REQUIRE(binding_count == 0);
#else
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, 7, -60.0f, 0.0f) == SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_bind_midi_cc(engine, 16, 74, 7, 0.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 128, 7, 0.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, 0, 0.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine live MIDI note renders through built-in instrument", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  SonareEngineBuiltinSynthConfig synth{};
  synth.gain = 0.5f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(std::max(peak_abs(left), peak_abs(right)) > 0.0f);

  REQUIRE(sonare_engine_push_midi_note_off(engine, 7, 0, 0, 60, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 16, 60, 100, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 0);
#else
  SonareEngineBuiltinSynthConfig synth{};
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine malformed SoundFont bytes report invalid format", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);

  const uint8_t bad_sf2[] = {'n', 'o', 't', ' ', 's', 'f', '2'};
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_load_soundfont(engine, bad_sf2, sizeof(bad_sf2)) ==
          SONARE_ERROR_INVALID_FORMAT);
#else
  REQUIRE(sonare_engine_load_soundfont(engine, bad_sf2, sizeof(bad_sf2)) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif
  REQUIRE(sonare_engine_load_soundfont(engine, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine scheduled MIDI clips render through built-in instrument",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  SonareEngineBuiltinSynthConfig synth{};
  synth.gain = 0.5f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 9, &synth) == SONARE_OK);

  const SonareEngineMidiEvent events[] = {
      {0, midi1_word(0x9, 0, 60, 100), 0, 0, 0, 1, 0, 0, 0},
      {4096, midi1_word(0x8, 0, 60, 0), 0, 0, 0, 1, 0, 0, 0},
  };
  SonareEngineMidiClipSchedule clip{};
  clip.id = 42;
  clip.track_id = 9;
  clip.length_samples = 8192;
  clip.destination_id = 9;
  clip.events = events;
  clip.event_count = 2;
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(std::max(peak_abs(left), peak_abs(right)) > 0.0f);

  SonareEngineMidiEvent bad_group_event = events[0];
  bad_group_event.group = 16;
  SonareEngineMidiClipSchedule bad_clip = clip;
  bad_clip.events = &bad_group_event;
  bad_clip.event_count = 1;
  REQUIRE(sonare_engine_set_midi_clips(engine, &bad_clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_set_midi_clips(engine, nullptr, 0) == SONARE_OK);
#else
  REQUIRE(sonare_engine_set_midi_clips(engine, nullptr, 0) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine converts PPQ to samples from the tempo map", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 60.0) == SONARE_OK);

  int64_t sample = 0;
  REQUIRE(sonare_engine_sample_at_ppq(engine, 1.5, &sample) == SONARE_OK);
  REQUIRE(sample == 72000);
  REQUIRE(sonare_engine_sample_at_ppq(engine, -1.0, &sample) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_sample_at_ppq(engine, std::numeric_limits<double>::quiet_NaN(), &sample) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_sample_at_ppq(engine, 1.0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine owned MIDI input source drains into instruments", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_push_midi_input_note_on(engine, 0, 0, 64, 100, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareEngineBuiltinSynthConfig synth{};
  synth.gain = 0.5f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 3, &synth) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_OK);

  REQUIRE(sonare_engine_push_midi_input_note_on(engine, 0, 0, 64, 100, 4) == SONARE_OK);
  size_t pending = 0;
  REQUIRE(sonare_engine_midi_input_pending_count(engine, &pending) == SONARE_OK);
  REQUIRE(pending == 1);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 64) == SONARE_OK);
  REQUIRE(peak_abs(left) > 0.01f);
  REQUIRE(sonare_engine_midi_input_pending_count(engine, &pending) == SONARE_OK);
  REQUIRE(pending == 0);

  REQUIRE(sonare_engine_clear_midi_input_source(engine) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_note_off(engine, 0, 0, 64, 0, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
#else
  REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_input_note_on(engine, 0, 0, 64, 100, 4) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine exposes live non-destructive MIDI FX inserts", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":12}") == SONARE_OK);
  REQUIRE(sonare_engine_clear_midi_fx(engine, 5) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{bad json") == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"quantize_ppq\":0}") ==
          SONARE_ERROR_INVALID_PARAMETER);
#else
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":12}") ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_clear_midi_fx(engine, 5) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_daw_editing_c_api_smoke", "[c_api]") {
  auto samples = generate_sine(440.0f, 22050, 0.25f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_pitch_correct_to_midi(samples.data(), samples.size(), 22050, 69.0f, 70.0f, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_note_stretch(samples.data(), samples.size(), 22050, 100, 1000, 1.25f, &out,
                              &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length > samples.size());
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change(samples.data(), samples.size(), 22050, 5.0f, 1.1f, &out,
                              &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change(samples.data(), samples.size(), 22050, 0.0f, 9.0f, &out,
                              &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  // Time-varying correction: a caller-supplied per-frame F0 contour (here a
  // constant 440 Hz track) corrected toward MIDI 70.
  const int hop = 512;
  const size_t n_frames = samples.size() / static_cast<size_t>(hop) + 1;
  std::vector<float> f0(n_frames, 440.0f);
  std::vector<float> voiced_prob(n_frames, 1.0f);
  std::vector<int32_t> voiced(n_frames, 1);
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_pitch_correct_to_midi_timevarying(samples.data(), samples.size(), 22050, f0.data(),
                                                   voiced_prob.data(), voiced.data(), n_frames, hop,
                                                   70.0f, &out, &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == samples.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  // NULL voiced / voiced_prob default to "all voiced"; only f0_hz is required.
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_pitch_correct_to_midi_timevarying(samples.data(), samples.size(), 22050, f0.data(),
                                                   nullptr, nullptr, n_frames, hop, 70.0f, &out,
                                                   &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  sonare_free_floats(out);

  // Invalid args are rejected (null f0, zero frames, bad hop, out-of-range target).
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_pitch_correct_to_midi_timevarying(samples.data(), samples.size(), 22050, nullptr,
                                                   nullptr, nullptr, n_frames, hop, 70.0f, &out,
                                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_pitch_correct_to_midi_timevarying(samples.data(), samples.size(), 22050, f0.data(),
                                                   nullptr, nullptr, n_frames, 0, 70.0f, &out,
                                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_pitch_correct_to_midi_timevarying(samples.data(), samples.size(), 22050, f0.data(),
                                                   nullptr, nullptr, n_frames, hop, 200.0f, &out,
                                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_voice_change_realtime processes mono and interleaved stereo buffers",
          "[c_api][voice_changer]") {
  std::vector<float> mono(384);
  for (size_t i = 0; i < mono.size(); ++i) {
    mono[i] =
        0.05f * std::sin(sonare::constants::kTwoPi * 220.0f * static_cast<float>(i) / 48000.0f);
  }

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_voice_change_realtime(mono.data(), mono.size(), 48000, "neutral-monitor", 1, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == mono.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  std::vector<float> stereo(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) {
    stereo[i * 2] = mono[i];
    stereo[i * 2 + 1] = -mono[i];
  }
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change_realtime(stereo.data(), stereo.size(), 48000, "soft-whisper", 2, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == stereo.size());
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(out[i]));
  }
  sonare_free_floats(out);

  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_voice_change_realtime(stereo.data(), stereo.size() - 1, 48000, "soft-whisper", 2,
                                       &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);

  REQUIRE(sonare_voice_change_realtime(mono.data(), mono.size(), 48000, "neutral-monitor", 3, &out,
                                       &out_length) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_voice_change_realtime compensates chain latency (no silent pre-roll)",
          "[c_api][voice_changer]") {
  // A character preset (soft-whisper) carries retune-grain latency. A naive
  // length-in/length-out render would prepend ~latency samples of silent
  // pre-roll and truncate the tail. The offline wrapper now skips the pre-roll
  // and flushes the tail, so a steady full-amplitude tone has real signal right
  // at the front rather than a silent gap.
  std::vector<float> tone(8192);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] =
        0.3f * std::sin(sonare::constants::kTwoPi * 220.0f * static_cast<float>(i) / 48000.0f);
  }

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_voice_change_realtime(tone.data(), tone.size(), 48000, "soft-whisper", 1, &out,
                                       &out_length) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length == tone.size());

  auto window_rms = [&](size_t begin, size_t end) {
    double sum = 0.0;
    for (size_t i = begin; i < end; ++i) sum += static_cast<double>(out[i]) * out[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
  };
  const double front_rms = window_rms(0, 512);
  const double total_rms = window_rms(0, out_length);
  REQUIRE(total_rms > 0.0);
  // With the latency bug the leading window would be near-silent pre-roll; with
  // compensation it carries energy comparable to the rest of the signal.
  REQUIRE(front_rms > 0.25 * total_rms);

  sonare_free_floats(out);
}

TEST_CASE("sonare_realtime_voice_changer ISP limiter fields round-trip through the POD config",
          "[c_api]") {
  SonareRealtimeVoiceChangerConfig config{};
  REQUIRE(sonare_realtime_voice_changer_preset_config(SONARE_VC_PRESET_NEUTRAL_MONITOR, &config) ==
          SONARE_OK);
  config.limiter_enable_isp_limiter = 0;
  config.limiter_isp_ceiling_dbtp = -2.5f;

  SonareRealtimeVoiceChanger* handle = nullptr;
  REQUIRE(sonare_realtime_voice_changer_create(&config, 48000, 128, 1, &handle) == SONARE_OK);
  REQUIRE(handle != nullptr);

  SonareRealtimeVoiceChangerConfig read_back{};
  REQUIRE(sonare_realtime_voice_changer_get_config(handle, &read_back) == SONARE_OK);
  REQUIRE(read_back.limiter_enable_isp_limiter == 0);
  REQUIRE(read_back.limiter_isp_ceiling_dbtp == Catch::Approx(-2.5f).margin(0.001f));

  sonare_realtime_voice_changer_destroy(handle);
}

TEST_CASE("sonare_engine_get_transport_state surfaces bar position and time signature", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 120.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_time_signature(engine, 3, 4) == SONARE_OK);

  SonareTransportState state{};
  REQUIRE(sonare_engine_get_transport_state(engine, &state) == SONARE_OK);
  REQUIRE(state.time_signature.numerator == 3);
  REQUIRE(state.time_signature.denominator == 4);
  REQUIRE(state.bar_count >= 0);
  REQUIRE(std::isfinite(state.bar_start_ppq));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_get_transport_state polls safely during processing", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 120.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_loop(engine, 0.0, 4.0, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::atomic<bool> done{false};
  std::atomic<bool> invalid{false};
  std::thread poller([&] {
    while (!done.load(std::memory_order_acquire)) {
      SonareTransportState state{};
      if (sonare_engine_get_transport_state(engine, &state) != SONARE_OK ||
          !std::isfinite(state.ppq_position) || !std::isfinite(state.bpm) ||
          !std::isfinite(state.loop_start_ppq) || !std::isfinite(state.loop_end_ppq)) {
        invalid.store(true, std::memory_order_release);
        break;
      }
    }
  });

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  float* channels[] = {left.data(), right.data()};
  for (int i = 0; i < 1000; ++i) {
    REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  }

  done.store(true, std::memory_order_release);
  poller.join();
  REQUIRE_FALSE(invalid.load(std::memory_order_acquire));
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline NULLs the owned result on validation failure", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 16, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions bad_options{};
  bad_options.total_frames = 16;
  bad_options.block_size = 16;
  bad_options.num_channels = 0;  // invalid -> early validation failure
  bad_options.source_sample_rate = 48000;
  bad_options.target_sample_rate = 48000;

  // Pre-dirty the result so we can prove the failure path overwrites it with NULL
  // rather than leaving a dangling owned pointer that the free idiom would delete.
  SonareEngineBounceResult result{};
  result.interleaved = reinterpret_cast<float*>(0xDEADBEEF);
  result.frames = 123;

  REQUIRE(sonare_engine_bounce_offline(engine, &bad_options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.interleaved == nullptr);
  REQUIRE(result.frames == 0);
  sonare_free_floats(result.interleaved);  // must be a safe no-op on NULL

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_bounce_offline maps non-finite loudness targets to the default",
          "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = 4800;
  options.normalize_lufs = 1;
  options.target_lufs = std::numeric_limits<float>::quiet_NaN();

  // A garbage (NaN) target must behave like the 0.0f "use default" sentinel
  // instead of propagating NaN into the normalisation gain.
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  bool all_finite = true;
  for (size_t i = 0; i < result.sample_count; ++i) {
    all_finite = all_finite && std::isfinite(result.interleaved[i]);
  }
  REQUIRE(all_finite);
  sonare_free_bounce_result(&result);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_realtime_engine_c_api_smoke", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 60.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_time_signature(engine, 3, 4) == SONARE_OK);
  SonareEngineMarker markers[2]{};
  markers[0].id = 11;
  markers[0].ppq = 1.0;
  std::strncpy(markers[0].name, "intro", sizeof(markers[0].name) - 1);
  markers[1].id = 12;
  markers[1].ppq = 2.0;
  std::strncpy(markers[1].name, "out", sizeof(markers[1].name) - 1);
  REQUIRE(sonare_engine_set_markers(engine, markers, 2) == SONARE_OK);
  size_t marker_count = 0;
  REQUIRE(sonare_engine_marker_count(engine, &marker_count) == SONARE_OK);
  REQUIRE(marker_count == 2);
  SonareEngineMarker marker_out{};
  REQUIRE(sonare_engine_marker_by_index(engine, 0, &marker_out) == SONARE_OK);
  REQUIRE(marker_out.id == 11);
  REQUIRE(std::strcmp(marker_out.name, "intro") == 0);
  REQUIRE(sonare_engine_marker(engine, 12, &marker_out) == SONARE_OK);
  REQUIRE(marker_out.ppq == Catch::Approx(2.0));
  REQUIRE(sonare_engine_set_loop_from_markers(engine, 11, 12) == SONARE_OK);
  REQUIRE(sonare_engine_seek_marker(engine, 11, -1) == SONARE_OK);
  SonareEngineMetronomeConfig metronome{};
  metronome.enabled = 1;
  metronome.beat_gain = 0.25f;
  metronome.accent_gain = 0.75f;
  metronome.click_samples = 16;
  REQUIRE(sonare_engine_set_metronome(engine, &metronome) == SONARE_OK);
  SonareEngineMetronomeConfig metronome_out{};
  REQUIRE(sonare_engine_metronome(engine, &metronome_out) == SONARE_OK);
  REQUIRE(metronome_out.enabled == 1);
  REQUIRE(metronome_out.click_samples == 16);
  int64_t count_in_end = 0;
  REQUIRE(sonare_engine_count_in_end_sample(engine, 0, 2, &count_in_end) == SONARE_OK);
  REQUIRE(count_in_end == 288000);
  metronome.enabled = 0;
  REQUIRE(sonare_engine_set_metronome(engine, &metronome) == SONARE_OK);

  SonareParameterInfo parameter{};
  parameter.id = 7;
  std::strncpy(parameter.name, "gain", sizeof(parameter.name) - 1);
  std::strncpy(parameter.unit, "dB", sizeof(parameter.unit) - 1);
  parameter.min_value = -60.0f;
  parameter.max_value = 12.0f;
  parameter.default_value = 0.0f;
  parameter.rt_safe = 1;
  parameter.default_curve = 0;  // canonical AutomationCurve::Linear
  REQUIRE(sonare_engine_add_parameter(engine, &parameter) == SONARE_OK);
  size_t parameter_count = 0;
  REQUIRE(sonare_engine_parameter_count(engine, &parameter_count) == SONARE_OK);
  REQUIRE(parameter_count == 1);
  SonareParameterInfo parameter_out{};
  REQUIRE(sonare_engine_parameter_info_by_index(engine, 0, &parameter_out) == SONARE_OK);
  REQUIRE(parameter_out.id == 7);
  REQUIRE(std::strcmp(parameter_out.name, "gain") == 0);

  const SonareAutomationPoint points[] = {{0.0, 0.0f, 0}, {1.0, 6.0205999f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, 7, points, 2) == SONARE_OK);
  size_t lane_count = 0;
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 1);

  SonareEngineGraphNode graph_nodes[3]{};
  std::strncpy(graph_nodes[0].id, "in", sizeof(graph_nodes[0].id) - 1);
  graph_nodes[0].type = 0;
  graph_nodes[0].num_ports = 2;
  std::strncpy(graph_nodes[1].id, "gain", sizeof(graph_nodes[1].id) - 1);
  graph_nodes[1].type = 1;
  graph_nodes[1].gain_db = 0.0f;
  graph_nodes[1].num_ports = 2;
  std::strncpy(graph_nodes[2].id, "out", sizeof(graph_nodes[2].id) - 1);
  graph_nodes[2].type = 0;
  graph_nodes[2].num_ports = 2;
  SonareEngineGraphConnection graph_connections[4]{};
  std::strncpy(graph_connections[0].source_node, "in",
               sizeof(graph_connections[0].source_node) - 1);
  std::strncpy(graph_connections[0].dest_node, "gain", sizeof(graph_connections[0].dest_node) - 1);
  graph_connections[0].source_port = 0;
  graph_connections[0].dest_port = 0;
  graph_connections[0].mix = 1;
  std::strncpy(graph_connections[1].source_node, "in",
               sizeof(graph_connections[1].source_node) - 1);
  std::strncpy(graph_connections[1].dest_node, "gain", sizeof(graph_connections[1].dest_node) - 1);
  graph_connections[1].source_port = 1;
  graph_connections[1].dest_port = 1;
  graph_connections[1].mix = 1;
  std::strncpy(graph_connections[2].source_node, "gain",
               sizeof(graph_connections[2].source_node) - 1);
  std::strncpy(graph_connections[2].dest_node, "out", sizeof(graph_connections[2].dest_node) - 1);
  graph_connections[2].source_port = 0;
  graph_connections[2].dest_port = 0;
  graph_connections[2].mix = 1;
  std::strncpy(graph_connections[3].source_node, "gain",
               sizeof(graph_connections[3].source_node) - 1);
  std::strncpy(graph_connections[3].dest_node, "out", sizeof(graph_connections[3].dest_node) - 1);
  graph_connections[3].source_port = 1;
  graph_connections[3].dest_port = 1;
  graph_connections[3].mix = 1;
  SonareEngineGraphSpec graph_spec{};
  graph_spec.nodes = graph_nodes;
  graph_spec.node_count = 3;
  graph_spec.connections = graph_connections;
  graph_spec.connection_count = 4;
  SonareEngineGraphParameterBinding graph_bindings[1]{};
  graph_bindings[0].param_id = 7;
  std::strncpy(graph_bindings[0].node_id, "gain", sizeof(graph_bindings[0].node_id) - 1);
  graph_spec.parameter_bindings = graph_bindings;
  graph_spec.parameter_binding_count = 1;
  std::strncpy(graph_spec.input_node, "in", sizeof(graph_spec.input_node) - 1);
  std::strncpy(graph_spec.output_node, "out", sizeof(graph_spec.output_node) - 1);
  graph_spec.num_channels = 2;
  REQUIRE(sonare_engine_set_graph(engine, &graph_spec) == SONARE_OK);
  size_t graph_node_count = 0;
  size_t graph_connection_count = 0;
  REQUIRE(sonare_engine_graph_node_count(engine, &graph_node_count) == SONARE_OK);
  REQUIRE(sonare_engine_graph_connection_count(engine, &graph_connection_count) == SONARE_OK);
  REQUIRE(graph_node_count == 3);
  REQUIRE(graph_connection_count == 4);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.125f);
  clip_right.fill(-0.125f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 101;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 1.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  size_t clip_count = 0;
  REQUIRE(sonare_engine_clip_count(engine, &clip_count) == SONARE_OK);
  REQUIRE(clip_count == 1);

  std::array<float, 128> capture_left{};
  std::array<float, 128> capture_right{};
  float* capture_channels[] = {capture_left.data(), capture_right.data()};
  SonareEngineCaptureBuffer capture_buffer{};
  capture_buffer.channels = capture_channels;
  capture_buffer.num_channels = 2;
  capture_buffer.capacity_frames = 128;
  REQUIRE(sonare_engine_set_capture_buffer(engine, &capture_buffer) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_punch(engine, 48000, 48128, 1) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.75f).margin(0.0001f));

  SonareEngineCaptureStatus capture_status{};
  REQUIRE(sonare_engine_capture_status(engine, &capture_status) == SONARE_OK);
  REQUIRE(capture_status.captured_frames == 128);
  REQUIRE(capture_status.overflow_count == 0);
  REQUIRE(capture_status.armed == 1);
  REQUIRE(capture_status.source == SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT);
  REQUIRE(capture_status.record_offset_samples == 0);
  REQUIRE(capture_left[0] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(capture_right[0] == Catch::Approx(-0.75f).margin(0.0001f));
  REQUIRE(sonare_engine_reset_capture(engine) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &capture_status) == SONARE_OK);
  REQUIRE(capture_status.captured_frames == 0);

  std::array<SonareEngineTelemetry, 4> telemetry{};
  size_t written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  REQUIRE(written > 0);
  REQUIRE(telemetry[written - 1].render_frame == 0);
  REQUIRE(telemetry[written - 1].timeline_sample == 48000 + 128);

  REQUIRE(sonare_engine_render_offline(engine, channels, 2, 128, 128) == SONARE_OK);
  SonareEngineBounceOptions bounce_options{};
  bounce_options.total_frames = 128;
  bounce_options.block_size = 128;
  bounce_options.num_channels = 2;
  bounce_options.source_sample_rate = 48000;
  bounce_options.target_sample_rate = 24000;
  bounce_options.normalize_lufs = 0;
  bounce_options.dither = 0;
  SonareEngineBounceResult bounce{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce_options, &bounce) == SONARE_OK);
  REQUIRE(bounce.interleaved != nullptr);
  REQUIRE(bounce.frames == 64);
  REQUIRE(bounce.num_channels == 2);
  REQUIRE(bounce.sample_rate == 24000);
  REQUIRE(bounce.sample_count == 128);
  REQUIRE((std::isfinite(bounce.integrated_lufs) || std::isinf(bounce.integrated_lufs)));
  sonare_free_floats(bounce.interleaved);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare engine capture can record live input and record offset metadata", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.5f);
  clip_right.fill(-0.5f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 202;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 0.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  std::array<float, 128> capture_left{};
  std::array<float, 128> capture_right{};
  float* capture_channels[] = {capture_left.data(), capture_right.data()};
  SonareEngineCaptureBuffer capture_buffer{};
  capture_buffer.channels = capture_channels;
  capture_buffer.num_channels = 2;
  capture_buffer.capacity_frames = 128;
  REQUIRE(sonare_engine_set_capture_buffer(engine, &capture_buffer) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_source(engine, SONARE_ENGINE_CAPTURE_SOURCE_INPUT) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_capture_source(engine, static_cast<SonareEngineCaptureSource>(99)) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_record_offset_samples(engine, -37) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.75f).margin(0.0001f));
  std::array<SonareMeterTelemetryRecord, 4> input_meters{};
  size_t input_meter_count = 0;
  REQUIRE(sonare_engine_drain_meter_telemetry(engine, input_meters.data(), input_meters.size(),
                                              &input_meter_count) == SONARE_OK);
  bool found_input_meter = false;
  for (size_t i = 0; i < input_meter_count; ++i) {
    if (input_meters[i].target_id == 0xFFFFu) {
      found_input_meter = true;
      REQUIRE(input_meters[i].peak_db_l == Catch::Approx(-12.0412f).margin(0.05f));
    }
  }
  REQUIRE(found_input_meter);

  SonareEngineCaptureStatus status{};
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.captured_frames == 128);
  REQUIRE(status.source == SONARE_ENGINE_CAPTURE_SOURCE_INPUT);
  REQUIRE(status.record_offset_samples == -37);
  REQUIRE(capture_left[0] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(capture_right[0] == Catch::Approx(-0.25f).margin(0.0001f));

  REQUIRE(sonare_engine_set_input_monitor(engine, 0, 1.0f) == SONARE_OK);
  REQUIRE(sonare_engine_set_input_monitor(engine, 1, std::numeric_limits<float>::infinity()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_reset_capture(engine) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  left.fill(0.25f);
  right.fill(-0.25f);
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.5f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.5f).margin(0.0001f));
  REQUIRE(capture_left[0] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(capture_right[0] == Catch::Approx(-0.25f).margin(0.0001f));

  REQUIRE(sonare_engine_set_input_monitor(engine, 1, 0.5f) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  left.fill(0.25f);
  right.fill(-0.25f);
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.625f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.625f).margin(0.0001f));

  capture_left.fill(0.0f);
  capture_right.fill(0.0f);
  REQUIRE(sonare_engine_set_input_monitor(engine, 1, 1.0f) == SONARE_OK);
  REQUIRE(sonare_engine_set_capture_punch(engine, 64, 128, 1) == SONARE_OK);
  REQUIRE(sonare_engine_reset_capture(engine) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  left.fill(0.25f);
  right.fill(-0.25f);
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.captured_frames == 64);
  REQUIRE(capture_left[0] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(capture_right[0] == Catch::Approx(-0.25f).margin(0.0001f));

  capture_left.fill(0.0f);
  capture_right.fill(0.0f);
  REQUIRE(sonare_engine_set_capture_punch(engine, 0, 128, 1) == SONARE_OK);
  REQUIRE(sonare_engine_reset_capture(engine) == SONARE_OK);
  REQUIRE(sonare_engine_arm_capture(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  left.fill(0.25f);
  right.fill(-0.25f);
  REQUIRE(sonare_engine_process(engine, channels, 2, 64) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.captured_frames == 64);

  REQUIRE(sonare_engine_stop(engine, -1) == SONARE_OK);
  for (int i = 0; i < 3; ++i) {
    left.fill(0.5f);
    right.fill(-0.5f);
    REQUIRE(sonare_engine_process(engine, channels, 2, 64) == SONARE_OK);
  }
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.captured_frames == 64);
  REQUIRE(status.overflow_count == 0);
  REQUIRE(capture_left[63] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(capture_left[64] == Catch::Approx(0.0f).margin(0.0001f));

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  left.fill(0.75f);
  right.fill(-0.75f);
  REQUIRE(sonare_engine_process(engine, channels, 2, 64) == SONARE_OK);
  REQUIRE(sonare_engine_capture_status(engine, &status) == SONARE_OK);
  REQUIRE(status.captured_frames == 128);
  REQUIRE(capture_left[64] == Catch::Approx(0.75f).margin(0.0001f));
  REQUIRE(capture_right[64] == Catch::Approx(-0.75f).margin(0.0001f));

  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("sonare_engine_process_with_monitor returns a separate monitor bus", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 16, 16, 16) == SONARE_OK);

  std::array<float, 16> left{};
  std::array<float, 16> right{};
  left.fill(0.25f);
  right.fill(-0.25f);
  float* channels[] = {left.data(), right.data()};

  std::array<float, 16> monitor_left{};
  std::array<float, 16> monitor_right{};
  monitor_left.fill(99.0f);
  monitor_right.fill(99.0f);
  float* monitor_channels[] = {monitor_left.data(), monitor_right.data()};

  REQUIRE(sonare_engine_process_with_monitor(engine, channels, monitor_channels, 2, 16) ==
          SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.25f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.25f).margin(0.0001f));
  REQUIRE(monitor_left[0] == Catch::Approx(0.0f).margin(0.0001f));
  REQUIRE(monitor_right[0] == Catch::Approx(0.0f).margin(0.0001f));

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_VOICE_CHANGER
TEST_CASE("sonare_realtime_engine_freeze_c_api_matches_clip_playback", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 64, 64) == SONARE_OK);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.125f);
  clip_right.fill(-0.25f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 7;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 0.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  SonareEngineFreezeOptions freeze_options{};
  freeze_options.total_frames = 128;
  freeze_options.block_size = 128;
  freeze_options.num_channels = 2;
  freeze_options.clip_id = 77;
  freeze_options.start_ppq = 0.0;
  freeze_options.gain = 1.0f;
  SonareEngineFreezeResult freeze{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze_options, &freeze) == SONARE_OK);
  REQUIRE(freeze.clip_id == 77);
  REQUIRE(freeze.frames == 128);
  REQUIRE(freeze.num_channels == 2);
  size_t clip_count = 0;
  REQUIRE(sonare_engine_clip_count(engine, &clip_count) == SONARE_OK);
  REQUIRE(clip_count == 1);

  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, 128> left{};
  std::array<float, 128> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, 128, 128) == SONARE_OK);
  REQUIRE(left[0] == Catch::Approx(0.125f).margin(0.0001f));
  REQUIRE(right[0] == Catch::Approx(-0.25f).margin(0.0001f));
  REQUIRE(left[127] == Catch::Approx(0.125f).margin(0.0001f));
  REQUIRE(right[127] == Catch::Approx(-0.25f).margin(0.0001f));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_clips_accepts_repitch_warp_anchors", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 4, 64, 64) == SONARE_OK);

  std::array<float, 4> source{0.0f, 10.0f, 20.0f, 30.0f};
  const float* clip_channels[] = {source.data()};
  SonareEngineWarpAnchor anchors[] = {{0.0, 0.0}, {3.0, 1.5}};
  SonareEngineClip clip{};
  clip.id = 303;
  clip.channels = clip_channels;
  clip.num_channels = 1;
  clip.num_samples = 4;
  clip.start_ppq = 0.0;
  clip.length_samples = 4;
  clip.gain = 1.0f;
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_REPITCH;
  clip.warp_anchors = anchors;
  clip.warp_anchor_count = 2;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, 4> out{};
  float* channels[] = {out.data()};
  REQUIRE(sonare_engine_process(engine, channels, 1, 4) == SONARE_OK);
  REQUIRE(out[0] == Catch::Approx(0.0f).margin(0.0001f));
  REQUIRE(out[1] == Catch::Approx(5.0f).margin(0.0001f));
  REQUIRE(out[2] == Catch::Approx(10.0f).margin(0.0001f));
  REQUIRE(out[3] == Catch::Approx(15.0f).margin(0.0001f));

  clip.warp_mode = 99;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

  clip.warp_mode = SONARE_ENGINE_WARP_MODE_REPITCH;
  clip.loop = 1;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_clips_prebakes_direct_tempo_sync_warp", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 8192, 64, 64) == SONARE_OK);

  std::vector<float> source(4096);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = std::sin(static_cast<float>(i) * 0.02f);
  }
  const float* clip_channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 304;
  clip.channels = clip_channels;
  clip.num_channels = 1;
  clip.num_samples = static_cast<int64_t>(source.size());
  clip.start_ppq = 0.0;
  clip.length_samples = 8192;
  clip.gain = 1.0f;
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> out(8192, 0.0f);
  float* channels[] = {out.data()};
  REQUIRE(sonare_engine_process(engine, channels, 1, static_cast<int>(out.size())) == SONARE_OK);

  float peak = 0.0f;
  for (float v : out) {
    REQUIRE(std::isfinite(v));
    peak = std::max(peak, std::abs(v));
  }
  REQUIRE(peak > 0.1f);
  REQUIRE(std::abs(out.back()) > 0.0001f);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_clips_prebakes_direct_tempo_sync_segment_rates", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 8192, 64, 64) == SONARE_OK);

  std::vector<float> source(4096);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = std::sin(static_cast<float>(i) * 0.02f);
  }
  const float* clip_channels[] = {source.data()};
  const SonareEngineWarpAnchor anchors[] = {
      {0.0, 0.0},
      {2048.0, 1024.0},
      {8192.0, 4096.0},
  };
  SonareEngineClip clip{};
  clip.id = 305;
  clip.channels = clip_channels;
  clip.num_channels = 1;
  clip.num_samples = static_cast<int64_t>(source.size());
  clip.start_ppq = 0.0;
  clip.length_samples = 8192;
  clip.gain = 1.0f;
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  clip.warp_anchors = anchors;
  clip.warp_anchor_count = 3;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> out(8192, 0.0f);
  float* channels[] = {out.data()};
  REQUIRE(sonare_engine_process(engine, channels, 1, static_cast<int>(out.size())) == SONARE_OK);

  float peak = 0.0f;
  for (float v : out) {
    REQUIRE(std::isfinite(v));
    peak = std::max(peak, std::abs(v));
  }
  REQUIRE(peak > 0.1f);

  clip.length_samples = 4096;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::fill(out.begin(), out.end(), 0.0f);
  REQUIRE(sonare_engine_process(engine, channels, 1, static_cast<int>(out.size())) == SONARE_OK);
  float head_peak = 0.0f;
  float tail_peak = 0.0f;
  for (size_t i = 0; i < out.size(); ++i) {
    REQUIRE(std::isfinite(out[i]));
    if (i < 4096) {
      head_peak = std::max(head_peak, std::abs(out[i]));
    } else {
      tail_peak = std::max(tail_peak, std::abs(out[i]));
    }
  }
  REQUIRE(head_peak > 0.1f);
  REQUIRE(tail_peak == Catch::Approx(0.0f).margin(0.0001f));

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine streams paged clip providers and reports page misses", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 8, 16, 16) == SONARE_OK);

  SonareClipPageProvider* provider = nullptr;
  REQUIRE(sonare_clip_page_provider_create(1, 8, 4, &provider) == SONARE_OK);
  REQUIRE(provider != nullptr);

  std::array<float, 2> short_page0{1.0f, 2.0f};
  const float* short_page0_channels[] = {short_page0.data()};
  REQUIRE(sonare_clip_page_provider_supply(provider, 0, short_page0_channels, 1, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  std::array<float, 4> page0{1.0f, 2.0f, 3.0f, 4.0f};
  const float* page0_channels[] = {page0.data()};
  REQUIRE(sonare_clip_page_provider_supply(provider, 0, page0_channels, 1, 4) == SONARE_OK);

  SonareEngineClip clip{};
  clip.id = 123;
  clip.start_ppq = 0.0;
  clip.clip_offset_samples = 0;
  clip.length_samples = 8;
  clip.gain = 1.0f;
  clip.page_provider = provider;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, 8> out{};
  float* channels[] = {out.data()};
  REQUIRE(sonare_engine_process(engine, channels, 1, static_cast<int>(out.size())) == SONARE_OK);
  REQUIRE(out[0] == Catch::Approx(1.0f));
  REQUIRE(out[3] == Catch::Approx(4.0f));
  REQUIRE(out[4] == Catch::Approx(0.0f));
  REQUIRE(out[7] == Catch::Approx(0.0f));

  SonareClipPageRequest request{};
  int has_request = 0;
  REQUIRE(sonare_engine_pop_clip_page_request(engine, &request, &has_request) == SONARE_OK);
  REQUIRE(has_request == 1);
  REQUIRE(request.clip_id == 123);
  REQUIRE(request.channel == 0);
  REQUIRE(request.sample == 4);

  std::array<SonareEngineTelemetry, 8> telemetry{};
  size_t written = 0;
  REQUIRE(sonare_engine_drain_telemetry(engine, telemetry.data(), telemetry.size(), &written) ==
          SONARE_OK);
  bool found_underrun = false;
  for (size_t i = 0; i < written; ++i) {
    found_underrun = found_underrun || (telemetry[i].type == 1 && telemetry[i].value == 123);
  }
  REQUIRE(found_underrun);

  std::array<float, 4> page1{5.0f, 6.0f, 7.0f, 8.0f};
  const float* page1_channels[] = {page1.data()};
  REQUIRE(sonare_clip_page_provider_supply(provider, 1, page1_channels, 1, 4) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  out.fill(0.0f);
  REQUIRE(sonare_engine_process(engine, channels, 1, static_cast<int>(out.size())) == SONARE_OK);
  REQUIRE(out[4] == Catch::Approx(5.0f));
  REQUIRE(out[7] == Catch::Approx(8.0f));

  sonare_clip_page_provider_destroy(provider);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine offline bounce and freeze consume paged clip providers",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 8, 16, 16) == SONARE_OK);

  SonareClipPageProvider* provider = nullptr;
  REQUIRE(sonare_clip_page_provider_create(1, 8, 4, &provider) == SONARE_OK);

  std::array<float, 4> page0{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> page1{5.0f, 6.0f, 7.0f, 8.0f};
  const float* page0_channels[] = {page0.data()};
  const float* page1_channels[] = {page1.data()};
  REQUIRE(sonare_clip_page_provider_supply(provider, 0, page0_channels, 1, 4) == SONARE_OK);
  REQUIRE(sonare_clip_page_provider_supply(provider, 1, page1_channels, 1, 4) == SONARE_OK);

  SonareEngineClip clip{};
  clip.id = 321;
  clip.start_ppq = 0.0;
  clip.clip_offset_samples = 0;
  clip.length_samples = 0;
  clip.gain = 1.0f;
  clip.page_provider = provider;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  SonareEngineBounceOptions bounce_options{};
  REQUIRE(sonare_engine_bounce_options_default(&bounce_options) == SONARE_OK);
  bounce_options.total_frames = 8;
  bounce_options.block_size = 4;
  bounce_options.num_channels = 1;
  SonareEngineBounceResult bounce{};
  REQUIRE(sonare_engine_bounce_offline(engine, &bounce_options, &bounce) == SONARE_OK);
  REQUIRE(bounce.frames == 8);
  REQUIRE(bounce.num_channels == 1);
  REQUIRE(bounce.sample_count == 8);
  REQUIRE(bounce.interleaved != nullptr);
  for (size_t i = 0; i < bounce.sample_count; ++i) {
    REQUIRE(bounce.interleaved[i] == Catch::Approx(static_cast<float>(i + 1)).margin(0.0001f));
  }
  sonare_free_bounce_result(&bounce);

  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  SonareEngineFreezeOptions freeze_options{};
  freeze_options.total_frames = 8;
  freeze_options.block_size = 4;
  freeze_options.num_channels = 1;
  freeze_options.clip_id = 654;
  freeze_options.start_ppq = 0.0;
  freeze_options.gain = 1.0f;
  SonareEngineFreezeResult freeze{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze_options, &freeze) == SONARE_OK);
  REQUIRE(freeze.clip_id == 654);
  REQUIRE(freeze.frames == 8);
  REQUIRE(freeze.num_channels == 1);

  REQUIRE(sonare_clip_page_provider_clear(provider, 0) == SONARE_OK);
  REQUIRE(sonare_clip_page_provider_clear(provider, 1) == SONARE_OK);
  sonare_clip_page_provider_destroy(provider);

  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, 8> frozen_out{};
  float* channels[] = {frozen_out.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, 1, 8, 4) == SONARE_OK);
  for (size_t i = 0; i < frozen_out.size(); ++i) {
    REQUIRE(frozen_out[i] == Catch::Approx(static_cast<float>(i + 1)).margin(0.0001f));
  }

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_realtime_engine_freeze_c_api_preserves_explicit_zero_gain", "[c_api]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 64, 64) == SONARE_OK);

  std::array<float, 128> clip_left{};
  std::array<float, 128> clip_right{};
  clip_left.fill(0.5f);
  clip_right.fill(-0.5f);
  const float* clip_channels[] = {clip_left.data(), clip_right.data()};
  SonareEngineClip clip{};
  clip.id = 7;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = 128;
  clip.start_ppq = 0.0;
  clip.length_samples = 128;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  SonareEngineFreezeOptions freeze_options{};
  freeze_options.total_frames = 128;
  freeze_options.block_size = 128;
  freeze_options.num_channels = 2;
  freeze_options.clip_id = 77;
  freeze_options.start_ppq = 0.0;
  freeze_options.gain = 0.0f;
  SonareEngineFreezeResult freeze{};
  REQUIRE(sonare_engine_freeze_offline(engine, &freeze_options, &freeze) == SONARE_OK);

  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  std::array<float, 128> left{};
  std::array<float, 128> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(engine, channels, 2, 128, 128) == SONARE_OK);
  for (size_t i = 0; i < left.size(); ++i) {
    REQUIRE(left[i] == Catch::Approx(0.0f).margin(0.0001f));
    REQUIRE(right[i] == Catch::Approx(0.0f).margin(0.0001f));
  }

  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("sonare_last_error_message", "[c_api]") {
  SECTION("never returns null pointer") {
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
  }

  SECTION("captures detailed message when a C API call fails") {
    // 12 bytes of random non-audio data so format detection returns Unknown.
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    SonareAudio* audio = nullptr;

    SonareError err = sonare_audio_from_memory(garbage.data(), garbage.size(), &audio);
#ifdef SONARE_WITH_FFMPEG
    // With FFmpeg the buffer still fails to decode but the message comes from
    // FFmpeg rather than the static "Unsupported audio format" path.
    REQUIRE(err != SONARE_OK);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    REQUIRE(std::strlen(msg) > 0);
#else
    REQUIRE(err == SONARE_ERROR_INVALID_FORMAT);
    const char* msg = sonare_last_error_message();
    REQUIRE(msg != nullptr);
    // The detailed message must be more informative than the generic code label.
    REQUIRE(std::string(msg).find("Unsupported audio format") != std::string::npos);
    REQUIRE(std::string(msg).find("WAV, MP3") != std::string::npos);
    REQUIRE(std::string(msg).find("ffmpeg") != std::string::npos);
#endif
    REQUIRE(audio == nullptr);
  }
}

TEST_CASE("sonare_mastering_insert_param_info reports realtime param descriptors",
          "[c_api][mastering]") {
  // Unknown processor / null name yield an empty JSON array.
  REQUIRE(std::string(sonare_mastering_insert_param_info("nope.nope")) == "[]");
  REQUIRE(std::string(sonare_mastering_insert_param_info(nullptr)) == "[]");

  const std::string fdn = sonare_mastering_insert_param_info("effects.reverb.fdn");
  if (fdn == "[]") {
    SUCCEED("FX processors not built; realtime reverb descriptors unavailable");
    return;
  }
  // dryWet is realtime-safe and exposed with its integer id.
  REQUIRE(fdn.find("\"name\":\"dryWet\"") != std::string::npos);
  REQUIRE(fdn.find("\"rtSafe\":true") != std::string::npos);

  // Dattorro publishes a non-realtime-safe parameter (modDepthSamples, id 4,
  // which grows allpass buffers); the descriptor must flag it accordingly.
  const std::string dat = sonare_mastering_insert_param_info("effects.reverb.dattorro");
  REQUIRE(dat.find("\"name\":\"modDepthSamples\"") != std::string::npos);
  REQUIRE(dat.find("\"rtSafe\":false") != std::string::npos);
}

TEST_CASE("sonare_engine_set_track_strip_insert_param_by_name changes reverb mix in realtime",
          "[c_api][engine]") {
  if (std::string(sonare_mastering_insert_param_info("effects.reverb.fdn")) == "[]") {
    SUCCEED("FX processors not built");
    return;
  }
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 16;
  constexpr float kPi = 3.14159265358979323846f;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  for (int i = 0; i < kFrames; ++i) {
    source[static_cast<size_t>(i)] =
        std::sin(2.0f * kPi * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  // A reverb insert starting fully dry: the baseline output is the bare sine.
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"effects.reverb.fdn","params":"{\"dryWet\":0.0,\"decaySec\":2.0}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);

  // Bad arguments are rejected, not silently ignored.
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 0, 0, "dryWet", 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "bogusParam", 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, nullptr, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> dry_out{};
  float* io[] = {dry_out.data()};
  for (int block = 0; block < 8; ++block) {
    dry_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  const double dry_rms = rms(dry_out);

  // Flip the reverb fully wet in realtime; the diffuse tail changes the output
  // audibly without rebuilding the strip.
  REQUIRE(sonare_engine_set_track_strip_insert_param_by_name(engine, 10, 0, "dryWet", 1.0f) ==
          SONARE_OK);
  std::array<float, kBlock> wet_out{};
  io[0] = wet_out.data();
  for (int block = 0; block < 8; ++block) {
    wet_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  const double wet_rms = rms(wet_out);

  REQUIRE(dry_rms > 0.0);
  REQUIRE(std::abs(wet_rms - dry_rms) > 0.05 * dry_rms);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine track strip pan setters reflect in realtime", "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  const char* scene_json =
      R"({"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);

  // Unknown track / out-of-range arguments are rejected, not silently ignored.
  REQUIRE(sonare_engine_set_track_strip_pan(engine, 0, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_pan(engine, 99, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_pan_law(engine, 10, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_pan_mode(engine, 10, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_channel_delay_samples(engine, 10, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // Valid granular updates all succeed on the bound lane strip.
  REQUIRE(sonare_engine_set_track_strip_pan_law(engine, 10, SONARE_PAN_LAW_CONST_6DB) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_pan_mode(engine, 10, SONARE_PAN_MODE_STEREO_PAN) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_pan(engine, 10, -1.0f) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);
  }
  // Hard-left pan: the left channel carries the signal, the right is ~silent.
  REQUIRE(rms(left) > 0.5);
  REQUIRE(rms(right) < rms(left) * 0.05);

  // Dual-pan + channel-delay setters accept valid input on the same strip.
  REQUIRE(sonare_engine_set_track_strip_pan_mode(engine, 10, SONARE_PAN_MODE_DUAL_PAN) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_dual_pan(engine, 10, -1.0f, 1.0f) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_channel_delay_samples(engine, 10, 32) == SONARE_OK);
#else
  REQUIRE(sonare_engine_set_track_strip_pan(engine, 10, -1.0f) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine lane send timing taps pre or post fader", "[c_api][engine]") {
#if defined(SONARE_WITH_MIXING)
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 12;

  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* channels[] = {source.data()};

  // Builds an engine whose lane main path is attenuated -40 dB by the fader and
  // whose send to a bus is at unit gain with the given tap point. A pre-fader
  // send bypasses the -40 dB fader, so the bus (and the master sum) stays loud;
  // a post-fader send is attenuated alongside the main path.
  const auto master_rms = [&](int send_timing) -> double {
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

    SonareEngineClip clip{};
    clip.id = 1;
    clip.track_id = 10;
    clip.channels = channels;
    clip.num_channels = 1;
    clip.num_samples = kFrames;
    clip.length_samples = kFrames;
    clip.gain = 1.0f;
    REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

    SonareEngineBus buses[] = {{1, 0.0f, 1}};
    REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);

    // Establish the lane, attenuate the main path, then re-publish the lane with
    // the send so it lands on the attenuated strip.
    SonareEngineTrackLane bare_lane[] = {{10, nullptr, 0, 0, 1}};
    REQUIRE(sonare_engine_set_track_lanes(engine, bare_lane, 1) == SONARE_OK);
    const char* scene_json =
        R"({"version":1,"strips":[{"id":"track-10","faderDb":-40}],"buses":[],"connections":[]})";
    REQUIRE(sonare_engine_set_track_strip_json(engine, 10, scene_json) == SONARE_OK);

    SonareEngineTrackSend sends[] = {{1, 0.0f, 1, send_timing}};
    SonareEngineTrackLane lane[] = {{10, sends, 1, 0, 1}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

    REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
    std::array<float, kBlock> out{};
    float* io[] = {out.data()};
    for (int block = 0; block < 8; ++block) {
      out.fill(0.0f);
      REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
    }
    const double value = rms(out);
    sonare_engine_destroy(engine);
    return value;
  };

  const double pre_rms = master_rms(SONARE_SEND_TIMING_PRE_FADER);
  const double post_rms = master_rms(SONARE_SEND_TIMING_POST_FADER);
  REQUIRE(pre_rms > 0.1);
  REQUIRE(pre_rms > post_rms * 5.0);
#else
  SUCCEED("mixing feature not built");
#endif
}

TEST_CASE("sonare_engine scope telemetry reports a tone's spectrum and goniometer",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 32;
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kToneHz = 1000.0f;
  constexpr float kSampleRate = 48000.0f;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> tone{};
  for (int i = 0; i < kFrames; ++i) {
    tone[static_cast<size_t>(i)] =
        0.5f * std::sin(2.0f * kPi * kToneHz * static_cast<float>(i) / kSampleRate);
  }
  const float* channels[] = {tone.data(), tone.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = channels;
  clip.num_channels = 2;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  unsigned int applied = 0;
  REQUIRE(sonare_engine_configure_scope_telemetry(engine, kBlock, 32, &applied) == SONARE_OK);
  REQUIRE(applied == 32);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < 12; ++block) {
    REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);
  }

  std::array<SonareScopeTelemetryRecord, 64> records{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_scope_telemetry(engine, records.data(), records.size(), &count) ==
          SONARE_OK);
  REQUIRE(count > 0);

  // Find a master snapshot (target_id 0) and confirm the FFT peak sits in a low
  // band (1 kHz over a 32-band [0, 24 kHz] split -> band 0/1) far above a
  // high-frequency band, and that the goniometer carries scatter points.
  bool checked_master = false;
  for (size_t r = 0; r < count; ++r) {
    const SonareScopeTelemetryRecord& rec = records[r];
    if (rec.target_id != 0) continue;
    REQUIRE(rec.band_count == 32);
    uint32_t peak_band = 0;
    for (uint32_t b = 1; b < rec.band_count; ++b) {
      if (rec.bands[b] > rec.bands[peak_band]) peak_band = b;
    }
    REQUIRE(peak_band <= 2);
    REQUIRE(rec.bands[peak_band] > rec.bands[24] + 20.0f);
    REQUIRE(rec.point_count > 0);
    checked_master = true;
    break;
  }
  REQUIRE(checked_master);

  // Disabling capture (interval 0) drains nothing further once the queue empties.
  REQUIRE(sonare_engine_configure_scope_telemetry(engine, 0, 32, nullptr) == SONARE_OK);
#else
  REQUIRE(sonare_engine_configure_scope_telemetry(engine, kBlock, 32, nullptr) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}
