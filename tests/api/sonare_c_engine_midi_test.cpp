/// @file sonare_c_engine_midi_test.cpp
/// @brief Engine C ABI MIDI input, built-in instrument and per-note dimensions.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "midi/ump.h"
#include "sonare_c_engine_test_helpers.h"
#include "support/alloc_guard.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_patch_common.h"
#endif

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
  clip.gain = 1.0f;
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
TEST_CASE("sonare_engine drains a MIDI 2.0 RPN as its four-message MIDI 1.0 sequence",
          "[c_api][engine]") {
  // A Registered Controller lowers to CC 101 / 100 / 6 / 38, so one record needs
  // a budget of four and a budget of three can never consume it.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 5, 1) == SONARE_OK);
  const sonare::midi::Ump rpn =
      sonare::midi::make_midi2_registered_controller(0, 1, 0, 0, (uint32_t{12} << 25));
  const SonareEngineMidiEvent events[] = {{0, rpn.words[0], rpn.words[1], 0, 0, 2, 0, 0, 0}};
  SonareEngineMidiClipSchedule clip{};
  clip.gain = 1.0f;
  clip.id = 7;
  clip.track_id = 5;
  clip.length_samples = 256;
  clip.destination_id = 5;
  clip.events = events;
  clip.event_count = 1;
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);

  std::array<SonareExternalMidiEvent, 4> drained{};
  size_t count = 0;
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), 3, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), 4, &count) == SONARE_OK);
  REQUIRE(count == 4);
  const uint8_t expected[4][2] = {{101, 0}, {100, 0}, {6, 12}, {38, 0}};
  for (size_t i = 0; i < 4; ++i) {
    INFO("message " << i);
    REQUIRE(drained[i].byte_count == 3);
    REQUIRE(drained[i].bytes[0] == 0xB1u);
    REQUIRE(drained[i].bytes[1] == expected[i][0]);
    REQUIRE(drained[i].bytes[2] == expected[i][1]);
  }
  sonare_engine_destroy(engine);
}

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
  clip.gain = 1.0f;
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
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), 3, &count) ==
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
  clip.gain = 1.0f;
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
  // pair above matched, so only its retained channel's controller reset remains;
  // an unmatched note-on would surface here as an extra note-off or reset group.
  REQUIRE(sonare_engine_stop(engine, -1) == SONARE_OK);
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  REQUIRE(sonare_engine_drain_external_midi(engine, drained.data(), drained.size(), &count) ==
          SONARE_OK);
  REQUIRE(count == 4);
  const std::array<std::array<uint8_t, 3>, 4> expected_resets{
      {{0xB1, 64, 0}, {0xB1, 121, 0}, {0xB1, 123, 0}, {0xE1, 0, 64}}};
  for (size_t i = 0; i < count; ++i) {
    REQUIRE(drained[i].destination_id == 5);
    REQUIRE(drained[i].byte_count == 3);
    for (size_t byte = 0; byte < 3; ++byte) {
      REQUIRE(drained[i].bytes[byte] == expected_resets[i][byte]);
    }
  }

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
  // The table the earlier marks filled is state; a missing engine is the argument.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 99, 1) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_engine_set_midi_destination_external(nullptr, 99, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Freeing a slot lets the next mark succeed.
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 3, 0) == SONARE_OK);
  REQUIRE(sonare_engine_set_midi_destination_external(engine, 99, 1) == SONARE_OK);

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine reports a full MIDI CC binding table as state", "[c_api][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  // 16 channels x 16 controllers fill the 256-entry table.
  for (uint8_t channel = 0; channel < 16; ++channel) {
    for (uint8_t controller = 0; controller < 16; ++controller) {
      REQUIRE(sonare_engine_bind_midi_cc(engine, channel, controller, 7, 0.0f, 1.0f) == SONARE_OK);
    }
  }
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 74, 7, 0.0f, 1.0f) == SONARE_ERROR_INVALID_STATE);
  SonareMidiCcBinding binding{};
  binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  binding.cc_number = 74;
  binding.channel = 0;
  binding.param_id = 7;
  binding.min_value = 0.0f;
  binding.max_value = 1.0f;
  REQUIRE(sonare_engine_bind_midi_cc_binding(engine, &binding) == SONARE_ERROR_INVALID_STATE);
  // An out-of-range channel stays an argument refusal on the full table, and a
  // rebind of a bound controller and channel replaces it in place.
  REQUIRE(sonare_engine_bind_midi_cc(engine, 16, 74, 7, 0.0f, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_bind_midi_cc(engine, 0, 0, 7, -1.0f, 1.0f) == SONARE_OK);

  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_engine reports a full instrument rack as state", "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  SonareEngineBuiltinSynthConfig synth{};
  for (uint32_t destination = 0; destination < 16; ++destination) {
    REQUIRE(sonare_engine_set_builtin_instrument(engine, destination, &synth) == SONARE_OK);
  }
  // The rack the earlier binds filled refuses a 17th destination; a bad config
  // is still the argument, and replacing a bound destination still fits.
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 99, &synth) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 99, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 3, &synth) == SONARE_OK);
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 16);

  sonare_engine_destroy(engine);
}
#endif

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
  clip.gain = 1.0f;
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
          SONARE_ERROR_INVALID_STATE);

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
          SONARE_ERROR_INVALID_STATE);
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

  REQUIRE(sonare_engine_push_midi_input_pitch_bend(nullptr, 0, 0, 8192, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

#if defined(SONARE_WITH_ARRANGEMENT)
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

  // An input-source push before the source is enabled is refused rather than
  // queued into a drain nothing reads.
  REQUIRE(sonare_engine_push_midi_input_pitch_bend(engine, 0, 0, 8192, 0) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_engine_push_midi_input_channel_pressure(engine, 0, 0, 64, 0) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_engine_push_midi_input_poly_pressure(engine, 0, 0, 60, 64, 0) ==
          SONARE_ERROR_INVALID_STATE);
#else
  // A build without the engine says so before it judges an argument.
  REQUIRE(sonare_engine_push_midi_pitch_bend(engine, 3, 0, 0, 16384, -1) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_input_pitch_bend(engine, 0, 0, 16384, 0) ==
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
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"quantize_ppq\":1e300}") ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":1e100}") ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"chord_intervals\":[0,7.5]}") ==
          SONARE_ERROR_INVALID_FORMAT);
  // The three velocity-curve fields are narrowed to float from a full JSON
  // double, so a value outside float range is refused rather than cast first
  // and inspected as +inf afterwards.
  for (const char* key : {"velocity_scale", "velocity_offset", "velocity_gamma"}) {
    const std::string too_large = std::string("{\"") + key + "\":1e39}";
    const std::string too_small = std::string("{\"") + key + "\":-1e39}";
    INFO(key);
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, too_large.c_str()) == SONARE_ERROR_INVALID_FORMAT);
    REQUIRE(sonare_engine_set_midi_fx(engine, 5, too_small.c_str()) == SONARE_ERROR_INVALID_FORMAT);
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
TEST_CASE("config_json exceeding the JSON byte budget is refused, not parsed",
          "[c_api][engine][resource]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);

  // One JSON string value past the 64 MiB config_json budget. The refusal has
  // to come from the byte precheck, not from the parser walking a document
  // this size into json::Value nodes first.
  const std::string oversized =
      "{\"pad\": \"" + std::string(64u * 1024u * 1024u + 1024u, 'a') + "\"}";
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, oversized.c_str()) == SONARE_ERROR_INVALID_FORMAT);

  // The same destination still accepts an ordinary config, so the refusal
  // above is the size and not some other change to midi_fx_chain_from_json.
  REQUIRE(sonare_engine_set_midi_fx(engine, 5, "{\"transpose_semitones\":12}") == SONARE_OK);
  REQUIRE(sonare_engine_clear_midi_fx(engine, 5) == SONARE_OK);

  sonare_engine_destroy(engine);
}

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

