/// @file sonare_c_engine_strip_test.cpp
/// @brief Engine C ABI track lanes, strips, buses and scope telemetry.

#include "sonare_c_engine_test_helpers.h"

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

namespace {

// Renders the engine to a 5.1 bounce and returns the per-plane energy.
std::array<double, 6> bounce_surround_energy(SonareRealtimeEngine* engine, int frames, int block) {
  // A bounce leaves the transport at its end; rewind so every call renders the clip.
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = frames;
  options.block_size = block;
  options.num_channels = 6;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.normalize_lufs = 0;
  options.dither = 0;
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.num_channels == 6);
  std::array<double, 6> energy{};
  for (int64_t f = 0; f < result.frames; ++f) {
    for (int ch = 0; ch < 6; ++ch) {
      const float v = result.interleaved[f * 6 + ch];
      energy[static_cast<size_t>(ch)] += static_cast<double>(v) * v;
    }
  }
  sonare_free_bounce_result(&result);
  return energy;
}

}  // namespace

TEST_CASE("sonare_engine_set_track_strip_surround_pan moves a lane across the 5.1 bed",
          "[c_api][engine][surround]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 8;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
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
  // A strip with no surroundPan: the setter alone has to place the lane.
  const char* strip_json = R"({"version":1,"buses":[{"id":"master","role":"master"}],)"
                           R"("strips":[{"id":"s"}]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, strip_json) == SONARE_OK);

  // A zero-initialized struct (distance 0) is accepted as the default distance.
  SonareSurroundPan pan{};
  pan.azimuth = -110.0f;
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 10, &pan) == SONARE_OK);
  const std::array<double, 6> left = bounce_surround_energy(engine, kFrames, kBlock);
  REQUIRE(left[4] > 1.0);             // Ls
  REQUIRE(left[5] < left[4] * 1e-3);  // Rs
  REQUIRE(left[2] < left[4] * 1e-3);  // C
  REQUIRE(left[3] == 0.0);            // LFE

  pan.azimuth = 110.0f;
  pan.lfe = 0.5f;
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 10, &pan) == SONARE_OK);
  const std::array<double, 6> right = bounce_surround_energy(engine, kFrames, kBlock);
  // The move glides instead of jumping, so Ls keeps a short tail, and the glide is
  // constant-power: Ls + Rs carry the unit-power DC source for every frame.
  REQUIRE(right[5] > right[4] * 5.0);  // Rs
  REQUIRE(right[4] > 0.0);             // Ls glide tail
  REQUIRE(std::abs(right[4] + right[5] - kFrames) < kFrames * 1e-4);
  REQUIRE(right[3] > 0.0);  // LFE send now on

  // Rejected requests leave the placement alone.
  SonareSurroundPan bad = pan;
  bad.azimuth = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 10, &bad) ==
          SONARE_ERROR_INVALID_PARAMETER);
  bad = pan;
  bad.distance = std::numeric_limits<float>::infinity();
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 10, &bad) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 10, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 99, &pan) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_surround_pan(engine, 0, &pan) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_surround_pan(nullptr, 10, &pan) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const std::array<double, 6> kept = bounce_surround_energy(engine, kFrames, kBlock);
  REQUIRE(kept[5] > 1.0);
  REQUIRE(kept[4] < kept[5] * 1e-3);

  sonare_engine_destroy(engine);
}

namespace {

constexpr int kPreRollBlock = 128;
constexpr int kPreRollFrames = kPreRollBlock * 48;

// Builds a one-lane engine playing a DC clip, parks the lane fader at
// @p fader_db through the smoothed lane parameter, and hands the engine over
// ready to render. The caller destroys it.
SonareRealtimeEngine* make_pre_roll_engine(const float* const* clip_channels, float fader_db) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kPreRollBlock, 64, 16) == SONARE_OK);

  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = kPreRollFrames;
  clip.length_samples = kPreRollFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_parameter_smoothed(
              engine, engine_track_lane_target(engine, 10, "faderDb"), fader_db, -1) == SONARE_OK);
  return engine;
}

}  // namespace

TEST_CASE("an engine offline render opens at the lane's settled fader", "[c_api][engine][bounce]") {
  // A one-shot offline render has to start from the steady state. Without the
  // offline pre-roll the fader command is only drained once the first audible
  // block is already rendering, so the lane's 5 ms smoother glides down from
  // unity and the head of a lane parked at -12 dB comes out up to 12 dB loud.
  // The project bounce path has always pre-rolled; these two engine-level entry
  // points are the same contract.
  constexpr float kFaderDb = -12.0f;
  std::array<float, kPreRollFrames> clip_l{};
  std::array<float, kPreRollFrames> clip_r{};
  clip_l.fill(1.0f);
  clip_r.fill(1.0f);
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = kPreRollFrames;
  options.block_size = kPreRollBlock;
  options.num_channels = 2;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.normalize_lufs = 0;
  options.dither = 0;

  // Reference bounce at unity: the same signal path with no fader offset, so the
  // level assertion below needs no build-dependent absolute value.
  SonareRealtimeEngine* reference_engine = make_pre_roll_engine(clip_channels, 0.0f);
  SonareEngineBounceResult reference{};
  REQUIRE(sonare_engine_bounce_offline(reference_engine, &options, &reference) == SONARE_OK);
  REQUIRE(reference.interleaved != nullptr);
  const float unity = reference.interleaved[(kPreRollFrames - 1) * 2];
  REQUIRE(unity > 0.0f);
  sonare_free_bounce_result(&reference);
  sonare_engine_destroy(reference_engine);

  SonareRealtimeEngine* engine = make_pre_roll_engine(clip_channels, kFaderDb);
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.interleaved != nullptr);
  REQUIRE(result.frames == kPreRollFrames);

  // The fader is static for the whole render, so every sample of the first block
  // already sits at the steady-state level. A ramp-in shows up here as a louder
  // head.
  const float steady = result.interleaved[(kPreRollFrames - 1) * 2];
  float head_peak = 0.0f;
  for (int frame = 0; frame < kPreRollBlock; ++frame) {
    head_peak = std::max(head_peak, std::abs(result.interleaved[frame * 2]));
    head_peak = std::max(head_peak, std::abs(result.interleaved[frame * 2 + 1]));
  }
  REQUIRE(head_peak == Catch::Approx(steady).epsilon(0.01));
  // ... and that steady level is the requested -12 dB, so the assertion above is
  // not satisfied by a fader that never took effect.
  REQUIRE(steady == Catch::Approx(unity * std::pow(10.0f, kFaderDb / 20.0f)).epsilon(0.02));
  sonare_free_bounce_result(&result);
  sonare_engine_destroy(engine);

  // The freeze path captures the same settled lane.
  SonareRealtimeEngine* freeze_engine = make_pre_roll_engine(clip_channels, kFaderDb);
  SonareEngineFreezeOptions freeze{};
  freeze.total_frames = kPreRollFrames;
  freeze.block_size = kPreRollBlock;
  freeze.num_channels = 2;
  freeze.clip_id = 99;
  freeze.start_ppq = 0.0;
  freeze.gain = 1.0f;
  SonareEngineFreezeResult frozen{};
  REQUIRE(sonare_engine_freeze_offline(freeze_engine, &freeze, &frozen) == SONARE_OK);
  REQUIRE(frozen.clip_id == 99u);

  // Drop the lane so the frozen clip (which carries no track id) reaches the
  // master mix directly, and rewind: the freeze render left the playhead at the
  // end of the timeline.
  REQUIRE(sonare_engine_set_track_lanes(freeze_engine, nullptr, 0) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(freeze_engine, 0, -1) == SONARE_OK);
  std::array<float, kPreRollFrames> left{};
  std::array<float, kPreRollFrames> right{};
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_render_offline(freeze_engine, channels, 2, kPreRollFrames, kPreRollBlock) ==
          SONARE_OK);
  const float frozen_steady = left[kPreRollFrames - 1];
  REQUIRE(frozen_steady > 0.0f);
  float frozen_head_peak = 0.0f;
  for (int frame = 0; frame < kPreRollBlock; ++frame) {
    frozen_head_peak = std::max(frozen_head_peak, std::abs(left[static_cast<size_t>(frame)]));
    frozen_head_peak = std::max(frozen_head_peak, std::abs(right[static_cast<size_t>(frame)]));
  }
  REQUIRE(frozen_head_peak == Catch::Approx(frozen_steady).epsilon(0.01));

  sonare_engine_destroy(freeze_engine);
}

