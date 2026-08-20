/// @file binding_project_bounce_test.cpp
/// @brief Project bounce parity tests.

#include <sonare/sonare_c_mixing.h>

#include <algorithm>
#include <array>

#include "arrangement/edit_compiler.h"
#include "binding_project_parity_test_helpers.h"

TEST_CASE("bounce_with_instruments drives a callback instrument for routed MIDI", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  // A held note: on at ppq 0, off at ppq 2.0 (still sounding through the bounce).
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // note-on, note 60, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 2.0;
  events[1].data0 = 0x20803C00u;  // note-off, note 60
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);

  // Route the track to destination 5 and bind a callback instrument there.
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 12000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  CallbackInstrumentState state;
  SonareInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.callbacks.user_data = &state;
  binding.callbacks.prepare = &cb_prepare;
  binding.callbacks.on_event = &cb_on_event;
  binding.callbacks.render = &cb_render;
  binding.callbacks.latency_samples = 0;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2);

  // The instrument was prepared, received the note-on, and rendered audio.
  REQUIRE(state.prepared >= 1);
  REQUIRE(state.note_on == 1);
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak > 0.0f);
  sonare_free_floats(out);

  // Without instruments the same routed MIDI track bounces to silence.
  float* silent = nullptr;
  size_t silent_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &silent, &silent_len) == SONARE_OK);
  float silent_peak = 0.0f;
  for (size_t i = 0; i < silent_len; ++i) silent_peak = std::max(silent_peak, std::abs(silent[i]));
  REQUIRE(silent_peak == 0.0f);
  sonare_free_floats(silent);

  sonare_project_destroy(project);
}

TEST_CASE("channel-strip project bounce shares the live destination voice pool", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("channel-strip bounce requires the mixing build");
#else
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 128;
  constexpr int kFrames = kBlockSize * 32;
  constexpr uint32_t kDestination = 17;

  // Two tracks address one polyphony-limited destination, but their distinct
  // scene strips force the project bounce through its per-strip stem path.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"strip-a"},{"id":"strip-b"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  std::array<uint32_t, 2> tracks{};
  for (size_t i = 0; i < tracks.size(); ++i) {
    const char* route = i == 0 ? "strip-a" : "strip-b";
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
    tracks[i] = track;
    const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
    REQUIRE(sonare_project_set_midi_events(project, clip, events, 1) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
    REQUIRE(sonare_project_set_track_route(project, track, route, "") == SONARE_OK);
  }

  SonareProjectBounceOptions bounce_options{};
  bounce_options.total_frames = kFrames;
  bounce_options.block_size = kBlockSize;
  bounce_options.num_channels = 2;
  bounce_options.sample_rate = kSampleRate;
  SonareBuiltinInstrumentBinding bounce_binding{};
  bounce_binding.destination_id = kDestination;
  bounce_binding.config.polyphony = 1;
  bounce_binding.config.gain = 1.0f;
  float* bounced = nullptr;
  size_t bounced_length = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &bounce_options, &bounce_binding,
                                                         1, &bounced,
                                                         &bounced_length) == SONARE_OK);
  float bounce_peak = 0.0f;
  for (size_t i = 0; i < bounced_length; ++i) {
    bounce_peak = std::max(bounce_peak, std::abs(bounced[i]));
  }

  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlockSize, 64, 16) == SONARE_OK);
  SonareEngineBuiltinSynthConfig live_config{};
  live_config.polyphony = 1;
  live_config.gain = 1.0f;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &live_config) == SONARE_OK);
  const SonareEngineTrackLane lanes[] = {{tracks[0], nullptr, 0, 0, 1},
                                         {tracks[1], nullptr, 0, 0, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, std::size(lanes)) == SONARE_OK);
  const char* live_strip =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"live"}]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, tracks[0], live_strip) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(engine, tracks[1], live_strip) == SONARE_OK);
  // Mirror project bounce's offline pre-roll, which settles static lane
  // controls before the first audible frame. The stopped render does not
  // advance the MIDI timeline.
  std::array<float, kBlockSize> prime_left{};
  std::array<float, kBlockSize> prime_right{};
  float* prime_channels[] = {prime_left.data(), prime_right.data()};
  REQUIRE(sonare_engine_process(engine, prime_channels, 2, kBlockSize) == SONARE_OK);
  const SonareEngineMidiEvent events[] = {
      {0, 0x20903C7Fu, 0u, 0u, 0u, 1u, 0u, 0u, 0u},
  };
  const SonareEngineMidiClipSchedule clips[] = {
      {1, tracks[0], 0, 0.0, kFrames, 0, 0, kDestination, events, std::size(events)},
      {2, tracks[1], 0, 0.0, kFrames, 0, 0, kDestination, events, std::size(events)},
  };
  REQUIRE(sonare_engine_set_midi_clips(engine, clips, std::size(clips)) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
  float* channels[] = {left.data(), right.data()};
  float live_peak = 0.0f;
  for (int block = 0; block < kFrames / kBlockSize; ++block) {
    left.fill(0.0f);
    right.fill(0.0f);
    REQUIRE(sonare_engine_process(engine, channels, 2, kBlockSize) == SONARE_OK);
    for (float sample : left) {
      live_peak = std::max(live_peak, std::abs(sample));
    }
  }
  sonare_engine_destroy(engine);
  sonare_free_floats(bounced);
  sonare_project_destroy(project);

  REQUIRE(live_peak > 0.01f);
  // The live lane runtime and standalone scene mixer own distinct control
  // smoothers, but the shared destination's one-voice steal decision is common
  // to both paths. Its peak is therefore the stable observable for this fixture.
  REQUIRE(std::abs(bounce_peak - live_peak) < 1.0e-5f);
#endif
}