TEST_CASE("sonare_engine raw UMP push delivers a MIDI 2.0 note at full velocity width",
          "[c_api][engine][midi]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  // The built-in synth scales its output linearly with velocity, so a 16-bit
  // velocity between two adjacent 7-bit steps must land strictly between their
  // peaks; a push that truncated to 7 bits would equal one of them.
  constexpr uint32_t kDestination = 3;
  constexpr int kBlock = 256;
  enum class Path { kDestination, kInputSource };
  auto render = [](Path path, uint16_t velocity16) -> float {
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, kBlock, 16, 16) == SONARE_OK);
    SonareEngineBuiltinSynthConfig synth{};
    synth.gain = 0.4f;
    REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);
    const sonare::midi::Ump note = sonare::midi::make_midi2_note_on(0, 0, 60, velocity16);
    if (path == Path::kDestination) {
      REQUIRE(sonare_engine_push_midi_ump(engine, kDestination, note.words, 2, -1) == SONARE_OK);
    } else {
      REQUIRE(sonare_engine_set_midi_input_source(engine, kDestination) == SONARE_OK);
      REQUIRE(sonare_engine_push_midi_input_ump(engine, note.words, 2, 0) == SONARE_OK);
      size_t pending = 0;
      REQUIRE(sonare_engine_midi_input_pending_count(engine, &pending) == SONARE_OK);
      REQUIRE(pending == 1);
    }
    std::vector<float> left(kBlock * 8, 0.0f);
    std::vector<float> right(kBlock * 8, 0.0f);
    for (size_t at = 0; at < left.size(); at += kBlock) {
      float* channels[] = {left.data() + at, right.data() + at};
      REQUIRE(sonare_engine_process(engine, channels, 2, kBlock) == SONARE_OK);
    }
    sonare_engine_destroy(engine);
    return peak_abs(left);
  };

  const uint16_t lo = sonare::midi::scale_velocity_7_to_16(64);
  const uint16_t hi = sonare::midi::scale_velocity_7_to_16(65);
  const auto mid = static_cast<uint16_t>((uint32_t{lo} + hi) / 2u);
  for (const Path path : {Path::kDestination, Path::kInputSource}) {
    INFO("path " << static_cast<int>(path));
    const float lo_peak = render(path, lo);
    const float mid_peak = render(path, mid);
    const float hi_peak = render(path, hi);
    REQUIRE(lo_peak > 0.01f);
    REQUIRE(mid_peak > lo_peak);
    REQUIRE(mid_peak < hi_peak);
  }
