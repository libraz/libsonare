/// @file sonare_c_engine_midi_test.cpp
/// @brief Engine C ABI MIDI input, built-in instrument and per-note dimensions.

#include "sonare_c_engine_test_helpers.h"

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

  SonareMidiCcBinding high_resolution{};
  high_resolution.cc_number = 1;
  high_resolution.cc_lsb_number = 33;
  high_resolution.channel = 0;
  high_resolution.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
  high_resolution.param_id = 7;
  high_resolution.min_value = -60.0f;
  high_resolution.max_value = 0.0f;
  REQUIRE(sonare_engine_bind_midi_cc_binding(engine, &high_resolution) == SONARE_OK);
  REQUIRE(sonare_engine_midi_cc_binding_count(engine, &binding_count) == SONARE_OK);
  REQUIRE(binding_count == 1);

  audio.fill(1.0f);
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 1, 64, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_cc(engine, 0, 0, 0, 33, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_process(engine, channels, 1, 64) == SONARE_OK);
  REQUIRE(std::abs(audio[0] - 0.03163f) < 0.002f);
#elif defined(SONARE_WITH_ARRANGEMENT)
  // Binding a CC needs only the arrangement subsystem; the block above
  // additionally needs the graph to observe the bound parameter actually move.
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, 7, -60.0f, 0.0f) == SONARE_OK);
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