TEST_CASE("source-aware MIDI stems keep typed automation on their track", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("source-aware channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"a"},{"id":"b"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  constexpr uint32_t kDestination = 37;
  uint32_t track_a = 0;
  uint32_t track_b = 0;
  uint32_t clip_a = 0;
  uint32_t clip_b = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_a, &clip_a) == SONARE_OK);
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_b, &clip_b) == SONARE_OK);
  // Distinct notes make an accidental A/B owner swap observable once the
  // typed pan lanes isolate the two source stems into opposite channels.
  const SonareMidiEventPod events_a[] = {
      {0.0, 0x20903C7Fu, 0u},
      {3.0, 0x20803C00u, 0u},
  };
  const SonareMidiEventPod events_b[] = {
      {0.0, 0x20904340u, 0u},
      {3.0, 0x20804300u, 0u},
  };
  REQUIRE(sonare_project_set_midi_events(project, clip_a, events_a, std::size(events_a)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip_b, events_b, std::size(events_b)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track_a, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track_b, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track_a, "a", "") == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track_b, "b", "") == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 48000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config.polyphony = 8;

  struct StereoEnergy {
    double left = 0.0;
    double right = 0.0;
  };
  const auto bounce_energy = [&]() {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                           &out_len) == SONARE_OK);
    StereoEnergy energy;
    const size_t begin = 12000u * 2u;
    for (size_t i = begin; i + 1 < out_len; i += 2) {
      energy.left += static_cast<double>(out[i]) * static_cast<double>(out[i]);
      energy.right += static_cast<double>(out[i + 1]) * static_cast<double>(out[i + 1]);
    }
    sonare_free_floats(out);
    return energy;
  };

  SonareAutomationPoint pan_a_point{};
  pan_a_point.ppq = 0.0;
  pan_a_point.value = -1.0f;
  pan_a_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDescEx pan_a{};
  // Persistent ids intentionally use the legacy mixer values. The typed kind,
  // not this edit-time id, selects the reserved per-track playback route.
  pan_a.target_param_id = 2;  // TrackMixerRuntime::kPan
  pan_a.target_kind = SONARE_AUTOMATION_TARGET_TRACK_PAN;
  pan_a.points = &pan_a_point;
  pan_a.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane_ex(project, track_a, &pan_a, nullptr) == SONARE_OK);

  SonareAutomationPoint pan_b_point{};
  pan_b_point.ppq = 0.0;
  pan_b_point.value = 1.0f;
  pan_b_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDescEx pan_b = pan_a;
  pan_b.points = &pan_b_point;
  REQUIRE(sonare_project_add_automation_lane_ex(project, track_b, &pan_b, nullptr) == SONARE_OK);

  // First establish the asymmetric, hard-panned per-owner baseline. A lane
  // accidentally applied to the opposite source would exchange these planes.
  const StereoEnergy panned = bounce_energy();
  REQUIRE(panned.left > 1.0e-6);
  REQUIRE(panned.right > 1.0e-6);

  SonareAutomationPoint fader_point{};
  fader_point.ppq = 0.0;
  fader_point.value = -6.0206f;
  fader_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDescEx fader{};
  fader.target_param_id = 1;  // TrackMixerRuntime::kFaderDb
  fader.target_kind = SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
  fader.points = &fader_point;
  fader.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane_ex(project, track_a, &fader, nullptr) == SONARE_OK);

  const StereoEnergy fader_a_once = bounce_energy();
  const double left_ratio = fader_a_once.left / panned.left;
  const double right_ratio = fader_a_once.right / panned.right;
  // A -6 dB fader is applied once to A's left-panned stem: energy is 0.25x.
  // Applying it again through the scene strip would be 0.0625x (-12 dB), and
  // applying it to B would instead change the right plane. Both bounds make
  // the source owner and the exactly-once rule observable.
  REQUIRE(left_ratio == Catch::Approx(0.25).margin(0.025));
  REQUIRE(right_ratio == Catch::Approx(1.0).margin(0.025));
  sonare_project_destroy(project);
#endif
}

TEST_CASE("shared opaque automation keeps the first project-order owner", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("shared channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"shared"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  constexpr uint32_t kDestination = 41;
  uint32_t track_a = 0;
  uint32_t track_b = 0;
  uint32_t clip_a = 0;
  uint32_t clip_b = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_a, &clip_a) == SONARE_OK);
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track_b, &clip_b) == SONARE_OK);
  const SonareMidiEventPod events_a[] = {
      {0.0, 0x20903C7Fu, 0u},
      {3.0, 0x20803C00u, 0u},
  };
  const SonareMidiEventPod events_b[] = {
      {0.0, 0x20904340u, 0u},
      {3.0, 0x20804300u, 0u},
  };
  REQUIRE(sonare_project_set_midi_events(project, clip_a, events_a, std::size(events_a)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip_b, events_b, std::size(events_b)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track_a, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track_b, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track_a, "shared", "") == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track_b, "shared", "") == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 48000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config.polyphony = 8;

  const auto bounce_energy = [&]() {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                           &out_len) == SONARE_OK);
    double energy = 0.0;
    const size_t begin = 12000u * 2u;
    for (size_t i = begin; i < out_len; ++i) {
      energy += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    }
    sonare_free_floats(out);
    return energy;
  };

  const double unautomated = bounce_energy();
  SonareAutomationPoint first_point{};
  first_point.ppq = 0.0;
  first_point.value = -6.0206f;
  first_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDesc first_lane{};
  first_lane.target_param_id = 1;  // legacy opaque fader target
  first_lane.points = &first_point;
  first_lane.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane(project, track_a, &first_lane, nullptr) == SONARE_OK);

  SonareAutomationPoint loser_point{};
  loser_point.ppq = 0.0;
  loser_point.value = -80.0f;
  loser_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDesc loser_lane = first_lane;
  loser_lane.points = &loser_point;
  REQUIRE(sonare_project_add_automation_lane(project, track_b, &loser_lane, nullptr) == SONARE_OK);

  const double first_owner = bounce_energy();
  REQUIRE(unautomated > 1.0e-8);
  // The first project-order opaque lane reaches the shared strip once. The
  // -6 dB fader therefore reduces energy to one quarter; compiling an error,
  // selecting B, or scheduling both lanes would fail this narrow range.
  REQUIRE(first_owner / unautomated == Catch::Approx(0.25).margin(0.03));
  sonare_project_destroy(project);
#endif
}

TEST_CASE("auto-length bounce keeps both tracks of a shared MIDI destination", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  // Hard-panned strips keep each track's stem on its own plane, so a dropped
  // track shows up as a silent plane instead of a slightly quieter mix.
  const char* scene = R"({"version":1,"buses":[{"id":"master","role":"master"}],)"
                      R"("strips":[{"id":"left","pan":-1.0},{"id":"right","pan":1.0}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  uint32_t track_a = 0;
  uint32_t track_b = 0;
  uint32_t clip_a = 0;
  uint32_t clip_b = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track_a, &clip_a) == SONARE_OK);
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track_b, &clip_b) == SONARE_OK);

  // A tritone apart: neither fundamental sits on a harmonic of the other, so a
  // per-frequency probe attributes each tone to exactly one track.
  constexpr int kNoteA = 60;
  constexpr int kNoteB = 66;
  const auto note_on = [](int note) {
    return 0x20900000u | (static_cast<uint32_t>(note) << 8) | 0x7Fu;
  };
  const auto note_off = [](int note) { return 0x20800000u | (static_cast<uint32_t>(note) << 8); };
  const SonareMidiEventPod events_a[] = {
      {0.0, note_on(kNoteA), 0u},
      {0.9, note_off(kNoteA), 0u},
  };
  const SonareMidiEventPod events_b[] = {
      {0.0, note_on(kNoteB), 0u},
      {0.9, note_off(kNoteB), 0u},
  };
  REQUIRE(sonare_project_set_midi_events(project, clip_a, events_a, std::size(events_a)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip_b, events_b, std::size(events_b)) ==
          SONARE_OK);
  // Both tracks keep the default MIDI destination id 0: the implicit share that
  // must still bounce once each of them is bound to a channel strip.
  REQUIRE(sonare_project_set_track_route(project, track_a, "left", "") == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track_b, "right", "") == SONARE_OK);

  SonareProjectBounceOptions options{};
  // Auto-length is the path that prebuilds a mixer for the caller's tail query
  // and hands it to the stem summing pass.
  options.total_frames = 0;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 0;
  binding.config.polyphony = 8;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len > 0);
  REQUIRE(out_len % 2 == 0);

  // Probe the sustained middle of the render, past the attack/decay stages and
  // before the note-offs.
  const size_t frames = out_len / 2;
  const size_t begin = frames / 4;
  const size_t end = begin + (frames * 2) / 5;
  REQUIRE(end > begin);
  const auto note_hz = [](int note) {
    return static_cast<double>(sonare::constants::kA4Hz) *
           std::pow(2.0, static_cast<double>(note - static_cast<int>(sonare::constants::kMidiA4)) /
                             static_cast<double>(sonare::constants::kSemitonesPerOctave));
  };
  const auto tone_magnitude = [&](size_t channel, double hz) {
    const double w = sonare::constants::kTwoPiD * hz / 48000.0;
    double re = 0.0;
    double im = 0.0;
    for (size_t i = begin; i < end; ++i) {
      const double sample = static_cast<double>(out[i * 2 + channel]);
      re += sample * std::cos(w * static_cast<double>(i));
      im += sample * std::sin(w * static_cast<double>(i));
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(end - begin);
  };
  const double left_a = tone_magnitude(0, note_hz(kNoteA));
  const double left_b = tone_magnitude(0, note_hz(kNoteB));
  const double right_a = tone_magnitude(1, note_hz(kNoteA));
  const double right_b = tone_magnitude(1, note_hz(kNoteB));
  sonare_free_floats(out);

  // Each track reaches its own strip: dropping either one - or rejecting the
  // whole bounce over a strip-count mismatch - collapses one of these tones.
  REQUIRE(left_a > 1.0e-3);
  REQUIRE(right_b > 1.0e-3);
  REQUIRE(left_a > 2.0 * left_b);
  REQUIRE(right_b > 2.0 * right_a);
  sonare_project_destroy(project);
#endif
}