#else
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  const sonare::midi::Ump note = sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u);
  REQUIRE(sonare_engine_push_midi_ump(engine, 3, note.words, 2, -1) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_input_ump(engine, note.words, 2, 0) ==
          SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("sonare_engine raw UMP push refuses data messages and mismatched word counts",
          "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_OK);
#endif

  const sonare::midi::Ump note = sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u);
  const uint32_t sysex7[2] = {0x30000000u, 0u};
  const uint32_t data128[4] = {0x50000000u, 0u, 0u, 0u};
  struct Refused {
    const char* what;
    const uint32_t* words;
    size_t count;
  };
  const Refused refused[] = {
      {"null words", nullptr, 2},
      {"zero words", note.words, 0},
      {"MIDI 2.0 message sent as one word", note.words, 1},
      {"MIDI 2.0 message sent as three words", note.words, 3},
      {"five words", data128, 5},
      {"SysEx7 (MT 0x3)", sysex7, 2},
      {"128-bit data (MT 0x5)", data128, 4},
  };
#if defined(SONARE_WITH_ARRANGEMENT)
  constexpr SonareError kRefusedShape = SONARE_ERROR_INVALID_PARAMETER;
#else
  constexpr SonareError kRefusedShape = SONARE_ERROR_NOT_SUPPORTED;
#endif
  for (const Refused& r : refused) {
    INFO(r.what);
    REQUIRE(sonare_engine_push_midi_ump(engine, 3, r.words, r.count, -1) == kRefusedShape);
    REQUIRE(sonare_engine_push_midi_input_ump(engine, r.words, r.count, 0) == kRefusedShape);
  }
  REQUIRE(sonare_engine_push_midi_ump(nullptr, 3, note.words, 2, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_input_ump(nullptr, note.words, 2, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

#if defined(SONARE_WITH_ARRANGEMENT)
  // Endpoint-level messages are accepted and discarded at the destination.
  const uint32_t stream[4] = {0xF0000101u, 0u, 0u, 0u};
  const uint32_t jr_clock[1] = {0x00101234u};
  REQUIRE(sonare_engine_push_midi_ump(engine, 3, stream, 4, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_ump(engine, 3, jr_clock, 1, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_ump(engine, stream, 4, 0) == SONARE_OK);

  // An input-source push is refused once the source is cleared, like its
  // scalar siblings.
  REQUIRE(sonare_engine_clear_midi_input_source(engine) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_ump(engine, note.words, 2, 0) ==
          SONARE_ERROR_INVALID_STATE);
#endif

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine raw UMP push reports a full queue as retryable back-pressure",
          "[c_api][engine][midi]") {
  // Without a process call nothing drains, so either the command queue or the
  // slot ring fills; both surface as the same transient error the scalar pushes
  // return for a full queue.
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
  const sonare::midi::Ump note = sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u);
  size_t accepted = 0;
  SonareError refused = SONARE_OK;
  for (size_t i = 0; i < 1024 && refused == SONARE_OK; ++i) {
    refused = sonare_engine_push_midi_ump(engine, 3, note.words, 2, -1);
    if (refused == SONARE_OK) ++accepted;
  }
  REQUIRE(accepted > 0);
  REQUIRE(refused == SONARE_ERROR_OUT_OF_MEMORY);
  sonare_engine_destroy(engine);
}
#endif

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
constexpr uint32_t kProgramDestination = 7;
constexpr int kProgramBlock = 256;
constexpr int kProgramBlocks = 8;

// Engine with a SoundFont-less Sf2Player (the GM/GS fallback floor) on kProgramDestination.
SonareRealtimeEngine* make_program_engine() {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, kProgramBlock, 16, 16) == SONARE_OK);
  SonareEngineSf2InstrumentConfig config{};
  config.struct_version = 4;
  REQUIRE(sonare_engine_set_sf2_instrument(engine, kProgramDestination, &config) == SONARE_OK);
  return engine;
}

std::vector<float> render_program_blocks(SonareRealtimeEngine* engine) {
  std::vector<float> left(kProgramBlock * kProgramBlocks, 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  for (size_t at = 0; at < left.size(); at += kProgramBlock) {
    float* channels[] = {left.data() + at, right.data() + at};
    REQUIRE(sonare_engine_process(engine, channels, 2, kProgramBlock) == SONARE_OK);
  }
  return left;
}

// Pushes @p select, then a note-on on channel 0, and renders.
template <typename Select>
std::vector<float> render_after(Select select) {
  SonareRealtimeEngine* engine = make_program_engine();
  select(engine);
  REQUIRE(sonare_engine_push_midi_note_on(engine, kProgramDestination, 0, 0, 60, 100, -1) ==
          SONARE_OK);
  std::vector<float> out = render_program_blocks(engine);
  sonare_engine_destroy(engine);
  return out;
}

void push_raw_program(SonareRealtimeEngine* engine, bool bank_valid, uint8_t msb) {
  const sonare::midi::Ump ump =
      sonare::midi::make_midi2_program_change(0, 0, 0, msb, 0, bank_valid);
  REQUIRE(sonare_engine_push_midi_ump(engine, kProgramDestination, ump.words, ump.word_count, -1) ==
          SONARE_OK);
}

void push_typed_program(SonareRealtimeEngine* engine, bool bank_valid, uint8_t msb) {
  REQUIRE(sonare_engine_push_midi_program(engine, kProgramDestination, 0, 0, 0, bank_valid ? 1 : 0,
                                          msb, 0, -1) == SONARE_OK);
}

// Roland GS DT1 "RX BANK SELECT" for part 1 (channel 0), address 40 11 23.
void push_rx_bank_select(SonareRealtimeEngine* engine, uint8_t value) {
  uint8_t sysex[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x23, value, 0x00, 0xF7};
  const int sum = 0x40 + 0x11 + 0x23 + value;
  sysex[9] = static_cast<uint8_t>((128 - sum % 128) % 128);
  REQUIRE(sonare_engine_push_midi_sysex(engine, kProgramDestination, sysex, sizeof(sysex), -1) ==
          SONARE_OK);
}
#endif

}  // namespace

TEST_CASE("sonare_engine typed program change equals the raw UMP and reaches the voice",
          "[c_api][engine][midi]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  const std::vector<float> none = render_after([](SonareRealtimeEngine*) {});
  const std::vector<float> bank0 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 0); });
  const std::vector<float> bank8 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 8); });
  const std::vector<float> raw8 =
      render_after([](SonareRealtimeEngine* e) { push_raw_program(e, true, 8); });
  REQUIRE(peak_abs(bank8) > 0.01f);
  CHECK(bank8 == raw8);
  CHECK(bank8 != bank0);
  CHECK(bank8 != none);
