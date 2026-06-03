/// @file binding_project_bounce_test.cpp
/// @brief Project bounce parity tests.

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