namespace {

// Builds a one-lane engine playing @p clip_channels through a tape insert at
// @p bias. The caller destroys it.
SonareRealtimeEngine* make_tape_bias_engine(const float* const* clip_channels, float bias) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kPreRollBlock, 64, 16) == SONARE_OK);

  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = kPreRollFrames;
  clip.length_samples = kPreRollFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  const std::string strip_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",)"
      R"("processor":"saturation.tape","params":"{\"bias\":)" +
      std::to_string(bias) + R"(}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, strip_json.c_str()) == SONARE_OK);
  return engine;
}

}  // namespace

TEST_CASE("an engine bounce matches a raw offline render through a biased tape insert",
          "[c_api][engine][bounce]") {
  // A biased tape core relaxes toward a DC operating point even on silence, so
  // any audio the bounce pre-roll runs through the strip shifts its state.
  std::array<float, kPreRollFrames> clip_l{};
  std::array<float, kPreRollFrames> clip_r{};
  for (int i = 0; i < kPreRollFrames; ++i) {
    clip_l[static_cast<size_t>(i)] =
        0.5f * std::sin(sonare::constants::kTwoPi * 220.0f * i / 48000.0f);
    clip_r[static_cast<size_t>(i)] = clip_l[static_cast<size_t>(i)];
  }
  const float* clip_channels[] = {clip_l.data(), clip_r.data()};

  SonareEngineBounceOptions options{};
  REQUIRE(sonare_engine_bounce_options_default(&options) == SONARE_OK);
  options.total_frames = kPreRollFrames;
  options.block_size = kPreRollBlock;
  options.num_channels = 2;
  options.source_sample_rate = 48000;
  options.target_sample_rate = 48000;
  options.normalize_lufs = 0;
  options.dither = 0;

  for (const float bias : {0.0f, 0.5f}) {
    CAPTURE(bias);
    SonareRealtimeEngine* render_engine = make_tape_bias_engine(clip_channels, bias);
    std::array<float, kPreRollFrames> left{};
    std::array<float, kPreRollFrames> right{};
    float* channels[] = {left.data(), right.data()};
    REQUIRE(sonare_engine_render_offline(render_engine, channels, 2, kPreRollFrames,
                                         kPreRollBlock) == SONARE_OK);
    sonare_engine_destroy(render_engine);

    SonareRealtimeEngine* bounce_engine = make_tape_bias_engine(clip_channels, bias);
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(bounce_engine, &options, &result) == SONARE_OK);
    REQUIRE(result.frames == kPreRollFrames);

    float peak = 0.0f;
    float max_diff = 0.0f;
    for (int frame = 0; frame < kPreRollFrames; ++frame) {
      const auto f = static_cast<size_t>(frame);
      peak = std::max({peak, std::abs(left[f]), std::abs(right[f])});
      max_diff = std::max(max_diff, std::abs(result.interleaved[f * 2] - left[f]));
      max_diff = std::max(max_diff, std::abs(result.interleaved[f * 2 + 1] - right[f]));
    }
    REQUIRE(peak > 0.1f);
    CHECK(max_diff <= peak * 1e-6f);
    sonare_free_bounce_result(&result);
    sonare_engine_destroy(bounce_engine);
  }
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

  REQUIRE(sonare_engine_set_parameter_smoothed(
              engine, engine_track_lane_target(engine, 10, "faderDb"), -12.0f, -1) == SONARE_OK);
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