#else
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_program(engine, 3, 0, 0, 0, 1, 8, 0, -1) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_push_midi_input_program(engine, 0, 0, 0, 1, 8, 0, 0) ==
          SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
#endif
}

TEST_CASE("sonare_engine typed program change refuses out-of-domain arguments",
          "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_OK);
#endif

  struct Args {
    const char* what;
    uint8_t group, channel, program;
    int bank_valid;
    uint8_t msb, lsb;
  };
  const Args refused[] = {
      {"group 16", 16, 0, 0, 1, 0, 0},
      {"channel 16", 0, 16, 0, 1, 0, 0},
      {"program 128", 0, 0, 128, 1, 0, 0},
      {"bank MSB 128", 0, 0, 0, 1, 128, 0},
      {"bank LSB 128", 0, 0, 0, 1, 0, 128},
      {"MSB with bank-valid false", 0, 0, 0, 0, 1, 0},
      {"LSB with bank-valid false", 0, 0, 0, 0, 0, 1},
  };
#if defined(SONARE_WITH_ARRANGEMENT)
  constexpr SonareError kRefusedArg = SONARE_ERROR_INVALID_PARAMETER;
#else
  constexpr SonareError kRefusedArg = SONARE_ERROR_NOT_SUPPORTED;
#endif
  for (const Args& a : refused) {
    INFO(a.what);
    REQUIRE(sonare_engine_push_midi_program(engine, 3, a.group, a.channel, a.program, a.bank_valid,
                                            a.msb, a.lsb, -1) == kRefusedArg);
    REQUIRE(sonare_engine_push_midi_input_program(engine, a.group, a.channel, a.program,
                                                  a.bank_valid, a.msb, a.lsb, 0) == kRefusedArg);
  }
  REQUIRE(sonare_engine_push_midi_program(nullptr, 3, 0, 0, 0, 1, 8, 0, -1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_push_midi_input_program(nullptr, 0, 0, 0, 1, 8, 0, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

#if defined(SONARE_WITH_ARRANGEMENT)
  // Bank-valid false with a zero bank is the "keep the bank" form and is accepted.
  REQUIRE(sonare_engine_push_midi_program(engine, 3, 0, 0, 5, 0, 0, 0, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_program(engine, 0, 0, 5, 0, 0, 0, 0) == SONARE_OK);
  // A cleared input source refuses, like its siblings.
  REQUIRE(sonare_engine_clear_midi_input_source(engine) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_program(engine, 0, 0, 5, 1, 8, 0, 0) ==
          SONARE_ERROR_INVALID_STATE);
#endif
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine input-source program change reaches the destination voice",
          "[c_api][engine][midi]") {
  const std::vector<float> bank0 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 0); });
  const std::vector<float> bank8 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 8); });

  SonareRealtimeEngine* engine = make_program_engine();
  REQUIRE(sonare_engine_set_midi_input_source(engine, kProgramDestination) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_program(engine, 0, 0, 0, 1, 8, 0, 0) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_input_note_on(engine, 0, 0, 60, 100, 0) == SONARE_OK);
  const std::vector<float> via_input = render_program_blocks(engine);
  sonare_engine_destroy(engine);
  CHECK(via_input == bank8);
  CHECK(via_input != bank0);
}