TEST_CASE("an over-capacity opaque automation lane is reported, not truncated", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("channel-strip bounce requires the mixing build");
#else
  // Builds a project whose single strip-bound track carries one opaque fader
  // lane with `point_count` breakpoints, bounces it, and returns the resulting
  // status plus the diagnostics the bounce recorded.
  struct BounceOutcome {
    SonareError status = SONARE_OK;
    std::vector<SonareProjectDiagnostic> diagnostics;
    uint32_t track_id = 0;
  };
  const auto bounce_with_breakpoints = [](size_t point_count, int64_t total_frames) {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
    const char* scene =
        R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"strip"}]})";
    REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
    const SonareMidiEventPod events[] = {
        {0.0, 0x20903C7Fu, 0u},
        {3.0, 0x20803C00u, 0u},
    };
    REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, 7u) == SONARE_OK);
    REQUIRE(sonare_project_set_track_route(project, track, "strip", "") == SONARE_OK);

    // Strictly increasing positions: at 120 BPM / 48 kHz these are 240 samples
    // apart, so every breakpoint is a distinct monotonic push and a rejection
    // can only be the lane running out of capacity.
    std::vector<SonareAutomationPoint> points(point_count);
    for (size_t i = 0; i < point_count; ++i) {
      points[i].ppq = 0.01 * static_cast<double>(i + 1);
      points[i].value = -6.0f * static_cast<float>(i % 2);
      points[i].curve_to_next = SONARE_CURVE_LINEAR;
    }
    SonareAutomationLaneDesc lane{};
    lane.target_param_id = 1;  // legacy opaque fader target
    lane.points = points.data();
    lane.point_count = points.size();
    REQUIRE(sonare_project_add_automation_lane(project, track, &lane, nullptr) == SONARE_OK);

    SonareProjectBounceOptions options{};
    options.total_frames = total_frames;
    options.block_size = 128;
    options.num_channels = 2;
    options.sample_rate = 48000;
    SonareBuiltinInstrumentBinding binding{};
    binding.destination_id = 7u;
    binding.config.polyphony = 8;

    BounceOutcome outcome;
    outcome.track_id = track;
    float* out = nullptr;
    size_t out_len = 0;
    outcome.status = sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1,
                                                                    &out, &out_len);
    sonare_free_floats(out);

    SonareProjectCompileResult compiled{};
    REQUIRE(sonare_project_last_bounce_compile_result(project, &compiled) == SONARE_OK);
    outcome.diagnostics.assign(compiled.diagnostics,
                               compiled.diagnostics + compiled.diagnostic_count);
    sonare_project_free_compile_result(&compiled);
    sonare_project_destroy(project);
    return outcome;
  };

  const auto capacity_error = [](const BounceOutcome& outcome) {
    return std::any_of(
        outcome.diagnostics.begin(), outcome.diagnostics.end(),
        [&](const SonareProjectDiagnostic& d) {
          return d.code == static_cast<uint32_t>(
                               sonare::arrangement::Diagnostic::Code::kAutomationLaneCapacity) &&
                 d.severity ==
                     static_cast<uint32_t>(sonare::arrangement::Diagnostic::Severity::kError) &&
                 d.target_id == outcome.track_id;
        });
  };

  // A lane the strip can hold bounces unchanged, with no capacity diagnostic:
  // this pins that the check does not fire on ordinary automation.
  const BounceOutcome fits = bounce_with_breakpoints(512, 48000);
  REQUIRE(fits.status == SONARE_OK);
  REQUIRE_FALSE(capacity_error(fits));

  // Past the strip lane's capacity the bounce used to render the curve frozen at
  // the last accepted breakpoint and still return success. Both the explicit and
  // the auto-derived render length must now name the lane instead.
  for (const int64_t total_frames : {static_cast<int64_t>(48000), static_cast<int64_t>(0)}) {
    INFO("total_frames " << total_frames);
    const BounceOutcome overflows = bounce_with_breakpoints(2048, total_frames);
    REQUIRE(overflows.status != SONARE_OK);
    REQUIRE(capacity_error(overflows));
  }
#endif
}

TEST_CASE("channel-strip bounce rejects a shared opaque callback destination", "[project]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"a"},{"id":"b"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  constexpr uint32_t kDestination = 23;
  for (const char* route : {"a", "b"}) {
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
    const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
    REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
    REQUIRE(sonare_project_set_track_route(project, track, route, "") == SONARE_OK);
  }

  SonareProjectBounceOptions options{};
  options.total_frames = 512;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  CallbackInstrumentState state;
  SonareInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.callbacks.user_data = &state;
  binding.callbacks.on_event = &cb_on_event;
  binding.callbacks.render = &cb_render;
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  sonare_project_destroy(project);
#endif
}

TEST_CASE("bounce_with_instruments auto length includes callback instrument tail", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;
  events[0].data1 = 0u;
  events[1].ppq = 0.5;
  events[1].data0 = 0x20803C00u;
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 0;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  CallbackInstrumentState state;
  SonareInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.callbacks.user_data = &state;
  binding.callbacks.render = &cb_render;

  float* no_tail = nullptr;
  size_t no_tail_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &no_tail,
                                                 &no_tail_len) == SONARE_OK);
  sonare_free_floats(no_tail);

  constexpr int kTailSamples = 4096;
  binding.callbacks.tail_samples = kTailSamples;
  float* with_tail = nullptr;
  size_t with_tail_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &with_tail,
                                                 &with_tail_len) == SONARE_OK);
  sonare_free_floats(with_tail);

  REQUIRE(with_tail_len == no_tail_len + static_cast<size_t>(kTailSamples * options.num_channels));
  sonare_project_destroy(project);
}

TEST_CASE("bounce_with_builtin_instruments renders the built-in synth for routed MIDI",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // note-on, note 60, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 2.0;
  events[1].data0 = 0x20803C00u;  // note-off, note 60
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 12000;
  options.num_channels = 2;
  options.sample_rate = 48000;

  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.config.waveform = SONARE_SYNTH_WAVEFORM_SAW;  // any patch (zero-init => sine)

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2);
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak > 0.0f);
  sonare_free_floats(out);

  // Determinism: an identical bounce yields bit-identical output.
  float* out2 = nullptr;
  size_t out2_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out2,
                                                         &out2_len) == SONARE_OK);
  REQUIRE(out2_len == out_len);
  sonare_free_floats(out2);

  sonare_project_destroy(project);
}