TEST_CASE("sonare_engine schedules track monitor modes on the prepared monitor bus",
          "[c_api][engine][monitor]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);

  // Public validation is independent of the optional mixing feature.
  REQUIRE(sonare_engine_set_track_monitor_mode(nullptr, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_OFF,
                                               -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_monitor_mode(engine, 0,
                                               static_cast<SonareEngineTrackMonitorMode>(99),
                                               -1) == SONARE_ERROR_INVALID_PARAMETER);

#if !defined(SONARE_WITH_MIXING)
  REQUIRE(sonare_engine_set_track_monitor_mode(engine, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_PFL,
                                               -1) == SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
  return;
#else
  constexpr int kBlock = 64;
  constexpr int kFrames = kBlock * 32;
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);
  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* source_channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 10;
  clip.channels = source_channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, kBlock> main{};
  std::array<float, kBlock> monitor{};
  float* main_channels[] = {main.data()};
  float* monitor_channels[] = {monitor.data()};

  // A transition in the middle of a block splits the render exactly at the
  // requested render frame: main remains unchanged, while only the second
  // half receives the newly enabled PFL cue.
  REQUIRE(sonare_engine_set_track_monitor_mode(engine, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_PFL,
                                               kBlock / 2) == SONARE_OK);
  main.fill(0.0f);
  monitor.fill(99.0f);
  REQUIRE(sonare_engine_process_with_monitor(engine, main_channels, monitor_channels, 1, kBlock) ==
          SONARE_OK);
  REQUIRE(main[0] == Catch::Approx(1.0f));
  REQUIRE(main.back() == Catch::Approx(1.0f));
  REQUIRE(monitor[0] == Catch::Approx(0.0f));
  REQUIRE(monitor[static_cast<size_t>(kBlock / 2)] == Catch::Approx(1.0f));
  REQUIRE(monitor.back() == Catch::Approx(1.0f));

  // Set the lane fader down and let its 5 ms smoother settle. PFL remains at
  // the post-strip/pre-lane-fader level, while AFL follows the lane fader.
  REQUIRE(sonare_engine_set_parameter(engine, engine_track_lane_target(engine, 10, "faderDb"),
                                      -6.0f, -1) == SONARE_OK);
  for (int block = 0; block < 8; ++block) {
    main.fill(0.0f);
    monitor.fill(0.0f);
    REQUIRE(sonare_engine_process_with_monitor(engine, main_channels, monitor_channels, 1,
                                               kBlock) == SONARE_OK);
  }
  main.fill(0.0f);
  monitor.fill(0.0f);
  REQUIRE(sonare_engine_process_with_monitor(engine, main_channels, monitor_channels, 1, kBlock) ==
          SONARE_OK);
  const float settled_main = main.back();
  REQUIRE(settled_main < 0.7f);
  REQUIRE(settled_main > 0.4f);
  REQUIRE(monitor.back() == Catch::Approx(1.0f).margin(0.01f));

  REQUIRE(sonare_engine_set_track_monitor_mode(engine, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_AFL,
                                               -1) == SONARE_OK);
  main.fill(0.0f);
  monitor.fill(0.0f);
  REQUIRE(sonare_engine_process_with_monitor(engine, main_channels, monitor_channels, 1, kBlock) ==
          SONARE_OK);
  REQUIRE(main.back() < 0.7f);
  REQUIRE(main.back() > 0.4f);
  REQUIRE(monitor.back() == Catch::Approx(main.back()).margin(0.01f));

  // A valid transition reports queue back-pressure as OOM, not as an invalid
  // mode. The tiny queue is intentionally left undrained.
  SonareRealtimeEngine* tiny = nullptr;
  REQUIRE(sonare_engine_create(&tiny) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(tiny, 48000.0, kBlock, 2, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_monitor_mode(tiny, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_OFF, -1) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_monitor_mode(tiny, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_PFL, -1) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_monitor_mode(tiny, 0, SONARE_ENGINE_TRACK_MONITOR_MODE_AFL, -1) ==
          SONARE_ERROR_OUT_OF_MEMORY);
  sonare_engine_destroy(tiny);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("sonare_engine track send zero-init defaults to post-fader", "[c_api][engine]") {
  // Post-fader is the zero value of SonareSendTiming, so a zero-initialized
  // SonareEngineTrackSend taps post-fader (the historical lane-send behavior)
  // instead of silently flipping to pre-fader. The integer enum is never
  // serialized (scene/project JSON uses the strings "pre"/"post"), so this
  // ordering is wire-safe.
  STATIC_REQUIRE(SONARE_SEND_TIMING_POST_FADER == 0);
  STATIC_REQUIRE(SONARE_SEND_TIMING_PRE_FADER == 1);
  SonareEngineTrackSend zero_init{};
  REQUIRE(zero_init.send_timing == SONARE_SEND_TIMING_POST_FADER);
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
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  SonareEngineBus duplicate_buses[] = {{1, 0.0f, 1, 0, nullptr, 0}, {1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, duplicate_buses, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A surround bus layout (5.1 = 2) is accepted; an out-of-range layout value
  // is rejected.
  SonareEngineBus surround_bus[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_5_1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, surround_bus, 1) == SONARE_OK);
  SonareEngineBus bad_layout_bus[] = {{1, 0.0f, 99, 0, nullptr, 0}};
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
  // Direct lane x plus the 0 dB send into the 5.1 bus's front-left plane, which
  // folds to the mono output at 0.5x (stereo fold, then average): 1.5x.
  REQUIRE(out.back() > 2.11f);
  REQUIRE(out.back() < 2.13f);
  // max_records == 0 is documented as a safe no-op, NOT a way to probe the
  // pending backlog without draining: it must report 0 here even though the
  // real drain immediately below finds records.
  size_t probed_meter_count = 123;
  REQUIRE(sonare_engine_drain_meter_telemetry(engine, nullptr, 0, &probed_meter_count) ==
          SONARE_OK);
  REQUIRE(probed_meter_count == 0);

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
  // The -6 dB send halves the folded 0.5x contribution: x + 0.25x.
  REQUIRE(out.back() > 1.76f);
  REQUIRE(out.back() < 1.78f);

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
  // A -6 dB bus gain halves the folded contribution the same way: x + 0.25x.
  REQUIRE(out.back() > 1.76f);
  REQUIRE(out.back() < 1.78f);

  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, "{bad json") == SONARE_ERROR_INVALID_FORMAT);
  const char* bus_strip_json =
      R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":12,\"band0.enabled\":1}"}]}],"connections":[]})";
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_strip_json) == SONARE_OK);
#else
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine resolves and sets bus/master insert automation ids", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 64) == SONARE_OK);

  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  const char* bus_strip_json =
      R"({"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":0,\"band0.enabled\":1}"}]}],"connections":[]})";
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_strip_json) == SONARE_OK);

  // Resolve a bus insert parameter to its reserved automation id (top 3 bits 111).
  uint32_t bus_id = 0;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "band0.gainDb", &bus_id) ==
          SONARE_OK);
  REQUIRE((bus_id & 0xE0000000u) == 0xE0000000u);
  // The reserved id drives an automation lane and a one-off parameter set.
  SonareAutomationPoint points[] = {{0.0, 6.0f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, bus_id, points, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_parameter(engine, bus_id, 3.0f, -1) == SONARE_OK);
  // And the by-name manual set reaches the same target.
  REQUIRE(sonare_engine_set_bus_strip_insert_param_by_name(engine, 1, 0, "band0.gainDb", -3.0f) ==
          SONARE_OK);

  // An unknown bus / insert / name is rejected, and out_id is defined rather
  // than left holding whatever the caller's local happened to contain.
  uint32_t unresolved = 0xDEADBEEFu;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(
              engine, 9, 0, "band0.gainDb", &unresolved) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(unresolved == 0);
  unresolved = 0xDEADBEEFu;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "nope", &unresolved) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(unresolved == 0);
  REQUIRE(sonare_engine_set_bus_strip_insert_param_by_name(engine, 9, 0, "band0.gainDb", 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // Argument guards.
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(nullptr, 1, 0, "band0.gainDb", &bus_id) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 0, 0, "band0.gainDb", &bus_id) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "band0.gainDb", nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // Master strip insert resolution mirrors the bus path.
  const char* master_json =
      R"({"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"dynamics.compressor","params":"{\"thresholdDb\":-3,\"ratio\":4}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, master_json) == SONARE_OK);
  uint32_t master_id = 0;
  REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 0, "thresholdDb", &master_id) ==
          SONARE_OK);
  REQUIRE((master_id & 0xE0000000u) == 0xE0000000u);
  REQUIRE(master_id != bus_id);

  sonare_engine_destroy(engine);
}
#endif

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine applies, restores and clears strip insert parameters by name",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 64) == SONARE_OK);

  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  const char* eq_insert =
      R"("inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":0,\"band0.enabled\":1}"}])";
  const std::string track_json = std::string(R"({"version":1,"strips":[{"id":"track-10",)") +
                                 eq_insert + R"(}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, track_json.c_str()) == SONARE_OK);
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  const std::string bus_json = std::string(R"({"version":1,"strips":[],"buses":[{"id":"1",)") +
                               eq_insert + R"(}],"connections":[]})";
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, bus_json.c_str()) == SONARE_OK);
  const std::string master_json = std::string(R"({"version":1,"strips":[{"id":"master",)") +
                                  eq_insert + R"(}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, master_json.c_str()) == SONARE_OK);

  // Apply-now reports an unknown target through out_applied, not an error.
  int applied = -1;
  REQUIRE(sonare_engine_apply_track_strip_insert_param_by_name_now(engine, 10, 0, "band0.gainDb",
                                                                   6.0f, &applied) == SONARE_OK);
  REQUIRE(applied == 1);
  applied = -1;
  REQUIRE(sonare_engine_apply_track_strip_insert_param_by_name_now(engine, 10, 0, "nope", 6.0f,
                                                                   &applied) == SONARE_OK);
  REQUIRE(applied == 0);
  REQUIRE(sonare_engine_apply_track_strip_insert_param_by_name_now(
              engine, 10, 0, "band0.gainDb", 6.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // A refused call still defines out_applied rather than leaving the caller's value.
  applied = -1;
  REQUIRE(sonare_engine_apply_track_strip_insert_param_by_name_now(
              engine, 0, 0, "band0.gainDb", 6.0f, &applied) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(applied == 0);
  applied = -1;
  REQUIRE(sonare_engine_apply_master_strip_insert_param_by_name_now(engine, 0, "band0.gainDb", 3.0f,
                                                                    &applied) == SONARE_OK);
  REQUIRE(applied == 1);
  applied = -1;
  REQUIRE(sonare_engine_apply_bus_strip_insert_param_by_name_now(engine, 1, 0, "band0.gainDb", 3.0f,
                                                                 &applied) == SONARE_OK);
  REQUIRE(applied == 1);
  applied = -1;
  REQUIRE(sonare_engine_apply_bus_strip_insert_param_by_name_now(engine, 9, 0, "band0.gainDb", 3.0f,
                                                                 &applied) == SONARE_OK);
  REQUIRE(applied == 0);

  // Restore refuses an unknown target outright.
  REQUIRE(sonare_engine_restore_track_strip_insert_param_by_name(engine, 10, 0, "band0.gainDb",
                                                                 -2.0f) == SONARE_OK);
  REQUIRE(sonare_engine_restore_track_strip_insert_param_by_name(
              engine, 11, 0, "band0.gainDb", -2.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_restore_master_strip_insert_param_by_name(engine, 0, "band0.gainDb",
                                                                  -2.0f) == SONARE_OK);
  REQUIRE(sonare_engine_restore_master_strip_insert_param_by_name(engine, 0, "nope", -2.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_restore_bus_strip_insert_param_by_name(engine, 1, 0, "band0.gainDb",
                                                               -2.0f) == SONARE_OK);
  REQUIRE(sonare_engine_restore_bus_strip_insert_param_by_name(
              engine, 9, 0, "band0.gainDb", -2.0f) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_clear_track_insert_parameter_bases(engine, 10) == SONARE_OK);
  REQUIRE(sonare_engine_clear_track_insert_parameter_bases(engine, 11) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_clear_master_insert_parameter_bases(engine) == SONARE_OK);
  REQUIRE(sonare_engine_clear_bus_insert_parameter_bases(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_clear_bus_insert_parameter_bases(engine, 9) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_clear_bus_insert_parameter_bases(engine, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_settle_insert_parameters(engine) == SONARE_OK);
  REQUIRE(sonare_engine_apply_commands_due_now_preserving_future(engine) == SONARE_OK);
  REQUIRE(sonare_engine_settle_insert_parameters(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_apply_commands_due_now_preserving_future(nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_automation_lane with no points removes the lane", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 64) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  const char* track_json =
      R"({"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\"band0.type\":1,\"band0.frequencyHz\":1000,\"band0.gainDb\":0,\"band0.enabled\":1}"}]}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, track_json) == SONARE_OK);
  uint32_t id = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, "band0.gainDb", &id) ==
          SONARE_OK);

  const SonareAutomationPoint points[] = {{0.0, 6.0f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, id, points, 1) == SONARE_OK);
  size_t lane_count = 0;
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 1);
  REQUIRE(sonare_engine_set_automation_lane(engine, id, nullptr, 0) == SONARE_OK);
  REQUIRE(sonare_engine_automation_lane_count(engine, &lane_count) == SONARE_OK);
  REQUIRE(lane_count == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("a track fader id held across a lane reorder keeps driving its own track",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 80;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);

  // Only track 20 sounds, so the output reads track 20's fader alone.
  std::vector<float> source(kFrames, 1.0f);
  const float* source_channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 20;
  clip.channels = source_channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  uint32_t track10_fader = 0;
  REQUIRE(sonare_engine_resolve_track_lane_automation_id(engine, 10, "faderDb", &track10_fader) ==
          SONARE_OK);

  SonareEngineTrackLane reordered[] = {{20, nullptr, 0, 0, 1}, {10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, reordered, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_parameter(engine, track10_fader, -60.0f, -1) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  const auto render = [&](int blocks) {
    for (int block = 0; block < blocks; ++block) {
      out.fill(0.0f);
      REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
    }
  };
  render(30);
  REQUIRE(out.back() == Catch::Approx(1.0f).margin(1.0e-3));

  // Once track 10 is removed its id addresses nothing, not the track now at its old position.
  SonareEngineTrackLane only20[] = {{20, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, only20, 1) == SONARE_OK);
  SonareParameterInfo info{};
  REQUIRE(sonare_engine_parameter_info(engine, track10_fader, &info) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_parameter(engine, track10_fader, -60.0f, -1) == SONARE_OK);
  render(30);
  REQUIRE(out.back() == Catch::Approx(1.0f).margin(1.0e-3));

  sonare_engine_destroy(engine);
}

TEST_CASE("a track insert lane set before a lane reorder survives a later automation-lane edit",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock * 40;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 16) == SONARE_OK);

  // Only track 20 sounds; track 10 is silent, so the output reads track 20's gain alone.
  std::array<float, kFrames> source{};
  source.fill(1.0f);
  const float* source_channels[] = {source.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.track_id = 20;
  clip.channels = source_channels;
  clip.num_channels = 1;
  clip.num_samples = kFrames;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  const char* gain_insert =
      R"("inserts":[{"slot":"pre","processor":"utility.gain","params":"{\"levelDb\":0}"}])";
  for (const char* track : {"10", "20"}) {
    const std::string json = std::string(R"({"version":1,"strips":[{"id":"track-)") + track +
                             R"(",)" + gain_insert + R"(}],"buses":[],"connections":[]})";
    REQUIRE(sonare_engine_set_track_strip_json(engine, static_cast<uint32_t>(std::stoul(track)),
                                               json.c_str()) == SONARE_OK);
  }
  const std::string master_json = std::string(R"({"version":1,"strips":[{"id":"master",)") +
                                  gain_insert + R"(}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, master_json.c_str()) == SONARE_OK);

  uint32_t track10_gain = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, "levelDb",
                                                           &track10_gain) == SONARE_OK);
  const SonareAutomationPoint duck[] = {{0.0, -60.0f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, track10_gain, duck, 1) == SONARE_OK);

  // Reorder, then edit an unrelated lane: the track 10 lane must still name track 10.
  SonareEngineTrackLane reordered[] = {{20, nullptr, 0, 0, 1}, {10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, reordered, 2) == SONARE_OK);
  uint32_t master_gain = 0;
  REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 0, "levelDb", &master_gain) ==
          SONARE_OK);
  const SonareAutomationPoint unity[] = {{0.0, 0.0f, 0}};
  REQUIRE(sonare_engine_set_automation_lane(engine, master_gain, unity, 1) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> out{};
  float* io[] = {out.data()};
  for (int block = 0; block < 30; ++block) {
    out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(out.back() == Catch::Approx(1.0f).margin(1.0e-3));

  sonare_engine_destroy(engine);
}

namespace {

// A one-strip scene whose inserts are @p processors, pre-fader up to 32 and post-fader after,
// each constructed from the escaped JSON object @p params.
std::string insert_scene(const char* strip_key, const char* strip_id,
                         const std::vector<const char*>& processors, const char* params = "{}") {
  std::string inserts;
  for (size_t i = 0; i < processors.size(); ++i) {
    if (i > 0) inserts += ",";
    inserts += std::string(R"({"slot":")") + (i < 32 ? "pre" : "post") + R"(","processor":")" +
               processors[i] + R"(","params":")" + params + R"("})";
  }
  if (std::string(strip_key) == "buses") {
    return std::string(R"({"version":1,"strips":[],"buses":[{"id":")") + strip_id +
           R"(","inserts":[)" + inserts + R"(]}],"connections":[]})";
  }
  return std::string(R"({"version":1,"strips":[{"id":")") + strip_id + R"(","inserts":[)" +
         inserts + R"(]}],"buses":[],"connections":[]})";
}