TEST_CASE("sonare_engine program change at a render frame precedes a note at the same frame",
          "[c_api][engine][midi]") {
  const std::vector<float> bank8 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 8); });
  const std::vector<float> bank0 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 0); });

  SonareRealtimeEngine* engine = make_program_engine();
  REQUIRE(sonare_engine_push_midi_program(engine, kProgramDestination, 0, 0, 0, 1, 8, 0, 0) ==
          SONARE_OK);
  REQUIRE(sonare_engine_push_midi_note_on(engine, kProgramDestination, 0, 0, 60, 100, 0) ==
          SONARE_OK);
  const std::vector<float> same_frame = render_program_blocks(engine);
  sonare_engine_destroy(engine);
  CHECK(same_frame == bank8);
  CHECK(same_frame != bank0);
}

TEST_CASE("sonare_engine program change bank follows the live GS RX BANK SELECT switch",
          "[c_api][engine][midi]") {
  const std::vector<float> bank0 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 0); });
  const std::vector<float> bank8 =
      render_after([](SonareRealtimeEngine* e) { push_typed_program(e, true, 8); });
  REQUIRE(bank0 != bank8);

  // RX BANK SELECT off: the bank is ignored and the bank-0 voice sounds.
  const std::vector<float> rx_off = render_after([](SonareRealtimeEngine* e) {
    push_rx_bank_select(e, 0);
    push_typed_program(e, true, 8);
  });
  CHECK(rx_off == bank0);

  // RX BANK SELECT on: the bank-8 voice sounds.
  const std::vector<float> rx_on = render_after([](SonareRealtimeEngine* e) {
    push_rx_bank_select(e, 1);
    push_typed_program(e, true, 8);
  });
  CHECK(rx_on == bank8);
}
#endif

namespace {

// One call on the live push family, with the C function it names in a refusal.
struct LivePush {
  const char* function;
  std::function<SonareError(SonareRealtimeEngine*)> push;
};

const sonare::midi::Ump kLiveNote2 = sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u);