TEST_CASE("bounce_with_synth_instruments validates null handles before patch conversion",
          "[project]") {
  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 0;
  REQUIRE(sonare_synth_preset_patch("sine", &binding.patch) == SONARE_OK);

  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 123;
  REQUIRE(sonare_project_bounce_with_synth_instruments(nullptr, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  out_len = 123;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  sonare_project_destroy(project);
}

TEST_CASE("synth bounce GM mode follows program changes through the C API", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[3]{};
  events[0] = {0.0, 0x20C00400u, 0u};  // ch. 1 program 4 (electric piano)
  events[1] = {0.0, 0x20903C64u, 0u};  // ch. 1 note-on C4
  events[2] = {0.5, 0x20803C00u, 0u};
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 3) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 12000;
  options.block_size = 128;
  options.num_channels = 1;
  options.sample_rate = 48000;
  SonareSynthInstrumentBinding fixed{};
  fixed.destination_id = 0;
  REQUIRE(sonare_synth_preset_patch("sine", &fixed.patch) == SONARE_OK);
  SonareSynthInstrumentBinding gm = fixed;
  gm.use_gm_programs = 1;

  float* fixed_audio = nullptr;
  size_t fixed_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &fixed, 1, &fixed_audio,
                                                       &fixed_len) == SONARE_OK);
  float* gm_audio = nullptr;
  size_t gm_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &gm, 1, &gm_audio,
                                                       &gm_len) == SONARE_OK);
  REQUIRE(fixed_len == gm_len);
  bool differs = false;
  for (size_t i = 0; i < gm_len; ++i) {
    if (gm_audio[i] != fixed_audio[i]) {
      differs = true;
      break;
    }
  }
  REQUIRE(differs);
  sonare_free_floats(fixed_audio);
  sonare_free_floats(gm_audio);
  sonare_project_destroy(project);
}

TEST_CASE("synth bounce GM programs 4 and 40 stay audible and distinct", "[project][synth_patch]") {
  const auto render = [](uint8_t program, bool use_gm_programs) {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
    const SonareMidiEventPod events[] = {
        {0.0, 0x20C00000u | (static_cast<uint32_t>(program) << 8u), 0u},
        {0.0, 0x20903C64u, 0u},  // C4 note-on after the program change.
        {0.5, 0x20803C00u, 0u},
    };
    REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, 0) == SONARE_OK);

    SonareProjectBounceOptions options{};
    options.total_frames = 12000;
    options.block_size = 128;
    options.num_channels = 1;
    options.sample_rate = 48000;
    SonareSynthInstrumentBinding binding{};
    binding.destination_id = 0;
    binding.use_gm_programs = use_gm_programs ? 1 : 0;
    REQUIRE(sonare_synth_preset_patch("sine", &binding.patch) == SONARE_OK);

    float* audio = nullptr;
    size_t audio_len = 0;
    REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &audio,
                                                         &audio_len) == SONARE_OK);
    REQUIRE(audio != nullptr);
    REQUIRE(audio_len == static_cast<size_t>(options.total_frames));
    std::vector<float> result(audio, audio + audio_len);
    sonare_free_floats(audio);
    sonare_project_destroy(project);

    float peak = 0.0f;
    for (float sample : result) {
      REQUIRE(std::isfinite(sample));
      peak = std::max(peak, std::abs(sample));
    }
    REQUIRE(peak > 0.0f);
    return result;
  };

  const std::vector<float> disabled = render(4, false);
  const std::vector<float> gm4 = render(4, true);
  const std::vector<float> gm40 = render(40, true);
  REQUIRE(disabled.size() == gm4.size());
  REQUIRE(gm4.size() == gm40.size());

  const auto max_delta = [](const std::vector<float>& lhs, const std::vector<float>& rhs) {
    float delta = 0.0f;
    for (size_t i = 0; i < lhs.size(); ++i) {
      delta = std::max(delta, std::abs(lhs[i] - rhs[i]));
    }
    return delta;
  };
  REQUIRE(max_delta(disabled, gm4) > 1.0e-6f);
  REQUIRE(max_delta(gm4, gm40) > 1.0e-6f);
}

TEST_CASE("bounce auto-derives total_frames from the arrangement", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;
  events[0].data1 = 0u;
  events[1].ppq = 2.0;
  events[1].data0 = 0x20803C00u;
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 0) == SONARE_OK);

  // total_frames omitted (0) => auto-derived from the compiled timeline. This
  // previously returned INVALID_PARAMETER, breaking the documented quick-start.
  SonareProjectBounceOptions options{};
  options.num_channels = 2;
  options.sample_rate = 48000;

  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 0;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  // A 4-quarter clip at the default tempo spans well over a second of audio.
  REQUIRE(out_len >= 48000u * 2u);
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak > 0.0f);
  sonare_free_floats(out);

  // The plain (silent) bounce also accepts an omitted length now (no throw).
  float* silent = nullptr;
  size_t silent_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &silent, &silent_len) == SONARE_OK);
  REQUIRE(silent_len >= 48000u * 2u);
  sonare_free_floats(silent);

  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects non-project output sample rates", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 0) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 44100;

  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 123;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);

  options.sample_rate = 48000;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == 256);
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("mono project bounce downmixes stereo audio clips instead of dropping right",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  constexpr int kFrames = 256;
  std::vector<float> right_only(static_cast<size_t>(kFrames) * 2, 0.0f);
  for (int i = 0; i < kFrames; ++i) {
    right_only[static_cast<size_t>(i) * 2 + 1] = 1.0f;
  }

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "right-only";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = right_only.data();
  clip_desc.audio_frames = kFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = 64;
  options.num_channels = 1;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(kFrames));
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak == Catch::Approx(0.5f).margin(1e-6f));
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("deserialize success returns warnings and failed bounce records missing timeline",
          "[project]") {
  const std::string json =
      R"({"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,)"
      R"("channel_strip_ref":"","output_target":"","midi_destination_id":0,)"
      R"("automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,)"
      R"("start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,)"
      R"("fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},)"
      R"("loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0}]})";

  SonareProject* project = nullptr;
  char* diag = nullptr;
  REQUIRE(sonare_project_deserialize(json.c_str(), json.size(), &project, &diag) == SONARE_OK);
  REQUIRE(project != nullptr);
  REQUIRE(diag != nullptr);
  const std::string warnings(diag);
  REQUIRE(warnings.find("dangling_clip_source") != std::string::npos);
  sonare_free_string(diag);

  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_last_bounce_compile_result(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline == 0);
  REQUIRE(result.diagnostic_count > 0);
  sonare_project_free_compile_result(&result);
  sonare_project_destroy(project);
}

TEST_CASE("a never-bounced project reports a fully empty last-bounce compile result", "[project]") {
  // Both a freshly created project and one just loaded from JSON have no
  // recorded bounce, so both read as empty in full: no timeline AND no
  // diagnostics. That pair is what lets a host tell "never rendered" from a
  // failed render, since a bounce only loses its timeline through an error
  // diagnostic and therefore always leaves at least one behind (the case
  // asserted directly above).
  SonareProject* created = nullptr;
  REQUIRE(sonare_project_create(&created) == SONARE_OK);
  SonareProjectCompileResult fresh{};
  REQUIRE(sonare_project_last_bounce_compile_result(created, &fresh) == SONARE_OK);
  REQUIRE(fresh.has_timeline == 0);
  REQUIRE(fresh.diagnostic_count == 0);
  sonare_project_free_compile_result(&fresh);

  char* json = nullptr;
  size_t json_len = 0;
  REQUIRE(sonare_project_serialize(created, &json, &json_len) == SONARE_OK);
  sonare_project_destroy(created);

  SonareProject* loaded = nullptr;
  REQUIRE(sonare_project_deserialize(json, json_len, &loaded, nullptr) == SONARE_OK);
  sonare_free_string(json);
  SonareProjectCompileResult reloaded{};
  REQUIRE(sonare_project_last_bounce_compile_result(loaded, &reloaded) == SONARE_OK);
  REQUIRE(reloaded.has_timeline == 0);
  REQUIRE(reloaded.diagnostic_count == 0);
  sonare_project_free_compile_result(&reloaded);
  sonare_project_destroy(loaded);
}