TEST_CASE("sonare_engine refuses a built-in waveform outside the enum", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

#if defined(SONARE_WITH_ARRANGEMENT)
  SonareEngineBuiltinSynthConfig synth{};
  synth.gain = 0.5f;
  // Same domain as the bounce surface: the two entry points share one validator
  // so they cannot answer differently for the same ordinal.
  for (const int waveform : {SONARE_SYNTH_WAVEFORM_COUNT, -1, 5, 2147483647}) {
    CAPTURE(waveform);
    synth.waveform = waveform;
    REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
  // A refused bind leaves nothing attached, so the destination is still free.
  size_t count = 1;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 0);

  for (int waveform = 0; waveform < SONARE_SYNTH_WAVEFORM_COUNT; ++waveform) {
    CAPTURE(waveform);
    synth.waveform = waveform;
    REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  }
  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
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

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine drains external MIDI routing to the host", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // Route destination 5 to the external queue instead of an instrument.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 5, 1) == SONARE_OK);
  const SonareEngineMidiEvent events[] = {
      {0, midi1_word(0x9, 1, 64, 110), 0, 0, 0, 1, 0, 0, 0},
      {48, midi1_word(0x8, 1, 64, 0), 0, 0, 0, 1, 0, 0, 0},
  };
  SonareEngineMidiClipSchedule clip{};
  clip.id = 7;
  clip.track_id = 5;
  clip.length_samples = 256;
  clip.destination_id = 5;
  clip.events = events;
  clip.event_count = 2;
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);

  std::array<SonareExternalMidiEvent, 16> drained{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
          SONARE_OK);
  REQUIRE(count == 2);
  REQUIRE(drained[0].destination_id == 5);
  REQUIRE(drained[0].byte_count == 3);
  REQUIRE((drained[0].bytes[0] & 0xF0u) == 0x90u);  // note on
  REQUIRE(drained[0].render_frame == 0);
  REQUIRE(drained[1].destination_id == 5);
  REQUIRE((drained[1].bytes[0] & 0xF0u) == 0x80u);  // note off
  REQUIRE(drained[1].render_frame == 48);

  // Overflow telemetry is observable, and a too-small buffer is rejected.
  uint32_t dropped = 123;
  REQUIRE(sonare_engine_external_midi_dropped_count(engine, &dropped) == SONARE_OK);
  REQUIRE(dropped == 0);
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), 2, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // Argument guards.
  REQUIRE(sonare_engine_set_midi_destination_external(nullptr, 5, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_external_midi_dropped_count(engine, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine takes a clip event's UMP group from word0, not the struct field",
          "[c_api][engine]") {
  // SonareEngineMidiEvent.group is a separate field from word0 and a
  // zero-initialized struct leaves it 0, while the header documents packing the
  // group into word0 itself. A caller who follows the header and does not also
  // restate the group is the normal case, not a caller error, so the core reads
  // the group from word0. Observable here through hang-note bookkeeping: the
  // sequencer keys its active-note table on the UMP group, so a note-on and its
  // note-off read under two different groups never pair up and the note is still
  // considered sounding when the transport stops.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 5, 1) == SONARE_OK);

  constexpr uint32_t kGroup = 5u;
  const uint32_t note_on = midi1_word(0x9, 1, 64, 110) | (kGroup << 24u);
  const uint32_t note_off = midi1_word(0x8, 1, 64, 0) | (kGroup << 24u);
  // The pair is authored identically on the wire; only the redundant struct
  // field differs, and only because one event was written without restating it.
  const SonareEngineMidiEvent events[] = {
      {0, note_on, 0, 0, 0, 1, static_cast<uint8_t>(kGroup), 0, 0},
      {48, note_off, 0, 0, 0, 1, 0, 0, 0},
  };
  SonareEngineMidiClipSchedule clip{};
  clip.id = 7;
  clip.track_id = 5;
  clip.length_samples = 256;
  clip.destination_id = 5;
  clip.events = events;
  clip.event_count = 2;
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);

  std::array<SonareExternalMidiEvent, 32> drained{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
          SONARE_OK);
  // Both events are dispatched either way; the group only decides bookkeeping.
  REQUIRE(count == 2);
  REQUIRE((drained[0].bytes[0] & 0xF0u) == 0x90u);
  REQUIRE((drained[1].bytes[0] & 0xF0u) == 0x80u);

  // Stopping releases every note the sequencer still believes is sounding. The
  // pair above matched, so there is nothing left to release and the queue stays
  // empty; an unmatched note-on would surface here as an extra release.
  REQUIRE(sonare_engine_stop(engine, -1) == SONARE_OK);
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
          SONARE_OK);
  REQUIRE(count == 0);

  // An out-of-range group is still a malformed struct and is still rejected,
  // exactly like a non-zero `reserved`.
  SonareEngineMidiEvent bad = events[0];
  bad.group = 16;
  SonareEngineMidiClipSchedule bad_clip = clip;
  bad_clip.events = &bad;
  bad_clip.event_count = 1;
  REQUIRE(sonare_engine_set_midi_clips(engine, &bad_clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine reports external-destination table overflow", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // The slot table holds 16 distinct external destinations.
  for (uint32_t id = 0; id < 16; ++id) {
    REQUIRE(sonare_engine_set_midi_destination_external(engine, id, 1) == SONARE_OK);
  }
  // Re-marking an already-external destination is idempotent, not an overflow.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 3, 1) == SONARE_OK);
  // A 17th distinct destination overflows instead of silently routing internally.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 99, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Freeing a slot lets the next mark succeed.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 3, 0) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 99, 1) == SONARE_OK);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine_set_param_smoothing_ms validates its argument", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  REQUIRE(sonare_engine_set_param_smoothing_ms(engine, 0.0f) == SONARE_OK);
  REQUIRE(sonare_engine_set_param_smoothing_ms(engine, 75.0f) == SONARE_OK);
  REQUIRE(sonare_engine_set_param_smoothing_ms(engine, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_param_smoothing_ms(engine, std::nanf("")) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_param_smoothing_ms(nullptr, 10.0f) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine forwards MIDI clock/transport to the external queue", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(engine != nullptr);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 24000, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 120.0) == SONARE_OK);
  REQUIRE(sonare_engine_set_external_midi_clock_enabled(engine, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, 0) == SONARE_OK);

  std::vector<float> left(24000, 0.0f);
  std::vector<float> right(24000, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 24000) == SONARE_OK);

  // One Start plus 24 clock ticks (one every 1000 samples at 120 BPM).
  std::array<SonareExternalMidiEvent, 64> drained{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
          SONARE_OK);
  REQUIRE(count == 25);
  for (size_t i = 0; i < count; ++i) {
    REQUIRE(drained[i].destination_id == 0xFFFFFFFFu);
    REQUIRE(drained[i].byte_count == 1);
  }
  REQUIRE(drained[0].bytes[0] == 0xFAu);  // Start
  REQUIRE(drained[1].bytes[0] == 0xF8u);  // Clock

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine external MIDI stays monotonic across a loop wrap", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_tempo(engine, 120.0) == SONARE_OK);  // 1 beat = 24000 samples
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 5, 1) == SONARE_OK);

  // A note on at sample 0 and off at 12000, looping over the first beat
  // (ppq 0..1 == samples 0..24000). Each loop iteration re-dispatches both
  // events; the timeline sample wraps back to 0 while the device frame keeps
  // rising, so the drained device render frames must stay non-decreasing.
  const SonareEngineMidiEvent events[] = {
      {0, midi1_word(0x9, 1, 64, 110), 0, 0, 0, 1, 0, 0, 0},
      {12000, midi1_word(0x8, 1, 64, 0), 0, 0, 0, 1, 0, 0, 0},
  };
  SonareEngineMidiClipSchedule clip{};
  clip.id = 7;
  clip.track_id = 5;
  clip.length_samples = 24000;
  clip.destination_id = 5;
  clip.events = events;
  clip.event_count = 2;
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_set_loop(engine, 0.0, 1.0, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);

  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};

  // Process ~2.5 loop iterations and collect every drained channel-voice event.
  std::vector<int64_t> frames;
  std::array<SonareExternalMidiEvent, 64> drained{};
  for (int block = 0; block < 470; ++block) {
    REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
    size_t count = 0;
    REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
            SONARE_OK);
    for (size_t i = 0; i < count; ++i) {
      if (drained[i].destination_id == 5) frames.push_back(drained[i].render_frame);
    }
  }

  // We crossed the loop boundary at least twice, so both events re-fired.
  REQUIRE(frames.size() >= 4);
  for (size_t i = 1; i < frames.size(); ++i) {
    REQUIRE(frames[i] >= frames[i - 1]);  // monotonic device frames, no inversion
  }
  // The device frame keeps climbing past the loop length instead of resetting.
  REQUIRE(frames.back() >= 24000);

  sonare_engine_destroy(engine);
}
#endif

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
  REQUIRE(sonare_engine_sample_at_ppq(engine, 1.0e300, &sample) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_seek_ppq(engine, 1.0e300, -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_loop(engine, 0.0, 1.0e300, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_sample_at_ppq(engine, 1.0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  std::array<float, 4> clip_audio{1.0f, 0.0f, 0.0f, 0.0f};
  const float* clip_channels[] = {clip_audio.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.channels = clip_channels;
  clip.num_channels = 1;
  clip.num_samples = static_cast<int64_t>(clip_audio.size());
  clip.start_ppq = 1.0e300;
  clip.length_samples = 4;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

  const SonareEngineWarpAnchor anchors[] = {{0.0, 0.0}, {1.0e300, 1.0e300}};
  clip.start_ppq = 0.0;
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  clip.warp_anchors = anchors;
  clip.warp_anchor_count = 2;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_ERROR_INVALID_PARAMETER);

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

TEST_CASE("sonare_engine pushes the three per-note dimensions on both live paths",
          "[c_api][engine]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  // The two live families reach an instrument by different routes -- the
  // engine-owned input source drains a UMP queue at block start, the
  // destination family queues a command at a render frame -- so each dimension
  // is measured through both rather than assumed to follow its sibling. The
  // built-in synth answers all three: a bend moves the pitch, either pressure
  // raises the level.
  constexpr uint32_t kDestination = 3;
  constexpr int kBlock = 256;
  constexpr size_t kSettle = 4096;
  constexpr size_t kMeasured = 16384;
  constexpr double kC4Hz = 261.626;
  // The range this synth bends over, which the note is a whole tone above at
  // full positive bend.
  constexpr double kBentHz = 293.665;
  enum class Path { kInputSource, kDestination };

  auto render = [](Path path, uint16_t bend14, uint8_t channel_pressure,
                   uint8_t key_pressure) -> std::vector<float> {
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 16, 16) == SONARE_OK);
    SonareEngineBuiltinSynthConfig synth{};
    synth.gain = 0.4f;
    REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);
    const bool live_input = path == Path::kInputSource;
    if (live_input) {
      REQUIRE(sonare_engine_set_midi_input_source(engine, kDestination) == SONARE_OK);
      REQUIRE(sonare_engine_push_midi_input_note_on(engine, 0, 0, 60, 100, 0) == SONARE_OK);
    } else {
      REQUIRE(sonare_engine_push_midi_note_on(engine, kDestination, 0, 0, 60, 100, -1) ==
              SONARE_OK);
    }

    std::vector<float> settle(kSettle, 0.0f);
    std::vector<float> settle_right(kSettle, 0.0f);
    for (size_t at = 0; at < kSettle; at += kBlock) {
      float* channels[] = {settle.data() + at, settle_right.data() + at};
      REQUIRE(sonare_engine_process(engine, channels, 2, kBlock) == SONARE_OK);
    }

    // Sent after the note is sounding, which is both how a live gesture arrives
    // and what keeps the measurement independent of how two events at the same
    // timestamp are ordered.
    if (live_input) {
      REQUIRE(sonare_engine_push_midi_input_pitch_bend(engine, 0, 0, bend14, 0) == SONARE_OK);
      REQUIRE(sonare_engine_push_midi_input_channel_pressure(engine, 0, 0, channel_pressure, 0) ==
              SONARE_OK);
      REQUIRE(sonare_engine_push_midi_input_poly_pressure(engine, 0, 0, 60, key_pressure, 0) ==
              SONARE_OK);
    } else {
      REQUIRE(sonare_engine_push_midi_pitch_bend(engine, kDestination, 0, 0, bend14, -1) ==
              SONARE_OK);
      REQUIRE(sonare_engine_push_midi_channel_pressure(engine, kDestination, 0, 0, channel_pressure,
                                                       -1) == SONARE_OK);
      REQUIRE(sonare_engine_push_midi_poly_pressure(engine, kDestination, 0, 0, 60, key_pressure,
                                                    -1) == SONARE_OK);
    }

    std::vector<float> left(kMeasured, 0.0f);
    std::vector<float> right(kMeasured, 0.0f);
    for (size_t at = 0; at < kMeasured; at += kBlock) {
      float* channels[] = {left.data() + at, right.data() + at};
      REQUIRE(sonare_engine_process(engine, channels, 2, kBlock) == SONARE_OK);
    }
    sonare_engine_destroy(engine);
    return left;
  };

  for (const Path path : {Path::kInputSource, Path::kDestination}) {
    const std::vector<float> plain = render(path, 8192, 0, 0);
    const std::vector<float> bent = render(path, 16383, 0, 0);
    const std::vector<float> pressed = render(path, 8192, 127, 0);
    const std::vector<float> keyed = render(path, 8192, 0, 127);

    // The note sounds without any of the three, so no check below can pass on a
    // silent render.
    const float plain_peak = peak_abs(plain);
    REQUIRE(plain_peak > 0.01f);

    REQUIRE(sonare::test::fft_fundamental(plain, 0, kC4Hz) == Catch::Approx(kC4Hz).margin(6.0));
    REQUIRE(sonare::test::fft_fundamental(bent, 0, kBentHz) == Catch::Approx(kBentHz).margin(6.0));

    REQUIRE(peak_abs(pressed) > plain_peak * 1.5f);
    REQUIRE(peak_abs(keyed) > plain_peak * 1.5f);
  }
#else
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_pitch_bend(engine, 0, 0, 8192, 0) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_pitch_bend(engine, 3, 0, 0, 8192, -1) ==
          SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("sonare_engine refuses a per-note dimension outside its domain", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);

  // A bend is 14-bit, so the one value a 7-bit range check would let through is
  // exactly the one that has to be refused here.
  REQUIRE(sonare_engine_push_midi_pitch_bend(engine, 3, 0, 0, 16384, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_channel_pressure(engine, 3, 0, 16, 64, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_poly_pressure(engine, 3, 0, 0, 128, 64, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_poly_pressure(engine, 3, 0, 0, 60, 128, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_input_pitch_bend(nullptr, 0, 0, 8192, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

#if defined(SONARE_WITH_ARRANGEMENT)
  // An input-source push before the source is enabled is refused rather than
  // queued into a drain nothing reads.
  REQUIRE(sonare_engine_push_midi_input_pitch_bend(engine, 0, 0, 8192, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_input_channel_pressure(engine, 0, 0, 64, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_input_poly_pressure(engine, 0, 0, 60, 64, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
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
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"quantize_ppq\":1e300}") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":1e100}") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"chord_intervals\":[0,7.5]}") ==
          SONARE_ERROR_INVALID_PARAMETER);
  // The three velocity-curve fields are narrowed to float from a full JSON
  // double, so a value outside float range is refused rather than cast first
  // and inspected as +inf afterwards.
  for (const char* key : {"velocity_scale", "velocity_offset", "velocity_gamma"}) {
    const std::string too_large = std::string("{\"") + key + "\":1e39}";
    const std::string too_small = std::string("{\"") + key + "\":-1e39}";
    INFO(key);
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, too_large.c_str()) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, too_small.c_str()) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
  REQUIRE(
      sonare_engine_set_midi_fx(
          engine, 5, "{\"velocity_scale\":1.5,\"velocity_offset\":-4,\"velocity_gamma\":0.8}") ==
      SONARE_OK);
  REQUIRE(sonare_engine_clear_midi_fx(engine, 5) == SONARE_OK);
#else
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":12}") ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_clear_midi_fx(engine, 5) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("a built-in synth field the core would replace in silence is refused", "[c_api][synth]") {
  // The core reads a non-positive or non-finite field as "use the built-in
  // default" and reports nothing, so such a request came back as a successful
  // call configured with something the caller never asked for. The assertion is
  // on the CONFIGURATION IN FORCE, not on the absence of an error: a call that
  // merely succeeds passes against both behaviours.
  constexpr int kBlock = 128;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  // Four notes at once, so the summed peak reads the voice count rather than
  // the envelope: a polyphony of one can only sound one of them.
  const auto chord_peak = [](const SonareEngineBuiltinSynthConfig& synth, SonareError* err) {
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 16, 16) == SONARE_OK);
    *err = sonare_engine_set_builtin_instrument(engine, 7, &synth);
    float peak = 0.0f;
    if (*err == SONARE_OK) {
      for (int note : {60, 64, 67, 72}) {
        REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, static_cast<uint8_t>(note), 100,
                                                -1) == SONARE_OK);
      }
      std::vector<float> left(kBlock, 0.0f);
      std::vector<float> right(kBlock, 0.0f);
      float* channels[] = {left.data(), right.data()};
      for (int block = 0; block < 12; ++block) {
        REQUIRE(sonare_engine_process(engine, channels, 2, kBlock) == SONARE_OK);
        for (float sample : left) peak = std::max(peak, std::abs(sample));
      }
    }
    sonare_engine_destroy(engine);
    return peak;
  };

  SonareError err = SONARE_OK;
  SonareEngineBuiltinSynthConfig defaulted{};
  const float by_default = chord_peak(defaulted, &err);
  REQUIRE(err == SONARE_OK);
  REQUIRE(by_default > 0.0f);

  SECTION("the voice count really is what this measures") {
    // The graded control. Without it a polyphony assertion below could not tell
    // a stolen voice from an unchanged render.
    SonareEngineBuiltinSynthConfig one = defaulted;
    one.polyphony = 1;
    const float single = chord_peak(one, &err);
    REQUIRE(err == SONARE_OK);
    SonareEngineBuiltinSynthConfig two = defaulted;
    two.polyphony = 2;
    const float pair = chord_peak(two, &err);
    REQUIRE(err == SONARE_OK);
    // One voice is quieter than two, and quieter than the default's sixteen.
    // Two versus sixteen is NOT asserted: past the point where every note of the
    // chord sounds, adding voices changes phase relationships rather than adding
    // energy, and the summed peak stops being monotonic in the voice count.
    CHECK(single < pair);
    CHECK(single < by_default);
  }

  SECTION("a negative voice count is refused rather than resolved to the default") {
    for (int bad : {-1, -48}) {
      CAPTURE(bad);
      SonareEngineBuiltinSynthConfig config = defaulted;
      config.polyphony = bad;
      chord_peak(config, &err);
      CHECK(err == SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  SECTION("a negative or non-finite envelope field is refused") {
    for (float bad : {-1.0f, nan, inf, -inf}) {
      CAPTURE(bad);
      for (float SonareEngineBuiltinSynthConfig::*field :
           {&SonareEngineBuiltinSynthConfig::gain, &SonareEngineBuiltinSynthConfig::attack_ms,
            &SonareEngineBuiltinSynthConfig::decay_ms, &SonareEngineBuiltinSynthConfig::sustain,
            &SonareEngineBuiltinSynthConfig::release_ms}) {
        SonareEngineBuiltinSynthConfig config = defaulted;
        config.*field = bad;
        chord_peak(config, &err);
        CHECK(err == SONARE_ERROR_INVALID_PARAMETER);
      }
    }
  }

  SECTION("the sentinel and a legal value are both still accepted") {
    // Without this the refusals above are satisfied by an entry point that
    // rejects every configuration.
    SonareEngineBuiltinSynthConfig louder = defaulted;
    louder.gain = 0.8f;
    const float raised = chord_peak(louder, &err);
    CHECK(err == SONARE_OK);
    CHECK(raised > by_default);
    SonareEngineBuiltinSynthConfig sentinel = defaulted;
    sentinel.gain = 0.0f;
    sentinel.polyphony = 0;
    const float unchanged = chord_peak(sentinel, &err);
    CHECK(err == SONARE_OK);
    CHECK(unchanged == by_default);
  }
}
#endif