// The eight input-source pushes with in-domain arguments.
[[maybe_unused]] std::vector<LivePush> valid_input_pushes() {
  return {
      {"sonare_engine_push_midi_input_note_on",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_note_on(e, 0, 0, 60, 100, 0);
       }},
      {"sonare_engine_push_midi_input_note_off",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_note_off(e, 0, 0, 60, 0, 0);
       }},
      {"sonare_engine_push_midi_input_cc",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_cc(e, 0, 0, 7, 100, 0);
       }},
      {"sonare_engine_push_midi_input_pitch_bend",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_pitch_bend(e, 0, 0, 8192, 0);
       }},
      {"sonare_engine_push_midi_input_channel_pressure",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_channel_pressure(e, 0, 0, 64, 0);
       }},
      {"sonare_engine_push_midi_input_poly_pressure",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_poly_pressure(e, 0, 0, 60, 64, 0);
       }},
      {"sonare_engine_push_midi_input_ump",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_ump(e, kLiveNote2.words, 2, 0);
       }},
      {"sonare_engine_push_midi_input_program",
       [](SonareRealtimeEngine* e) {
         return sonare_engine_push_midi_input_program(e, 0, 0, 5, 1, 8, 0, 0);
       }},
  };
}

}  // namespace

TEST_CASE("sonare_engine live push family names the refused argument", "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);

  // Each value fits its C type and lies outside the MIDI domain, so only the
  // range rule can refuse it. The input source stays disabled: an argument is
  // judged before the source state.
  const uint32_t sysex7[2] = {0x30000000u, 0u};
  struct Refusal {
    LivePush call;
    const char* message;
  };
  const Refusal refusals[] = {
      {{"sonare_engine_push_midi_note_on",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_note_on(e, 3, 0, 16, 60, 100, -1);
        }},
       "channel must be in [0, 15]"},
      {{"sonare_engine_push_midi_note_off",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_note_off(e, 3, 0, 0, 60, 128, -1);
        }},
       "velocity must be in [0, 127]"},
      {{"sonare_engine_push_midi_cc",
        [](SonareRealtimeEngine* e) { return sonare_engine_push_midi_cc(e, 3, 0, 0, 7, 128, -1); }},
       "value must be in [0, 127]"},
      {{"sonare_engine_push_midi_pitch_bend",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_pitch_bend(e, 3, 0, 0, 16384, -1);
        }},
       "bend14 must be in [0, 16383]"},
      {{"sonare_engine_push_midi_channel_pressure",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_channel_pressure(e, 3, 0, 0, 128, -1);
        }},
       "pressure must be in [0, 127]"},
      {{"sonare_engine_push_midi_poly_pressure",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_poly_pressure(e, 3, 0, 0, 128, 64, -1);
        }},
       "note must be in [0, 127]"},
      {{"sonare_engine_push_midi_program",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_program(e, 3, 0, 0, 0, 1, 128, 0, -1);
        }},
       "bank_msb must be in [0, 127]"},
      {{"sonare_engine_push_midi_program",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_program(e, 3, 0, 0, 0, 0, 0, 1, -1);
        }},
       "bank MSB and LSB must be 0 when bank-valid is false"},
      {{"sonare_engine_push_midi_ump",
        [&sysex7](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_ump(e, 3, sysex7, 2, -1);
        }},
       "words do not form a pushable UMP message"},
      {{"sonare_engine_push_midi_input_note_on",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_note_on(e, 16, 0, 60, 100, 0);
        }},
       "group must be in [0, 15]"},
      {{"sonare_engine_push_midi_input_note_off",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_note_off(e, 0, 0, 128, 0, 0);
        }},
       "note must be in [0, 127]"},
      {{"sonare_engine_push_midi_input_cc",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_cc(e, 0, 0, 128, 0, 0);
        }},
       "controller must be in [0, 127]"},
      {{"sonare_engine_push_midi_input_pitch_bend",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_pitch_bend(e, 0, 0, 16384, 0);
        }},
       "bend14 must be in [0, 16383]"},
      {{"sonare_engine_push_midi_input_channel_pressure",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_channel_pressure(e, 0, 16, 64, 0);
        }},
       "channel must be in [0, 15]"},
      {{"sonare_engine_push_midi_input_poly_pressure",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_poly_pressure(e, 0, 0, 60, 128, 0);
        }},
       "pressure must be in [0, 127]"},
      {{"sonare_engine_push_midi_input_program",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_program(e, 0, 0, 128, 0, 0, 0, 0);
        }},
       "program must be in [0, 127]"},
      {{"sonare_engine_push_midi_input_program",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_program(e, 0, 0, 0, 1, 0, 128, 0);
        }},
       "bank_lsb must be in [0, 127]"},
      {{"sonare_engine_push_midi_input_ump",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_ump(e, kLiveNote2.words, 1, 0);
        }},
       "words do not form a pushable UMP message"},
  };
  for (const Refusal& r : refusals) {
    INFO(r.call.function << " / " << r.message);
#if defined(SONARE_WITH_ARRANGEMENT)
    REQUIRE(r.call.push(engine) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          std::string(r.call.function) + ": " + r.message);
#else
    // A build without the engine says so before it judges an argument.
    REQUIRE(r.call.push(engine) == SONARE_ERROR_NOT_SUPPORTED);
#endif
  }
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sonare_engine input-source pushes refuse a disabled source as invalid state",
          "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
  for (const bool was_enabled : {false, true}) {
    if (was_enabled) {
      REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_OK);
      REQUIRE(sonare_engine_clear_midi_input_source(engine) == SONARE_OK);
    }
    for (const LivePush& p : valid_input_pushes()) {
      INFO(p.function << (was_enabled ? " after clear" : " never enabled"));
      REQUIRE(p.push(engine) == SONARE_ERROR_INVALID_STATE);
      CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_STATE);
      CHECK(std::string(sonare_last_error_message()) ==
            std::string(p.function) + ": the MIDI input source is not enabled");
    }
  }
  sonare_engine_destroy(engine);
}