uint32_t insert_param_strip_of(uint32_t id) { return (id >> 16u) & 0x1FFFu; }

bool id_alive(SonareRealtimeEngine* engine, uint32_t id) {
  SonareParameterInfo info{};
  return sonare_engine_parameter_info(engine, id, &info) == SONARE_OK;
}

}  // namespace

TEST_CASE("track insert ids keep their track across lane reorders and removals",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 16) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 10, insert_scene("strips", "track-10", {"utility.gain"}).c_str()) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 20, insert_scene("strips", "track-20", {"utility.gain"}).c_str()) ==
          SONARE_OK);
  uint32_t id10 = 0;
  uint32_t id20 = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, "levelDb", &id10) ==
          SONARE_OK);
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 20, 0, "levelDb", &id20) ==
          SONARE_OK);
  REQUIRE(id10 != id20);

  // A reorder leaves both ids as they were.
  SonareEngineTrackLane reordered[] = {{20, nullptr, 0, 0, 1}, {10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, reordered, 2) == SONARE_OK);
  uint32_t again = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, "levelDb", &again) ==
          SONARE_OK);
  REQUIRE(again == id10);

  // Removing another track leaves the id alive; removing its own track retires it for good.
  SonareEngineTrackLane only10[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, only10, 1) == SONARE_OK);
  REQUIRE(id_alive(engine, id10));
  REQUIRE_FALSE(id_alive(engine, id20));
  REQUIRE(sonare_engine_set_track_lanes(engine, reordered, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 20, insert_scene("strips", "track-20", {"utility.gain"}).c_str()) ==
          SONARE_OK);
  REQUIRE_FALSE(id_alive(engine, id20));
  uint32_t readded = 0;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 20, 0, "levelDb", &readded) ==
          SONARE_OK);
  REQUIRE(readded != id20);

  sonare_engine_destroy(engine);
}