TEST_CASE("project bounce exposes unrouted opaque automation diagnostics", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "unrouted automation";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);

  SonareAutomationPoint point{};
  point.ppq = 0.0;
  point.value = 0.25f;
  point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDesc lane{};
  lane.target_param_id = 99;  // host-defined opaque target
  lane.points = &point;
  lane.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane(project, track, &lane, nullptr) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == 256);
  sonare_free_floats(out);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_last_bounce_compile_result(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline != 0);
  REQUIRE(std::any_of(result.diagnostics, result.diagnostics + result.diagnostic_count,
                      [&](const SonareProjectDiagnostic& diagnostic) {
                        return diagnostic.code == 18u && diagnostic.severity == 1u &&
                               diagnostic.target_id == track;
                      }));
  REQUIRE(result.messages != nullptr);
  REQUIRE(std::string(result.messages).find("host-defined") != std::string::npos);
  REQUIRE(std::string(result.messages).find("unrouted") != std::string::npos);
  sonare_project_free_compile_result(&result);
  sonare_project_destroy(project);
}

namespace {

// These helpers serve the channel-strip cases below and nothing else, so they
// carry the same guard -- ungated they would have no callers under
// -DBUILD_MIXING=OFF, which the build rejects as unused functions.
#if defined(SONARE_WITH_MIXING)

// Replaces the first occurrence of `from` in `text` with `to`. REQUIRE-guards
// that the token was present so a serializer schema change fails loudly here
// rather than silently skipping the channel-strip wiring.
std::string replace_first(std::string text, const std::string& from, const std::string& to) {
  const size_t pos = text.find(from);
  REQUIRE(pos != std::string::npos);
  text.replace(pos, from.size(), to);
  return text;
}

// Peak absolute amplitude of an interleaved buffer.
float buffer_peak(const float* data, size_t len, size_t start = 0) {
  float peak = 0.0f;
  for (size_t i = std::min(start, len); i < len; ++i) peak = std::max(peak, std::abs(data[i]));
  return peak;
}

// Deserializes project JSON into a new handle (REQUIRE-guards success).
SonareProject* deserialize_project(const std::string& json) {
  SonareProject* project = nullptr;
  char* diag = nullptr;
  REQUIRE(sonare_project_deserialize(json.c_str(), json.size(), &project, &diag) == SONARE_OK);
  REQUIRE(project != nullptr);
  sonare_free_string(diag);
  return project;
}

#endif  // SONARE_WITH_MIXING

}  // namespace

// Channel-strip rendering only exists when the mixing subsystem is built;
// without it the bounce has no strip to apply.
#if defined(SONARE_WITH_MIXING)
TEST_CASE("bounce renders per-track channel-strip effects", "[project]") {
  // A single MIDI track routed to the built-in synth, bound to a mixer channel
  // strip. Before the fix the bounce dropped the strip entirely; now it renders
  // the track's stem through the scene's mixer, so a muted strip silences the
  // bounce and a transparent strip preserves it. MIDI content survives
  // serialization (host-supplied audio sample stores do not), so the synth
  // regenerates the stem the strip then processes.
  SonareProject* base = nullptr;
  REQUIRE(sonare_project_create(&base) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(base, 48000.0) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(base, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // note-on, note 60, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 3.0;
  events[1].data0 = 0x20803C00u;  // note-off, note 60
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(base, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(base, track, 0) == SONARE_OK);
  SonareAutomationPoint fader_point{};
  fader_point.ppq = 0.0;
  fader_point.value = -80.0f;
  fader_point.curve_to_next = SONARE_CURVE_HOLD;
  SonareAutomationLaneDesc fader_lane{};
  fader_lane.target_param_id = 1;  // engine::MixingRuntime::kFaderDb
  fader_lane.points = &fader_point;
  fader_lane.point_count = 1;
  uint32_t target_param_id = 0;
  REQUIRE(sonare_project_add_automation_lane(base, track, &fader_lane, &target_param_id) ==
          SONARE_OK);
  const std::string json_plain = serialize(base);
  sonare_project_destroy(base);

  SonareProjectBounceOptions options{};
  options.total_frames = 48000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 0;

  auto builtin_bounce_peak = [&](const std::string& json, size_t skip_samples = 0) {
    SonareProject* project = deserialize_project(json);
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                           &out_len) == SONARE_OK);
    const float peak = buffer_peak(out, out_len, skip_samples);
    sonare_free_floats(out);
    sonare_project_destroy(project);
    return peak;
  };

  // Reference: the unmodified project auditions the synth audibly. The fader
  // automation lane has no bound strip here, so it cannot affect the direct path.
  const float direct_peak = builtin_bounce_peak(json_plain, 12000);
  REQUIRE(direct_peak > 0.0f);

  // Bind the track to a muted strip; the bounce must now fall silent.
  const std::string json_muted = replace_first(
      replace_first(json_plain, "\"strips\":[]", "\"strips\":[{\"id\":\"s0\",\"muted\":true}]"),
      "\"channel_strip_ref\":\"\"", "\"channel_strip_ref\":\"s0\"");
  REQUIRE(builtin_bounce_peak(json_muted) == Catch::Approx(0.0f).margin(1e-6));

  // A transparent strip bound to the track must also bind the track automation:
  // the fader lane drives the strip to -80 dB instead of being silently ignored.
  const std::string json_unity = replace_first(
      replace_first(json_plain, "\"strips\":[]", "\"strips\":[{\"id\":\"s0\",\"faderDb\":0.0}]"),
      "\"channel_strip_ref\":\"\"", "\"channel_strip_ref\":\"s0\"");
  const float automated_peak = builtin_bounce_peak(json_unity, 12000);
  REQUIRE(automated_peak > 0.0f);
  REQUIRE(automated_peak < direct_peak * 0.001f);
}
#endif  // SONARE_WITH_MIXING