TEST_CASE("an SF2 instrument config outside its struct_version range is refused",
          "[c_api][engine][sf2]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
  SonareEngineSf2InstrumentConfig config{};
  for (const int version : {-1, -4, 5}) {
    INFO(version);
    config.struct_version = version;
    REQUIRE(sonare_engine_set_sf2_instrument(engine, 2, &config) == SONARE_ERROR_INVALID_PARAMETER);
  }
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 0);
  config.struct_version = 0;
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 2, &config) == SONARE_OK);
  sonare_engine_destroy(engine);

  // The project bounce spelling of the same config takes the same refusal.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectBounceOptions options{};
  options.total_frames = 64;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSf2InstrumentBinding binding{};
  binding.destination_id = 2;
  for (const int version : {-1, 5}) {
    INFO(version);
    binding.config.struct_version = version;
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
  }
  sonare_project_destroy(project);
}

// The failure injection lives in the allocator overrides the mixing test set links.
#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_engine live push refusals return their code when the heap fails",
          "[c_api][engine][midi]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
  // An argument refusal and a state refusal: both record their reason with the
  // first allocation failing, so neither may let a bad_alloc leave the C boundary.
  std::vector<std::pair<LivePush, SonareError>> cases = {
      {{"sonare_engine_push_midi_note_on",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_note_on(e, 3, 0, 16, 60, 100, -1);
        }},
       SONARE_ERROR_INVALID_PARAMETER},
      {{"sonare_engine_push_midi_input_cc",
        [](SonareRealtimeEngine* e) {
          return sonare_engine_push_midi_input_cc(e, 0, 0, 128, 0, 0);
        }},
       SONARE_ERROR_INVALID_PARAMETER}};
  for (const LivePush& p : valid_input_pushes()) cases.push_back({p, SONARE_ERROR_INVALID_STATE});
  for (const auto& [push, expected] : cases) {
    INFO(push.function);
    SonareError result = SONARE_OK;
    {
      sonare::test::AllocationFailureAtGuard fail_first(1);
      result = push.push(engine);
    }
    REQUIRE(result == expected);
    CHECK(sonare_last_error_code() == expected);
  }
  sonare_engine_destroy(engine);
}
#endif