TEST_CASE("insert ids fail once their slot holds another processor type", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 16) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 1) == SONARE_OK);
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);

  const auto set_all = [&](const char* processor) {
    REQUIRE(sonare_engine_set_track_strip_json(
                engine, 10, insert_scene("strips", "track-10", {processor}).c_str()) == SONARE_OK);
    REQUIRE(sonare_engine_set_bus_strip_json(
                engine, 1, insert_scene("buses", "1", {processor}).c_str()) == SONARE_OK);
    REQUIRE(sonare_engine_set_master_strip_json(
                engine, insert_scene("strips", "master", {processor}).c_str()) == SONARE_OK);
  };
  const auto resolve_all = [&](const char* key) {
    std::array<uint32_t, 3> ids{};
    REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 10, 0, key, &ids[0]) ==
            SONARE_OK);
    REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, key, &ids[1]) ==
            SONARE_OK);
    REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 0, key, &ids[2]) ==
            SONARE_OK);
    return ids;
  };

  set_all("utility.gain");
  const std::array<uint32_t, 3> gain_ids = resolve_all("levelDb");
  // A rebuild with the same processor type keeps every id.
  set_all("utility.gain");
  REQUIRE(resolve_all("levelDb") == gain_ids);

  set_all("dynamics.compressor");
  for (const uint32_t id : gain_ids) REQUIRE_FALSE(id_alive(engine, id));
  const std::array<uint32_t, 3> compressor_ids = resolve_all("thresholdDb");
  for (size_t i = 0; i < compressor_ids.size(); ++i) {
    REQUIRE(id_alive(engine, compressor_ids[i]));
    REQUIRE(compressor_ids[i] != gain_ids[i]);
  }
  // Returning to the first type does not revive the retired ids.
  set_all("utility.gain");
  for (const uint32_t id : gain_ids) REQUIRE_FALSE(id_alive(engine, id));

  sonare_engine_destroy(engine);
}

TEST_CASE("bus insert ids survive bus reorders and fail once the bus is removed",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 16) == SONARE_OK);
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}, {2, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
  for (const char* bus : {"1", "2"}) {
    REQUIRE(sonare_engine_set_bus_strip_json(
                engine, static_cast<uint32_t>(std::stoul(bus)),
                insert_scene("buses", bus, {"utility.gain"}).c_str()) == SONARE_OK);
  }
  uint32_t id1 = 0;
  uint32_t id2 = 0;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "levelDb", &id1) ==
          SONARE_OK);
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 2, 0, "levelDb", &id2) ==
          SONARE_OK);

  SonareEngineBus reordered[] = {{2, 0.0f, 1, 0, nullptr, 0}, {1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, reordered, 2) == SONARE_OK);
  uint32_t again = 0;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "levelDb", &again) ==
          SONARE_OK);
  REQUIRE(again == id1);
  REQUIRE(id_alive(engine, id2));

  REQUIRE(sonare_engine_set_track_buses(engine, reordered, 1) == SONARE_OK);
  REQUIRE(id_alive(engine, id2));
  REQUIRE_FALSE(id_alive(engine, id1));
  REQUIRE(sonare_engine_set_track_buses(engine, reordered, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(
              engine, 1, insert_scene("buses", "1", {"utility.gain"}).c_str()) == SONARE_OK);
  REQUIRE_FALSE(id_alive(engine, id1));
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 1, 0, "levelDb", &again) ==
          SONARE_OK);
  REQUIRE(again != id1);

  sonare_engine_destroy(engine);
}