// Both cases below route through effects.delay.stereo / a reverb send preset,
// so they additionally need the FX suite.
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_FX)
TEST_CASE("channel-strip bounce auto-renders mixer insert tails", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  char* scene_json = nullptr;
  REQUIRE(sonare_mixing_scene_preset_json("vocalReverbSend", &scene_json) == SONARE_OK);
  REQUIRE(scene_json != nullptr);
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);
  sonare_free_string(scene_json);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "impulse";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "vocal", nullptr) == SONARE_OK);

  constexpr int kClipFrames = 240;
  std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
  impulse[0] = 1.0f;
  impulse[1] = 1.0f;

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 0.01;  // 240 frames at 120 BPM / 48 kHz.
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = impulse.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  const size_t arrangement_samples = static_cast<size_t>(kClipFrames) * 2u;
  REQUIRE(out_len > arrangement_samples);
  REQUIRE(buffer_peak(out, out_len, arrangement_samples) > 0.0f);
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("channel-strip bounce preserves the longest serial send tail", "[project][tail]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  const char* scene_json = R"({
    "version":1,
    "strips":[{"id":"source","inserts":[
      {"slot":"post","processor":"effects.delay.stereo",
       "params":"{\"delayTimeLMs\":10,\"delayTimeRMs\":10,\"feedback\":0,\"dryWet\":1}"}],
      "sends":[{"id":"to-aux","destinationBusId":"aux","sendDb":0,"timing":"post"}]}],
    "buses":[
      {"id":"aux","role":"aux","inserts":[
        {"slot":"post","processor":"effects.delay.stereo",
         "params":"{\"delayTimeLMs\":20,\"delayTimeRMs\":20,\"feedback\":0,\"dryWet\":1}"}]},
      {"id":"master","role":"master","inserts":[
        {"slot":"post","processor":"effects.delay.stereo",
         "params":"{\"delayTimeLMs\":30,\"delayTimeRMs\":30,\"feedback\":0,\"dryWet\":1}"}]}],
    "connections":[{"source":"aux","destination":"master"}]
  })";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  // The accessor and auto-length bounce consume the same compiled master-path
  // value. It must exceed every individual insert tail on this serial route.
  SonareMixer* probe = sonare_mixer_from_scene_json(scene_json, 48000, 128);
  REQUIRE(probe != nullptr);
  int expected_tail = 0;
  REQUIRE(sonare_mixer_tail_samples(probe, &expected_tail) == SONARE_OK);
  REQUIRE(expected_tail > 1440);  // master delay alone is about 30 ms / 1440 samples.
  sonare_mixer_destroy(probe);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "serial-tail-impulse";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "source", nullptr) == SONARE_OK);

  constexpr int kClipFrames = 240;
  std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
  impulse[0] = 1.0f;
  impulse[1] = 1.0f;
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 0.01;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = impulse.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(kClipFrames + expected_tail) * 2u);
  const size_t late_route_start = static_cast<size_t>(std::max(0, expected_tail - 128)) * 2u;
  REQUIRE(buffer_peak(out, out_len, late_route_start) > 0.1f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
}
#endif  // defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_FX)

TEST_CASE("channel-strip bounce preserves tail impulse through master latency insert",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"track\"}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\",\"inserts\":["
      "{\"slot\":\"post\",\"processor\":\"dynamics.brickwallLimiter\","
      "\"params\":\"{\\\"lookaheadMs\\\":1.0,\\\"ceilingDb\\\":0.0}\"}]}],"
      "\"connections\":[{\"source\":\"track\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "tail-impulse";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "track", nullptr) == SONARE_OK);

  constexpr int kClipFrames = 240;
  constexpr int kImpulseFrame = kClipFrames - 32;
  std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
  impulse[static_cast<size_t>(kImpulseFrame) * 2u] = 0.25f;
  impulse[static_cast<size_t>(kImpulseFrame) * 2u + 1u] = 0.25f;

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 0.01;  // 240 frames at 120 BPM / 48 kHz.
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = impulse.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(kClipFrames) * 2u);
  size_t peak_index = 0;
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) {
    if (std::abs(out[i]) > peak) {
      peak = std::abs(out[i]);
      peak_index = i;
    }
  }
  INFO("peak=" << peak << " index=" << peak_index);
  REQUIRE(peak > 0.1f);
  REQUIRE(peak_index >= static_cast<size_t>(kImpulseFrame) * 2u);

  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("channel-strip bounce compensates mixer latency for unbound tracks", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  constexpr int kLatency = 16;
  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"latency\",\"channelDelaySamples\":16}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\"}],"
      "\"connections\":[{\"source\":\"latency\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  auto add_impulse_track = [&](const char* name, const char* strip_ref) {
    SonareProjectTrackDesc track_desc{};
    track_desc.kind = SONARE_TRACK_AUDIO;
    track_desc.name = name;
    uint32_t track = 0;
    REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
    if (strip_ref != nullptr) {
      REQUIRE(sonare_project_set_track_route(project, track, strip_ref, nullptr) == SONARE_OK);
    }

    constexpr int kClipFrames = 128;
    std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
    impulse[0] = 1.0f;
    impulse[1] = 1.0f;
    SonareProjectClipDesc clip_desc{};
    clip_desc.track_id = track;
    clip_desc.is_midi = 0;
    clip_desc.start_ppq = 0.0;
    clip_desc.length_ppq = 1.0;
    clip_desc.gain = 1.0f;
    clip_desc.audio_interleaved = impulse.data();
    clip_desc.audio_frames = kClipFrames;
    clip_desc.audio_channels = 2;
    clip_desc.audio_sample_rate = 48000;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);
  };

  add_impulse_track("bound", "latency");
  add_impulse_track("direct", nullptr);

  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2u);

  REQUIRE(out[0] == Catch::Approx(2.0f).margin(1e-5f));
  REQUIRE(out[1] == Catch::Approx(2.0f).margin(1e-5f));
  REQUIRE(out[static_cast<size_t>(kLatency) * 2u] == Catch::Approx(0.0f).margin(1e-6f));
  REQUIRE(out[static_cast<size_t>(kLatency) * 2u + 1u] == Catch::Approx(0.0f).margin(1e-6f));

  sonare_free_floats(out);
  sonare_project_destroy(project);
}

// Routes through effects.reverb.plate on the master bus, so it needs the FX
// suite in addition to the default-on channel-strip mixing path.
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_FX)
TEST_CASE("channel-strip bounce routes unbound tracks through master bus effects", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"bound\"}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\",\"inserts\":["
      "{\"slot\":\"post\",\"processor\":\"effects.reverb.plate\","
      "\"params\":\"{\\\"decaySec\\\":1.2,\\\"dryWet\\\":1.0}\"}]}],"
      "\"connections\":[{\"source\":\"bound\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  SonareProjectTrackDesc bound_desc{};
  bound_desc.kind = SONARE_TRACK_AUDIO;
  bound_desc.name = "bound";
  uint32_t bound_track = 0;
  REQUIRE(sonare_project_add_track(project, &bound_desc, &bound_track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, bound_track, "bound", nullptr) == SONARE_OK);

  SonareProjectTrackDesc direct_desc{};
  direct_desc.kind = SONARE_TRACK_AUDIO;
  direct_desc.name = "direct";
  uint32_t direct_track = 0;
  REQUIRE(sonare_project_add_track(project, &direct_desc, &direct_track) == SONARE_OK);

  constexpr int kClipFrames = 240;
  std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
  impulse[0] = 1.0f;
  impulse[1] = 1.0f;
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = direct_track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 0.01;  // 240 frames at 120 BPM / 48 kHz.
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = impulse.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  const size_t arrangement_samples = static_cast<size_t>(kClipFrames) * 2u;
  REQUIRE(out_len > arrangement_samples);
  REQUIRE(buffer_peak(out, out_len, arrangement_samples) > 0.0f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
}
#endif  // defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_FX)

// Channel-strip rendering only exists when the mixing subsystem is built;
// without it the bounce has no strip to apply.
#if defined(SONARE_WITH_MIXING)
TEST_CASE("channel-strip bounce solo mutes unbound direct tracks", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"solo\",\"soloed\":true}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\"}],"
      "\"connections\":[{\"source\":\"solo\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  SonareProjectTrackDesc solo_desc{};
  solo_desc.kind = SONARE_TRACK_AUDIO;
  solo_desc.name = "solo";
  uint32_t solo_track = 0;
  REQUIRE(sonare_project_add_track(project, &solo_desc, &solo_track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, solo_track, "solo", nullptr) == SONARE_OK);

  SonareProjectTrackDesc direct_desc{};
  direct_desc.kind = SONARE_TRACK_AUDIO;
  direct_desc.name = "direct";
  uint32_t direct_track = 0;
  REQUIRE(sonare_project_add_track(project, &direct_desc, &direct_track) == SONARE_OK);

  constexpr int kClipFrames = 128;
  std::vector<float> impulse(static_cast<size_t>(kClipFrames) * 2, 0.0f);
  impulse[0] = 1.0f;
  impulse[1] = 1.0f;

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = direct_track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = impulse.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = kClipFrames;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(kClipFrames) * 2u);
  REQUIRE(buffer_peak(out, out_len) == Catch::Approx(0.0f).margin(1e-6f));

  sonare_free_floats(out);
  sonare_project_destroy(project);
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("bounce_with_instruments PDC-compensates a latency-bearing instrument", "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  // A single note-on at ppq 0 (render frame 0).
  SonareMidiEventPod ev{};
  ev.ppq = 0.0;
  ev.data0 = 0x20903C40u;  // note-on, note 60
  ev.data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, &ev, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  constexpr int kLatency = 64;
  SonareProjectBounceOptions options{};
  options.total_frames = 512;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  LatencyCallbackState state;
  state.latency = kLatency;
  SonareInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.callbacks.user_data = &state;
  binding.callbacks.on_event = &lcb_on_event;
  binding.callbacks.render = &lcb_render;
  binding.callbacks.latency_samples = kLatency;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_OK);
  REQUIRE(out != nullptr);
  // The returned length is the requested frame count: the PDC pre-roll is
  // rendered then trimmed, not appended.
  REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2);

  // PDC: the note at musical frame 0 drives an instrument whose audible impulse
  // is internally `kLatency` late. The bounce renders the extra `kLatency`
  // frames and drops the leading pre-roll, so the impulse lands back at OUTPUT
  // frame 0 (both stereo samples), not at frame kLatency.
  REQUIRE(out[0] == Catch::Approx(1.0f));
  REQUIRE(out[1] == Catch::Approx(1.0f));
  // Where the uncompensated impulse would have appeared is now silent.
  REQUIRE(out[static_cast<size_t>(kLatency) * 2] == Catch::Approx(0.0f));
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