TEST_CASE("sonare_engine input-source pushes report a full queue as out of memory",
          "[c_api][engine][midi]") {
  // Nothing drains without a process call, so the bounded input queue fills.
  for (const LivePush& p : valid_input_pushes()) {
    INFO(p.function);
    SonareRealtimeEngine* engine = nullptr;
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 16, 16) == SONARE_OK);
    REQUIRE(sonare_engine_set_midi_input_source(engine, 3) == SONARE_OK);
    size_t accepted = 0;
    SonareError refused = SONARE_OK;
    for (size_t i = 0; i < 4096 && refused == SONARE_OK; ++i) {
      refused = p.push(engine);
      if (refused == SONARE_OK) ++accepted;
    }
    CHECK(accepted > 0);
    CHECK(refused == SONARE_ERROR_OUT_OF_MEMORY);
    sonare_engine_destroy(engine);
  }
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("synth patch refuses an unknown routing ordinal, a bad depth or an unknown sample enum",
          "[c_api][engine][synth_patch]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 32, 16) == SONARE_OK);

  const auto base = [] {
    SonareSynthPatch patch{};
    patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    patch.num_mod_routings = 1;
    patch.mod_routings[0] = {1, 1, 100.0f};
    return patch;
  };
  const auto refused = [&](const SonareSynthPatch& patch) {
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    CHECK_FALSE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
    CHECK(error != nullptr);
    CHECK(sonare_engine_set_synth_instrument(engine, 1, &patch) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).size() > 0);
  };

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (int source : {-1, SONARE_SYNTH_MOD_SOURCE_COUNT, 99}) {
    CAPTURE(source);
    SonareSynthPatch patch = base();
    patch.mod_routings[0].source = source;
    refused(patch);
  }
  for (int version : {-1, SONARE_SYNTH_PATCH_STRUCT_VERSION + 1}) {
    CAPTURE(version);
    SonareSynthPatch patch = base();
    patch.struct_version = version;
    refused(patch);
  }
  for (int destination : {-1, SONARE_SYNTH_MOD_DESTINATION_COUNT, 99}) {
    CAPTURE(destination);
    SonareSynthPatch patch = base();
    patch.mod_routings[0].destination = destination;
    refused(patch);
  }
  for (float depth : {nan, inf, -inf, 9601.0f, -9601.0f}) {
    CAPTURE(depth);
    SonareSynthPatch patch = base();
    patch.mod_routings[0].depth = depth;
    refused(patch);
  }
  // Refused whichever engine the patch selects, not only when the sample engine reads them.
  for (int engine_mode : {SONARE_SYNTH_ENGINE_DEFAULT, SONARE_SYNTH_ENGINE_SAMPLE}) {
    for (int value : {-1, SONARE_SAMPLE_LOOP_MODE_COUNT}) {
      CAPTURE(engine_mode, value);
      SonareSynthPatch patch = base();
      patch.engine_mode = engine_mode;
      patch.sample_loop = value;
      refused(patch);
    }
    for (int value : {-1, SONARE_SAMPLE_KEY_TRACK_COUNT}) {
      CAPTURE(engine_mode, value);
      SonareSynthPatch patch = base();
      patch.engine_mode = engine_mode;
      patch.sample_key_track = value;
      refused(patch);
    }
  }

  // The edges of every range are applied exactly as given.
  SonareSynthPatch patch = base();
  patch.num_mod_routings = 2;
  patch.mod_routings[0] = {SONARE_SYNTH_MOD_SOURCE_COUNT - 1,
                           SONARE_SYNTH_MOD_DESTINATION_COUNT - 1, 9600.0f};
  patch.mod_routings[1] = {1, 1, -9600.0f};
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
  const auto& routes = cfg.patch.mod_matrix.routes;
  CHECK(static_cast<int>(routes[0].source) == SONARE_SYNTH_MOD_SOURCE_COUNT - 1);
  CHECK(static_cast<int>(routes[0].destination) == SONARE_SYNTH_MOD_DESTINATION_COUNT - 1);
  CHECK(routes[0].depth == 9600.0f);
  CHECK(static_cast<int>(routes[1].source) == 1);
  CHECK(routes[1].depth == -9600.0f);
  REQUIRE(sonare_engine_set_synth_instrument(engine, 1, &patch) == SONARE_OK);

  SonareSynthPatch sample = base();
  sample.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  sample.sample_loop = SONARE_SAMPLE_LOOP_KEY_DOWN;
  sample.sample_key_track = SONARE_SAMPLE_KEY_TRACK_OFF;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(sample, &cfg, &error));
  CHECK(cfg.patch.sample.loop_override == 3);
  CHECK_FALSE(cfg.patch.sample.key_track);
  sonare_engine_destroy(engine);
}

TEST_CASE("sonare_sample_bank_add_sample refuses a loop_mode outside SoundFont sampleModes",
          "[c_api][sample_bank]") {
  SonareSampleBank* bank = sonare_sample_bank_create();
  REQUIRE(bank != nullptr);
  const std::vector<float> pcm(4096, 0.25f);
  size_t count = 0;
  for (int loop_mode : {-1, 4, 7}) {
    CAPTURE(loop_mode);
    SonareSampleDesc desc{};
    desc.loop_start = 100;
    desc.loop_end = 2000;
    desc.loop_mode = loop_mode;
    uint32_t index = 77;
    REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, &index) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
    REQUIRE(count == 0);
  }
  for (int loop_mode : {0, 1, 2, 3}) {
    CAPTURE(loop_mode);
    SonareSampleDesc desc{};
    desc.loop_start = 100;
    desc.loop_end = 2000;
    desc.loop_mode = loop_mode;
    uint32_t index = 0;
    REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, &index) ==
            SONARE_OK);
  }
  REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
  REQUIRE(count == 4);
  sonare_sample_bank_destroy(bank);
}
#endif
