/// @file sonare_c_engine_voice_test.cpp
/// @brief Engine and voice C API tests.

#include "sonare_c_test_helpers.h"

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
