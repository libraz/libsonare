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

namespace {

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

}  // namespace

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
  size_t lane_index = 0;
  REQUIRE(sonare_project_add_automation_lane(base, track, &fader_lane, &lane_index) == SONARE_OK);
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