TEST_CASE("a GS EFX type change retires the ids of its slot", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 16) == SONARE_OK);
  // Both types are one processor class; only the EFX type tells their byte layouts apart.
  const std::string first =
      insert_scene("strips", "master", {"effects.gsEfx"}, R"({\"typeMsb\":1,\"typeLsb\":16})");
  const std::string second =
      insert_scene("strips", "master", {"effects.gsEfx"}, R"({\"typeMsb\":2,\"typeLsb\":12})");
  const auto resolve_any = [&]() {
    for (int slot = 0; slot < 20; ++slot) {
      uint32_t id = 0;
      const std::string key = "byte" + std::to_string(slot);
      if (sonare_engine_resolve_master_insert_automation_id(engine, 0, key.c_str(), &id) ==
          SONARE_OK) {
        return id;
      }
    }
    return uint32_t{0};
  };

  REQUIRE(sonare_engine_set_master_strip_json(engine, first.c_str()) == SONARE_OK);
  const uint32_t first_id = resolve_any();
  REQUIRE(first_id != 0);
  REQUIRE(sonare_engine_set_master_strip_json(engine, second.c_str()) == SONARE_OK);
  REQUIRE_FALSE(id_alive(engine, first_id));
  const uint32_t second_id = resolve_any();
  REQUIRE(second_id != 0);
  REQUIRE(insert_param_strip_of(second_id) != insert_param_strip_of(first_id));

  sonare_engine_destroy(engine);
}

TEST_CASE("track strip, lane and bus changes that would overflow the insert id table are refused",
          "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 64, 16) == SONARE_OK);

  // 127 master rebuilds of 64 slots alternating the processor kind leave 64 of 8192 entries.
  const std::string gain =
      insert_scene("strips", "master", std::vector<const char*>(64, "utility.gain"));
  const std::string tilt =
      insert_scene("strips", "master", std::vector<const char*>(64, "eq.tilt"));
  for (int round = 0; round < 127; ++round) {
    REQUIRE(sonare_engine_set_master_strip_json(engine, (round % 2 == 0 ? gain : tilt).c_str()) ==
            SONARE_OK);
  }
  SonareEngineTrackLane lane10[] = {{10, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane10, 1) == SONARE_OK);
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  const auto gains = [](size_t count) { return std::vector<const char*>(count, "utility.gain"); };
  REQUIRE(
      sonare_engine_set_bus_strip_json(engine, 1, insert_scene("buses", "1", gains(40)).c_str()) ==
      SONARE_OK);  // 24 entries left

  // Track strip: 30 new slots do not fit, 20 do.
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 10, insert_scene("strips", "track-10", gains(30)).c_str()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 10, insert_scene("strips", "track-10", gains(20)).c_str()) == SONARE_OK);
  // 4 left. A rebuild that keeps every slot's kind needs no entry, on a track and on a bus.
  const char* quieter = R"({\"levelDb\":-1})";
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 10, insert_scene("strips", "track-10", gains(20), quieter).c_str()) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(
              engine, 1, insert_scene("buses", "1", gains(40), quieter).c_str()) == SONARE_OK);
  // Bus: a new kind in 40 slots does not fit.
  REQUIRE(
      sonare_engine_set_bus_strip_json(
          engine, 1, insert_scene("buses", "1", std::vector<const char*>(40, "eq.tilt")).c_str()) ==
      SONARE_ERROR_INVALID_PARAMETER);
  // Lane: a track entering with 10 slots does not fit.
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, 20, insert_scene("strips", "track-20", gains(10)).c_str()) == SONARE_OK);
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("a strip change that would overflow the insert id table is refused", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 64, 16) == SONARE_OK);

  // Every rebuild of 64 slots (a strip's limit) with the other processor type
  // mints 64 entries; the 13-bit selector field holds 8192, so rebuild 129 cannot fit.
  constexpr size_t kInserts = 64;
  const std::string gain =
      insert_scene("strips", "master", std::vector<const char*>(kInserts, "utility.gain"));
  const std::string tilt =
      insert_scene("strips", "master", std::vector<const char*>(kInserts, "eq.tilt"));
  for (int round = 0; round < 128; ++round) {
    REQUIRE(sonare_engine_set_master_strip_json(engine, (round % 2 == 0 ? gain : tilt).c_str()) ==
            SONARE_OK);
  }
  uint32_t before = 0;
  REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 0, "tiltDb", &before) ==
          SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_json(engine, gain.c_str()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // The refused change left the strip and its ids untouched; an unchanged chain still applies.
  REQUIRE(id_alive(engine, before));
  REQUIRE(sonare_engine_set_master_strip_json(engine, tilt.c_str()) == SONARE_OK);

  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("insert automation resolvers define out_id on every exit path",
          "[c_api][engine][mixing]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // Nothing is bound to these targets, so a mixing-enabled build takes the
  // id < 0 path and a mixing-off build takes the NOT_SUPPORTED branch. The
  // caller's sentinel must be gone either way.
#if defined(SONARE_WITH_MIXING)
  const SonareError expected = SONARE_ERROR_INVALID_PARAMETER;
#else
  const SonareError expected = SONARE_ERROR_NOT_SUPPORTED;
#endif

  uint32_t track_id = 0xDEADBEEFu;
  REQUIRE(sonare_engine_resolve_track_insert_automation_id(engine, 7, 0, "cutoff", &track_id) ==
          expected);
  REQUIRE(track_id == 0);

  uint32_t master_id = 0xDEADBEEFu;
  REQUIRE(sonare_engine_resolve_master_insert_automation_id(engine, 0, "cutoff", &master_id) ==
          expected);
  REQUIRE(master_id == 0);

  uint32_t bus_id = 0xDEADBEEFu;
  REQUIRE(sonare_engine_resolve_bus_insert_automation_id(engine, 7, 0, "cutoff", &bus_id) ==
          expected);
  REQUIRE(bus_id == 0);

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

#if defined(SONARE_WITH_MIXING)
  SonareEngineTrackLane lanes[] = {{10, nullptr, 0, 0, 1}, {20, nullptr, 0, 0, 1}};
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

TEST_CASE("sonare_engine_set_bus_strip_eq_band_json updates embedded bus EQ", "[c_api][engine]") {
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
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  SonareEngineTrackSend send[] = {{1, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane lane[] = {{10, send, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(
              engine, 0, 0, R"({"type":"Peak","enabled":true})") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(engine, 99, 0,
                                                   R"({"type":"Peak","enabled":true})") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(engine, 1, -1,
                                                   R"({"type":"Peak","enabled":true})") ==
          SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> flat_out{};
  float* io[] = {flat_out.data()};
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  REQUIRE(
      sonare_engine_set_bus_strip_eq_band_json(
          engine, 1, 0, R"({"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true})") ==
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
  REQUIRE(sonare_engine_set_bus_strip_eq_band_json(engine, 1, 0, "{}") ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE(
    "sonare_engine_set_master_strip_json keeps EQ across an identical resend and drops it "
    "without eq",
    "[c_api][engine]") {
  // Scene EQ is authoritative: a resent strip JSON carries whatever "eq" it
  // names, in place of whatever the engine held before -- an identical
  // resend keeps the filter (and its running state) exactly, a resend
  // missing "eq" flattens.
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
  const char* equalized_json =
      R"({"version":1,"strips":[{"id":"master","eq":{"bands":)"
      R"([{"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true}]}}],)"
      R"("buses":[],"connections":[]})";
  const char* flat_json = R"({"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_master_strip_json(engine, equalized_json) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> eq_out{};
  float* io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  const double eq_level = rms(eq_out);

  // An identical resend is an in-place update: the filter keeps running
  // rather than resetting, so the very next block matches an engine that
  // never resent at all.
  REQUIRE(sonare_engine_set_master_strip_json(engine, equalized_json) == SONARE_OK);
  std::array<float, kBlock> resent_out{};
  io[0] = resent_out.data();
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);

  SonareRealtimeEngine* reference = nullptr;
  REQUIRE(sonare_engine_create(&reference) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(reference, 48000.0, kBlock, 64, 64) == SONARE_OK);
  REQUIRE(sonare_engine_set_clips(reference, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_master_strip_json(reference, equalized_json) == SONARE_OK);
  REQUIRE(sonare_engine_play(reference, -1) == SONARE_OK);
  std::array<float, kBlock> reference_out{};
  float* ref_io[] = {reference_out.data()};
  for (int block = 0; block < 7; ++block) {
    reference_out.fill(0.0f);
    REQUIRE(sonare_engine_process(reference, ref_io, 1, kBlock) == SONARE_OK);
  }
  for (int i = 0; i < kBlock; ++i) {
    REQUIRE(resent_out[static_cast<size_t>(i)] ==
            Catch::Approx(reference_out[static_cast<size_t>(i)]).margin(1e-6f));
  }
  sonare_engine_destroy(reference);

  // A resend without "eq" drops the filter.
  REQUIRE(sonare_engine_set_master_strip_json(engine, flat_json) == SONARE_OK);
  std::array<float, kBlock> flat_after{};
  io[0] = flat_after.data();
  for (int block = 0; block < 6; ++block) {
    flat_after.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(rms(flat_after) < eq_level * 0.7);
#else
  REQUIRE(sonare_engine_set_master_strip_json(engine, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE(
    "sonare_engine_set_bus_strip_json keeps EQ across an identical resend and drops it without "
    "eq",
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
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  SonareEngineTrackSend send[] = {{1, 0.0f, 1, SONARE_SEND_TIMING_POST_FADER}};
  SonareEngineTrackLane lane[] = {{10, send, 1, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

  const char* equalized_json =
      R"({"version":1,"strips":[],"buses":[{"id":"1","eq":{"bands":)"
      R"([{"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true}]}}],)"
      R"("connections":[]})";
  const char* flat_json = R"({"version":1,"strips":[],"buses":[{"id":"1"}],"connections":[]})";
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, equalized_json) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> eq_out{};
  float* io[] = {eq_out.data()};
  for (int block = 0; block < 6; ++block) {
    eq_out.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  const double eq_level = rms(eq_out);

  // An identical resend is an in-place update on the bus too.
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, equalized_json) == SONARE_OK);
  std::array<float, kBlock> resent_out{};
  io[0] = resent_out.data();
  REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);

  SonareRealtimeEngine* reference = nullptr;
  REQUIRE(sonare_engine_create(&reference) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(reference, 48000.0, kBlock, 64, 64) == SONARE_OK);
  REQUIRE(sonare_engine_set_clips(reference, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_buses(reference, buses, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_lanes(reference, lane, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_json(reference, 1, equalized_json) == SONARE_OK);
  REQUIRE(sonare_engine_play(reference, -1) == SONARE_OK);
  std::array<float, kBlock> reference_out{};
  float* ref_io[] = {reference_out.data()};
  for (int block = 0; block < 7; ++block) {
    reference_out.fill(0.0f);
    REQUIRE(sonare_engine_process(reference, ref_io, 1, kBlock) == SONARE_OK);
  }
  for (int i = 0; i < kBlock; ++i) {
    REQUIRE(resent_out[static_cast<size_t>(i)] ==
            Catch::Approx(reference_out[static_cast<size_t>(i)]).margin(1e-6f));
  }
  sonare_engine_destroy(reference);

  // A resend without "eq" drops the filter on the bus.
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, flat_json) == SONARE_OK);
  std::array<float, kBlock> flat_after{};
  io[0] = flat_after.data();
  for (int block = 0; block < 6; ++block) {
    flat_after.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 1, kBlock) == SONARE_OK);
  }
  REQUIRE(rms(flat_after) < eq_level * 0.7);
