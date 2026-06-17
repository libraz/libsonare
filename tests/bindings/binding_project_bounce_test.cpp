/// @file binding_project_bounce_test.cpp
/// @brief Project bounce parity tests.

#include <sonare/sonare_c_mixing.h>

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