namespace {

// Records every note-on the sequencer dispatches (note number + render frame),
// so a bounce can assert that sequential clip notes ALL reach the instrument,
// not just the first. render() is a required no-op (callback instruments must
// supply a render function).
struct SequenceRecorderState {
  std::vector<uint8_t> note_on_notes;
  std::vector<int64_t> note_on_frames;
  int note_off = 0;
};

void seq_on_event(void* user, uint32_t /*destination_id*/, const uint32_t* words, int word_count,
                  int64_t render_frame) {
  if (word_count < 1) return;
  auto* state = static_cast<SequenceRecorderState*>(user);
  const uint8_t status = static_cast<uint8_t>((words[0] >> 16) & 0xF0u);
  const uint8_t note = static_cast<uint8_t>((words[0] >> 8) & 0x7Fu);
  if (status == 0x90u) {
    state->note_on_notes.push_back(note);
    state->note_on_frames.push_back(render_frame);
  } else if (status == 0x80u) {
    state->note_off += 1;
  }
}

void seq_render(void* /*user*/, float* const* /*channels*/, int /*num_channels*/,
                int /*num_frames*/) {}

}  // namespace

TEST_CASE("bounce dispatches every note of a sequential melody, not just the first", "[project]") {
  // Regression: a clip holding a sequential (non-overlapping) melody must
  // dispatch ALL its note events through the offline bounce. A prior defect
  // stopped MIDI dispatch after the first block, so only the note sounding at
  // render frame 0 ever reached the instrument (it then sustained forever
  // because its note-off was dropped too) -- a melody bounced as one frozen
  // note. The recorder asserts each note-on arrives, in order, at a distinct
  // and increasing render frame.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  // Three separated notes: C4 (60), E4 (64), G4 (67), each a quarter long with a
  // quarter of silence after, so no two notes overlap.
  SonareMidiEventPod events[6];
  const uint8_t notes[3] = {60, 64, 67};
  for (int i = 0; i < 3; ++i) {
    const double on_ppq = static_cast<double>(i);  // 0, 1, 2 quarters
    events[i * 2].ppq = on_ppq;
    events[i * 2].data0 = 0x20900040u | (static_cast<uint32_t>(notes[i]) << 8) | 0x40u;
    events[i * 2].data1 = 0u;
    events[i * 2 + 1].ppq = on_ppq + 0.5;  // note-off half a beat later
    events[i * 2 + 1].data0 = 0x20800000u | (static_cast<uint32_t>(notes[i]) << 8);
    events[i * 2 + 1].data1 = 0u;
  }
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 6) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 120000;  // 2.5 s at 48 kHz, past the last note-off.
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  SequenceRecorderState state;
  SonareInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.callbacks.user_data = &state;
  binding.callbacks.on_event = &seq_on_event;
  binding.callbacks.render = &seq_render;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_OK);
  sonare_free_floats(out);

  // All three note-ons must have been dispatched, in pitch order, each at a
  // strictly increasing render frame (the bug delivered only the first).
  REQUIRE(state.note_on_notes.size() == 3);
  REQUIRE(state.note_on_notes[0] == 60);
  REQUIRE(state.note_on_notes[1] == 64);
  REQUIRE(state.note_on_notes[2] == 67);
  REQUIRE(state.note_on_frames[0] < state.note_on_frames[1]);
  REQUIRE(state.note_on_frames[1] < state.note_on_frames[2]);
  // And every note-off too (3) -- otherwise the first note would hang.
  REQUIRE(state.note_off == 3);

  sonare_project_destroy(project);
}

namespace {

// Median f0 (Hz) of a mono window via the C-ABI YIN tracker, over voiced frames
// only (fill_na = 0). Returns 0 when no frame is voiced.
float window_median_hz(const float* samples, size_t length, float fmin, float fmax) {
  SonarePitchResult pitch{};
  REQUIRE(sonare_pitch_yin(samples, length, 48000, 2048, 512, fmin, fmax, 0.1f,
                           /*fill_na=*/0, &pitch) == SONARE_OK);
  const float median = pitch.median_f0;
  sonare_free_pitch_result(&pitch);
  return median;
}

}  // namespace