#else
  REQUIRE(sonare_engine_set_bus_strip_json(engine, 1, "{}") == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("sonare_mastering_insert_param_info reports realtime param descriptors",
          "[c_api][mastering]") {
  // Unknown processor / null name yield an empty JSON array.
  REQUIRE(std::string(sonare_mastering_insert_param_info("nope.nope")) == "[]");
  REQUIRE(std::string(sonare_mastering_insert_param_info(nullptr)) == "[]");

  const std::string fdn = sonare_mastering_insert_param_info("effects.reverb.fdn");
  if (fdn == "[]") {
    SKIP("FX processors not built; realtime reverb descriptors unavailable");
  }
  // dryWet is realtime-safe and exposed with its integer id.
  REQUIRE(fdn.find("\"name\":\"dryWet\"") != std::string::npos);
  REQUIRE(fdn.find("\"rtSafe\":true") != std::string::npos);

  // Dattorro publishes a non-realtime-safe parameter (modDepthSamples, id 4,
  // which grows allpass buffers); the descriptor must flag it accordingly.
  const std::string dat = sonare_mastering_insert_param_info("effects.reverb.dattorro");
  REQUIRE(dat.find("\"name\":\"modDepthSamples\"") != std::string::npos);
  REQUIRE(dat.find("\"unit\":\"samples\"") != std::string::npos);
  REQUIRE(dat.find("\"rtSafe\":false") != std::string::npos);
}
#endif  // defined(SONARE_WITH_MASTERING)

TEST_CASE("sonare_engine_set_track_strip_insert_param_by_name changes reverb mix in realtime",
          "[c_api][engine]") {
#if defined(SONARE_WITH_MASTERING)
  if (std::string(sonare_mastering_insert_param_info("effects.reverb.fdn")) == "[]") {
    SKIP("FX processors not built");
  }
#else
  SKIP("mastering support not built");
#endif
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
  REQUIRE(sonare_engine_set_track_strip_channel_delay_samples(
              engine, 10, sonare::mixing::kMaxAlignmentDelaySamples + 1) ==
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

TEST_CASE("sonare_engine bus strip pan setters reflect in realtime", "[c_api][engine]") {
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
  SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  // output_bus_id routes the lane's whole output to the bus (rather than the
  // master directly), so the bus panner is the only thing shaping the signal
  // the assertions below observe.
  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 1, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);

  // Null engine / unknown bus / out-of-range enum / non-finite values are
  // rejected, not silently ignored.
  REQUIRE(sonare_engine_set_bus_strip_pan(nullptr, 1, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 0, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 99, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 1, std::numeric_limits<float>::quiet_NaN()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_law(engine, 99, SONARE_PAN_LAW_CONST_6DB) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_law(engine, 1, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_mode(engine, 99, SONARE_PAN_MODE_STEREO_PAN) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_mode(engine, 1, 99) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_dual_pan(engine, 99, -1.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_dual_pan(engine, 1, std::numeric_limits<float>::infinity(),
                                               1.0f) == SONARE_ERROR_INVALID_PARAMETER);

  // Valid granular updates all succeed on the known bus.
  REQUIRE(sonare_engine_set_bus_strip_pan_law(engine, 1, SONARE_PAN_LAW_CONST_6DB) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_pan_mode(engine, 1, SONARE_PAN_MODE_STEREO_PAN) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 1, -1.0f) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < 4; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);
  }
  // Hard-left pan on the bus: the left channel carries the signal, the right
  // is ~silent.
  REQUIRE(rms(left) > 0.5);
  REQUIRE(rms(right) < rms(left) * 0.05);

  // Dual-pan setter accepts valid input on the same bus.
  REQUIRE(sonare_engine_set_bus_strip_pan_mode(engine, 1, SONARE_PAN_MODE_DUAL_PAN) == SONARE_OK);
  REQUIRE(sonare_engine_set_bus_strip_dual_pan(engine, 1, -1.0f, 1.0f) == SONARE_OK);
#else
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 1, -1.0f) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine bus strip pan setters refuse a surround bus",
          "[c_api][engine][surround]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 256, 64, 64) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  SonareEngineBus surround_bus[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_5_1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, surround_bus, 1) == SONARE_OK);

  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 1, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_law(engine, 1, SONARE_PAN_LAW_CONST_6DB) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_pan_mode(engine, 1, SONARE_PAN_MODE_STEREO_PAN) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_bus_strip_dual_pan(engine, 1, -1.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // EQ is not restricted to stereo buses -- only the pan family is.
  REQUIRE(
      sonare_engine_set_bus_strip_eq_band_json(
          engine, 1, 0, R"({"type":"Peak","frequencyHz":1000,"gainDb":6,"q":1,"enabled":true})") ==
      SONARE_OK);
#else
  REQUIRE(sonare_engine_set_bus_strip_pan(engine, 1, -1.0f) == SONARE_ERROR_NOT_SUPPORTED);
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

    SonareEngineBus buses[] = {{1, 0.0f, 1, 0, nullptr, 0}};
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
  SKIP("mixing feature not built");
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

  // max_records == 0 is documented as a safe no-op, NOT a way to learn the
  // pending backlog: it must report 0 and drain nothing even though records
  // are genuinely pending, as the very next (real) drain below proves.
  size_t probed_count = 123;
  REQUIRE(sonare_engine_drain_scope_telemetry(engine, nullptr, 0, &probed_count) == SONARE_OK);
  REQUIRE(probed_count == 0);

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

TEST_CASE("sonare_engine scope telemetry applies a pre-prepare band configuration",
          "[c_api][engine]") {
  constexpr int kBlock = 256;
  constexpr unsigned int kBands = 16;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  // Configuration before prepare() must be retained for the later tap
  // allocation, and the return value must describe that requested resolution
  // rather than the tap's still-unprepared default.
  unsigned int applied = 0;
  REQUIRE(sonare_engine_configure_scope_telemetry(engine, kBlock, kBands, &applied) == SONARE_OK);
  REQUIRE(applied == kBands);
#else
  REQUIRE(sonare_engine_configure_scope_telemetry(engine, kBlock, kBands, nullptr) ==
          SONARE_ERROR_NOT_SUPPORTED);
#endif

  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

#if defined(SONARE_WITH_MIXING)
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, io, 2, kBlock) == SONARE_OK);

  std::array<SonareScopeTelemetryRecord, 64> records{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_scope_telemetry(engine, records.data(), records.size(), &count) ==
          SONARE_OK);
  bool checked_master = false;
  for (size_t r = 0; r < count; ++r) {
    if (records[r].target_id != 0) continue;
    REQUIRE(records[r].band_count == kBands);
    checked_master = true;
    break;
  }
  REQUIRE(checked_master);
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_drain_meter_telemetry_wide reports per-plane meters for a surround bus",
          "[c_api][engine][surround]") {
  constexpr int kBlock = 256;
  constexpr int kFrames = kBlock;
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 64, 64) == SONARE_OK);

  std::array<float, kFrames> source{};
  source.fill(0.5f);
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
  // A 5.1 group bus; the lane routes into it and is panned hard to Ls (-110deg).
  SonareEngineBus buses[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_5_1, 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 1) == SONARE_OK);
  SonareEngineTrackLane lane[] = {{10, nullptr, 0, 1, 1}};  // output_bus_id = 1
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  const char* strip_json = R"({"version":1,"buses":[{"id":"master","role":"master"}],)"
                           R"("strips":[{"id":"s","surroundPan":{"azimuth":-110}}]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, 10, strip_json) == SONARE_OK);

  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::array<std::array<float, kBlock>, 6> planes{};
  std::array<float*, 6> io{};
  for (int c = 0; c < 6; ++c) {
    io[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
  }
  REQUIRE(sonare_engine_process(engine, io.data(), 6, kBlock) == SONARE_OK);

  std::array<SonareMeterTelemetryRecordWide, 16> meters{};
  size_t meter_count = 0;
  REQUIRE(sonare_engine_drain_meter_telemetry_wide(engine, meters.data(), meters.size(),
                                                   &meter_count) == SONARE_OK);
  // The 5.1 group bus meter (target 33) publishes all six planes; the surround
  // lane lands on Ls (plane 4), well above the silent front-left plane.
  bool found_wide_bus = false;
  for (size_t i = 0; i < meter_count; ++i) {
    if (meters[i].target_id != 33) continue;
    found_wide_bus = true;
    REQUIRE(meters[i].channel_count == 6);
    REQUIRE(meters[i].peak_db[4] > meters[i].peak_db[0] + 10.0f);
  }
  REQUIRE(found_wide_bus);
#else
  std::array<SonareMeterTelemetryRecordWide, 1> meters{};
  size_t meter_count = 0;
  REQUIRE(sonare_engine_drain_meter_telemetry_wide(engine, meters.data(), meters.size(),
                                                   &meter_count) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}