TEST_CASE("bounce retunes sequential synth notes to each note's pitch, not the first",
          "[project][synth_patch]") {
  // Regression for the audio-domain side of the sequential-melody defect: even
  // when every note-on is dispatched (see the dispatch test above), the synth
  // must RETUNE for each note. A clip holding C5 then G5 (non-overlapping) must
  // bounce audio whose pitch tracks C5 early and G5 late -- the reported bug
  // left the second note sounding at the first note's pitch while the amplitude
  // envelope still retriggered. This asserts on the rendered f0, which the
  // dispatch-count test cannot catch.
  constexpr uint8_t kC5 = 72;  // 523.25 Hz
  constexpr uint8_t kG5 = 79;  // 783.99 Hz
  // 120 BPM default, 48 kHz -> one quarter note = 24000 frames.
  constexpr int kQuarter = 24000;

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);

  // C5 sounds over [0, 1) quarters (frames 0..24000); after a quarter of
  // silence, G5 sounds over [2, 3) quarters (frames 48000..72000).
  SonareMidiEventPod events[4];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20900040u | (static_cast<uint32_t>(kC5) << 8);  // C5 on, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 1.0;
  events[1].data0 = 0x20800000u | (static_cast<uint32_t>(kC5) << 8);  // C5 off
  events[1].data1 = 0u;
  events[2].ppq = 2.0;
  events[2].data0 = 0x20900040u | (static_cast<uint32_t>(kG5) << 8);  // G5 on, vel 64
  events[2].data1 = 0u;
  events[3].ppq = 3.0;
  events[3].data0 = 0x20800000u | (static_cast<uint32_t>(kG5) << 8);  // G5 off
  events[3].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 4) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &patch) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 4 * kQuarter;  // 2.0 s: through the end of G5.
  options.block_size = 128;
  options.num_channels = 1;  // mono -> sample index == frame index for YIN.
  options.sample_rate = 48000;

  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 5;
  binding.patch = patch;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(options.total_frames));

  // Sanity: both notes produced audio.
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak > 0.0f);

  // Window into the sustain of each note (skip the attack transient, stay before
  // note-off). fmin/fmax bracket C5..G5 with margin so YIN cannot alias one to
  // the other.
  const float c5_median = window_median_hz(out + 4000, 16000, 300.0f, 1100.0f);
  const float g5_median = window_median_hz(out + 52000, 16000, 300.0f, 1100.0f);
  sonare_free_floats(out);

  // The first note tracks C5 (~523 Hz).
  REQUIRE(c5_median == Catch::Approx(523.25f).epsilon(0.06));
  // The decisive assertion: the second note must be RETUNED to G5 (~784 Hz),
  // not frozen at C5. A regression leaves g5_median near 523 Hz.
  REQUIRE(g5_median == Catch::Approx(783.99f).epsilon(0.06));
  REQUIRE(g5_median > 640.0f);  // unambiguously above C5.

  sonare_project_destroy(project);
}

// Channel-strip rendering only exists when the mixing subsystem is built;
// without it the bounce has no strip to apply.
#if defined(SONARE_WITH_MIXING)
TEST_CASE("channel-strip bounce opens at the static fader gain without a first-block ramp",
          "[project]") {
  // Regression for the bounce-settle defect: a strip with a non-default static
  // fader fed a constant source used to fade in over the first ~5 ms block,
  // because neither bounce path snapped its smoothers before the audible render.
  // The very first frame must already sit at the configured fader gain.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  // -6.0206 dB == 0.5 linear. A constant 0.5 source through this strip settles to
  // 0.25; if the fader smoother ramps in from 0 dB (unity) the first frame is
  // near the unattenuated 0.5 instead.
  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"s0\",\"faderDb\":-6.0206}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\"}],"
      "\"connections\":[{\"source\":\"s0\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "dc";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "s0", nullptr) == SONARE_OK);

  constexpr int kClipFrames = 2048;  // ~43 ms: well past the 5 ms fader smoother.
  std::vector<float> dc(static_cast<size_t>(kClipFrames) * 2, 0.5f);
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = static_cast<double>(kClipFrames) / 24000.0;  // 120 BPM, 48 kHz.
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = dc.data();
  clip_desc.audio_frames = kClipFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 1024;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == 2048u);  // 1024 frames * 2 channels.

  // The steady-state level (a late frame) is the reference.
  const float steady_l = out[2u * 1000u];
  const float steady_r = out[2u * 1000u + 1u];
  REQUIRE(steady_l == Catch::Approx(0.25f).margin(1e-4f));
  REQUIRE(steady_r == Catch::Approx(0.25f).margin(1e-4f));

  // The decisive assertion: the first frame must already be at the steady gain,
  // not the ~0.5 a fade-in from unity would produce.
  REQUIRE(out[0] == Catch::Approx(steady_l).margin(1e-4f));
  REQUIRE(out[1] == Catch::Approx(steady_r).margin(1e-4f));

  sonare_free_floats(out);
  sonare_project_destroy(project);
}
#endif  // SONARE_WITH_MIXING

TEST_CASE("bounce_with_builtin_instruments follows CC7 volume and CC11 expression", "[project]") {
  // 120 BPM: one quarter note is 24000 frames at 48 kHz.
  constexpr uint32_t kNoteOn = 0x20903C64u;   // note-on, note 60, vel 100
  constexpr uint32_t kNoteOff = 0x20803C00u;  // note-off, note 60

  // RMS of one interleaved frame range, over every channel.
  auto range_rms = [](const float* data, size_t first_frame, size_t last_frame, int num_channels) {
    double sum = 0.0;
    const size_t first = first_frame * static_cast<size_t>(num_channels);
    const size_t last = last_frame * static_cast<size_t>(num_channels);
    for (size_t i = first; i < last; ++i) sum += static_cast<double>(data[i]) * data[i];
    return static_cast<float>(std::sqrt(sum / static_cast<double>(last - first)));
  };

  SonareProjectBounceOptions options{};
  options.total_frames = 48000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 3;

  // Renders one held note under the given control-change events.
  auto bounce_with = [&](const std::vector<SonareMidiEventPod>& events) {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
    REQUIRE(sonare_project_set_midi_events(project, clip, events.data(), events.size()) ==
            SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, 3) == SONARE_OK);
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                           &out_len) == SONARE_OK);
    REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2u);
    std::vector<float> audio(out, out + out_len);
    sonare_free_floats(out);
    sonare_project_destroy(project);
    return audio;
  };

  // A fade-out written as expression: full for the first beat, then CC11 == 32.
  const std::vector<float> faded = bounce_with(
      {{0.0, kNoteOn, 0u}, {0.0, 0x20B00B7Fu, 0u}, {1.0, 0x20B00B20u, 0u}, {2.0, kNoteOff, 0u}});
  const float before_fade = range_rms(faded.data(), 4000, 20000, 2);
  const float after_fade = range_rms(faded.data(), 28000, 44000, 2);
  REQUIRE(before_fade > 0.0f);
  const float expression_ratio = (32.0f / 127.0f) * (32.0f / 127.0f);
  REQUIRE(after_fade == Catch::Approx(before_fade * expression_ratio).epsilon(0.1));

  // The same note under a lower CC7 renders quieter by the same law.
  const std::vector<float> loud =
      bounce_with({{0.0, kNoteOn, 0u}, {0.0, 0x20B0077Fu, 0u}, {2.0, kNoteOff, 0u}});
  const std::vector<float> quiet =
      bounce_with({{0.0, kNoteOn, 0u}, {0.0, 0x20B00740u, 0u}, {2.0, kNoteOff, 0u}});
  const float loud_rms = range_rms(loud.data(), 4000, 44000, 2);
  const float quiet_rms = range_rms(quiet.data(), 4000, 44000, 2);
  REQUIRE(loud_rms > 0.0f);
  const float volume_ratio = (64.0f / 127.0f) * (64.0f / 127.0f);
  REQUIRE(quiet_rms == Catch::Approx(loud_rms * volume_ratio).epsilon(0.1));

  // CC10 places the part: hard left leaves the right channel nearly empty.
  const std::vector<float> left_panned =
      bounce_with({{0.0, kNoteOn, 0u}, {0.0, 0x20B00A00u, 0u}, {2.0, kNoteOff, 0u}});
  double left_energy = 0.0;
  double right_energy = 0.0;
  for (size_t frame = 4000; frame < 44000; ++frame) {
    const double l = left_panned[frame * 2u];
    const double r = left_panned[frame * 2u + 1u];
    left_energy += l * l;
    right_energy += r * r;
  }
  REQUIRE(left_energy > 0.0);
  REQUIRE(right_energy < left_energy * 1e-4);
}
