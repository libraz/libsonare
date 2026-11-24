/// @file numeric_domain_rows_project_test.cpp
/// @brief The project, editing, MIDI, mixing and vocal-edit partition of the numeric-domain table
///        against its C entries.

#include "c_api/numeric_domain_rows_project.h"

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "numeric_domain_rows_support.h"
#include "util/sha256.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
#include "mixing/api/scene.h"
#include "support/project_bounce.h"
#endif

namespace {

using namespace sonare::test::numeric_domains;
using sonare_c_detail::kProjectDomainRows;
using sonare_c_detail::numeric_domain_table;

[[maybe_unused]] uint8_t to_u8(double value) {
  return static_cast<uint8_t>(static_cast<int>(value));
}
[[maybe_unused]] uint16_t to_u16(double value) {
  return static_cast<uint16_t>(static_cast<int>(value));
}
[[maybe_unused]] uint32_t to_u32(double value) {
  return static_cast<uint32_t>(static_cast<long long>(value));
}
[[maybe_unused]] int64_t to_i64(double value) { return static_cast<int64_t>(value); }
[[maybe_unused]] size_t to_count(double value) {
  return static_cast<size_t>(static_cast<long long>(value));
}

SonareError probe_scale_mask_for_mode(const std::string& param, double value) {
  int root = 0;
  if (is_param(param, "root")) root = static_cast<int>(value);
  uint16_t mask = 0;
  return sonare_scale_mask_for_mode(root, SONARE_MODE_MAJOR, &mask);
}

SonareError probe_extract_percussive_events(const std::string& param, double value) {
  SonarePercussiveEventConfig config{};
  sonare_percussive_event_config_init(&config);
  if (is_param(param, "config.onset_wait")) config.onset_wait = static_cast<int32_t>(value);
  const std::vector<float>& samples = analysis_samples();
  SonarePercussiveEventsResult result{};
  const SonareError err = sonare_extract_percussive_events(samples.data(), samples.size(),
                                                           kAnalysisRate, &config, &result);
  sonare_free_percussive_events_result(&result);
  return err;
}

#if defined(SONARE_WITH_PITCH_EDITOR)

struct ScaleArgs {
  int root = 0;
  uint16_t mode_mask = 0b101010110101;
  int pitch_class = 0;

  void set(const std::string& param, double value) {
    if (is_param(param, "root")) root = static_cast<int>(value);
    if (is_param(param, "mode_mask")) mode_mask = to_u16(value);
    if (is_param(param, "pitch_class")) pitch_class = static_cast<int>(value);
  }
};

SonareError probe_scale_quantize_midi(const std::string& param, double value) {
  ScaleArgs a;
  a.set(param, value);
  float out = 0.0f;
  return sonare_scale_quantize_midi(a.root, a.mode_mask, std::nanf(""), 60.0f, &out);
}

SonareError probe_scale_correction_semitones(const std::string& param, double value) {
  ScaleArgs a;
  a.set(param, value);
  float out = 0.0f;
  return sonare_scale_correction_semitones(a.root, a.mode_mask, std::nanf(""), 60.0f, &out);
}

SonareError probe_scale_pitch_class_enabled(const std::string& param, double value) {
  ScaleArgs a;
  a.set(param, value);
  int enabled = 0;
  return sonare_scale_pitch_class_enabled(a.root, a.mode_mask, a.pitch_class, &enabled);
}

SonareError probe_vocal_session_create(const std::string& param, double value) {
  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  if (is_param(param, "options.frame_length_samples")) options.frame_length_samples = to_u32(value);
  if (is_param(param, "options.hop_length_samples")) options.hop_length_samples = to_u32(value);
  const std::vector<float>& samples = analysis_samples();
  SonareVocalSession* session = nullptr;
  const SonareError err = sonare_vocal_session_create(
      samples.data(), static_cast<int64_t>(samples.size()), 1, kAnalysisRate, &options, &session);
  sonare_vocal_session_destroy(session);
  return err;
}

#endif  // SONARE_WITH_PITCH_EDITOR

#if defined(SONARE_WITH_MIXING)

SonareError probe_mixer_create(const std::string& param, double value) {
  int sample_rate = 48000;
  int max_block_size = 256;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  if (is_param(param, "max_block_size")) max_block_size = static_cast<int>(value);
  SonareMixer* mixer = nullptr;
  const SonareError err = sonare_mixer_create(sample_rate, max_block_size, &mixer);
  sonare_mixer_destroy(mixer);
  return err;
}

#if defined(SONARE_WITH_GRAPH)

/// A mixer built from a one-strip scene with a compressor insert and a master bus.
struct SceneMixer {
  SonareMixer* mixer = nullptr;
  SonareStrip* strip = nullptr;

  SceneMixer() {
    sonare::mixing::api::Scene scene;
    sonare::mixing::api::Strip strip_config;
    strip_config.id = "comp";
    sonare::mixing::api::Insert insert;
    insert.slot = sonare::mixing::api::InsertSlot::PreFader;
    insert.processor_name = "dynamics.compressor";
    insert.params_json = "{\"thresholdDb\":-3.0,\"ratio\":8.0}";
    strip_config.inserts.push_back(insert);
    scene.strips.push_back(strip_config);
    scene.buses.push_back({"master", "master"});
    scene.connections.push_back({"comp", "master"});
    const std::string json = sonare::mixing::api::scene_to_json(scene);
    mixer = sonare_mixer_from_scene_json(json.c_str(), 48000, 256);
    REQUIRE(mixer != nullptr);
    strip = sonare_mixer_strip_by_id(mixer, "comp");
    REQUIRE(strip != nullptr);
  }
  ~SceneMixer() { sonare_mixer_destroy(mixer); }
  SceneMixer(const SceneMixer&) = delete;
  SceneMixer& operator=(const SceneMixer&) = delete;
};

SonareError probe_strip_fader(const std::string& param, double value) {
  SceneMixer scene;
  const int curve = is_param(param, "curve") ? static_cast<int>(value) : 0;
  return sonare_strip_schedule_fader_automation(scene.strip, 0, 0.0f, curve);
}

SonareError probe_strip_pan(const std::string& param, double value) {
  SceneMixer scene;
  const int curve = is_param(param, "curve") ? static_cast<int>(value) : 0;
  return sonare_strip_schedule_pan_automation(scene.strip, 0, 0.0f, curve);
}

SonareError probe_strip_width(const std::string& param, double value) {
  SceneMixer scene;
  const int curve = is_param(param, "curve") ? static_cast<int>(value) : 0;
  return sonare_strip_schedule_width_automation(scene.strip, 0, 1.0f, curve);
}

SonareError probe_strip_send(const std::string& param, double value) {
  SceneMixer scene;
  size_t index = 0;
  REQUIRE(sonare_strip_add_send(scene.strip, "send", "master", 0.0f, 0, &index) == SONARE_OK);
  const int curve = is_param(param, "curve") ? static_cast<int>(value) : 0;
  return sonare_strip_schedule_send_automation(scene.strip, index, 0, 0.0f, curve);
}

SonareError probe_strip_insert(const std::string& param, double value) {
  SceneMixer scene;
  const int curve = is_param(param, "curve") ? static_cast<int>(value) : 0;
  const unsigned int param_id = is_param(param, "param_id") ? to_u32(value) : 0u;
  return sonare_strip_schedule_insert_automation(scene.strip, 0, param_id, 0, -24.0f, curve);
}

#endif  // SONARE_WITH_GRAPH
#endif  // SONARE_WITH_MIXING

#if defined(SONARE_WITH_MIXING_ASSISTANT)

SonareError probe_validate_mixing_assistant_config(const std::string&, double value) {
  return sonare_validate_mixing_assistant_config(static_cast<int>(value), nullptr, 0);
}

SonareError probe_mixing_assistant_suggest(const std::string& param, double value) {
  const float* left[] = {nullptr};
  const char* ids[] = {"track"};
  const size_t lengths[] = {0};
  int rates[] = {48000};
  int sample_rate = 48000;
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  if (is_param(param, "track_sample_rates[]")) rates[0] = static_cast<int>(value);
  char* json = nullptr;
  const SonareError err = sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths,
                                                          rates, 1, sample_rate, nullptr, 0, &json);
  sonare_free_string(json);
  return err;
}

#endif  // SONARE_WITH_MIXING_ASSISTANT

#if defined(SONARE_WITH_ARRANGEMENT)

/// A project owned for the length of one probe.
struct Project {
  SonareProject* handle = nullptr;
  Project() { REQUIRE(sonare_project_create(&handle) == SONARE_OK); }
  ~Project() { sonare_project_destroy(handle); }
  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;

  uint32_t add_audio_track() {
    SonareProjectTrackDesc desc{};
    desc.kind = SONARE_TRACK_AUDIO;
    desc.name = "track";
    uint32_t id = 0;
    REQUIRE(sonare_project_add_track(handle, &desc, &id) == SONARE_OK);
    return id;
  }

  uint32_t add_audio_clip(double length_ppq, const float* samples, size_t frames, int rate) {
    SonareProjectClipDesc desc{};
    desc.track_id = add_audio_track();
    desc.length_ppq = length_ppq;
    desc.gain = 1.0f;
    desc.audio_interleaved = samples;
    desc.audio_frames = static_cast<int64_t>(frames);
    desc.audio_channels = 1;
    desc.audio_sample_rate = rate;
    uint32_t id = 0;
    REQUIRE(sonare_project_add_clip(handle, &desc, &id) == SONARE_OK);
    return id;
  }

  uint32_t add_audio_clip() {
    const auto& samples = analysis_samples();
    return add_audio_clip(4.0, samples.data(), samples.size(), kAnalysisRate);
  }

  uint32_t add_midi_clip() {
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(handle, 0.0, 4.0, &track, &clip) == SONARE_OK);
    return clip;
  }
};

// ---- MIDI event builders ----------------------------------------------------------------

/// Every argument a MIDI builder takes, at a value its checks accept.
struct MidiArgs {
  double ppq = 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 60;
  uint8_t velocity = 100;
  uint8_t controller = 1;
  uint8_t value = 1;
  uint8_t pressure = 1;
  uint8_t program = 1;
  uint8_t bank_msb = 0;
  uint8_t bank_lsb = 0;
  uint8_t bank = 0;
  uint8_t index = 0;
  uint16_t bend = 0;

  void set(const std::string& param, double v) {
    if (is_param(param, "ppq")) ppq = v;
    if (is_param(param, "group")) group = to_u8(v);
    if (is_param(param, "channel")) channel = to_u8(v);
    if (is_param(param, "note")) note = to_u8(v);
    if (is_param(param, "velocity")) velocity = to_u8(v);
    if (is_param(param, "controller")) controller = to_u8(v);
    if (is_param(param, "value")) value = to_u8(v);
    if (is_param(param, "pressure")) pressure = to_u8(v);
    if (is_param(param, "program")) program = to_u8(v);
    if (is_param(param, "bank_msb")) bank_msb = to_u8(v);
    if (is_param(param, "bank_lsb")) bank_lsb = to_u8(v);
    if (is_param(param, "bank")) bank = to_u8(v);
    if (is_param(param, "index")) index = to_u8(v);
    if (is_param(param, "bend")) bend = to_u16(v);
  }
};

#define MIDI_BUILDER_PROBE(name, call)                               \
  SonareError probe_##name(const std::string& param, double value) { \
    MidiArgs a;                                                      \
    a.set(param, value);                                             \
    SonareMidiEventPod out{};                                        \
    return call;                                                     \
  }

MIDI_BUILDER_PROBE(midi_note_on,
                   sonare_midi_note_on(a.ppq, a.group, a.channel, a.note, a.velocity, &out))
MIDI_BUILDER_PROBE(midi_note_off,
                   sonare_midi_note_off(a.ppq, a.group, a.channel, a.note, a.velocity, &out))
MIDI_BUILDER_PROBE(midi_cc, sonare_midi_cc(a.ppq, a.group, a.channel, a.controller, a.value, &out))
MIDI_BUILDER_PROBE(midi_poly_pressure,
                   sonare_midi_poly_pressure(a.ppq, a.group, a.channel, a.note, a.pressure, &out))
MIDI_BUILDER_PROBE(midi_program, sonare_midi_program(a.ppq, a.group, a.channel, a.program, &out))
MIDI_BUILDER_PROBE(midi_channel_pressure,
                   sonare_midi_channel_pressure(a.ppq, a.group, a.channel, a.pressure, &out))
MIDI_BUILDER_PROBE(midi_pitch_bend, sonare_midi_pitch_bend(a.ppq, a.group, a.channel, a.bend, &out))
MIDI_BUILDER_PROBE(midi2_note_on,
                   sonare_midi2_note_on(a.ppq, a.group, a.channel, a.note, 1000, 0, 0, &out))
MIDI_BUILDER_PROBE(midi2_note_off,
                   sonare_midi2_note_off(a.ppq, a.group, a.channel, a.note, 1000, &out))
MIDI_BUILDER_PROBE(midi2_cc, sonare_midi2_cc(a.ppq, a.group, a.channel, a.controller, 0, &out))
MIDI_BUILDER_PROBE(midi2_poly_pressure,
                   sonare_midi2_poly_pressure(a.ppq, a.group, a.channel, a.note, 0, &out))
MIDI_BUILDER_PROBE(midi2_channel_pressure,
                   sonare_midi2_channel_pressure(a.ppq, a.group, a.channel, 0, &out))
MIDI_BUILDER_PROBE(midi2_pitch_bend, sonare_midi2_pitch_bend(a.ppq, a.group, a.channel, 0, &out))
MIDI_BUILDER_PROBE(midi2_program, sonare_midi2_program(a.ppq, a.group, a.channel, a.program, 1,
                                                       a.bank_msb, a.bank_lsb, &out))
MIDI_BUILDER_PROBE(midi2_registered_controller,
                   sonare_midi2_registered_controller(a.ppq, a.group, a.channel, a.bank, a.index, 0,
                                                      &out))
MIDI_BUILDER_PROBE(midi2_assignable_controller,
                   sonare_midi2_assignable_controller(a.ppq, a.group, a.channel, a.bank, a.index, 0,
                                                      &out))
MIDI_BUILDER_PROBE(midi2_relative_registered_controller,
                   sonare_midi2_relative_registered_controller(a.ppq, a.group, a.channel, a.bank,
                                                               a.index, 0, &out))
MIDI_BUILDER_PROBE(midi2_relative_assignable_controller,
                   sonare_midi2_relative_assignable_controller(a.ppq, a.group, a.channel, a.bank,
                                                               a.index, 0, &out))
MIDI_BUILDER_PROBE(midi2_registered_per_note_controller,
                   sonare_midi2_registered_per_note_controller(a.ppq, a.group, a.channel, a.note,
                                                               a.index, 0, &out))
MIDI_BUILDER_PROBE(midi2_assignable_per_note_controller,
                   sonare_midi2_assignable_per_note_controller(a.ppq, a.group, a.channel, a.note,
                                                               a.index, 0, &out))
MIDI_BUILDER_PROBE(midi2_per_note_pitch_bend,
                   sonare_midi2_per_note_pitch_bend(a.ppq, a.group, a.channel, a.note, 0, &out))
MIDI_BUILDER_PROBE(midi2_per_note_management,
                   sonare_midi2_per_note_management(a.ppq, a.group, a.channel, a.note, 0, 0, &out))

#undef MIDI_BUILDER_PROBE

template <auto Lookup>
SonareError probe_midi_lookup(const std::string&, double value) {
  (void)Lookup(static_cast<int>(value));
  return sonare_last_error_code();
}

template <auto Lookup>
SonareError probe_midi_bank_lookup(const std::string& param, double value) {
  const int ordinal = static_cast<int>(value);
  const bool bank = is_param(param, "bank_lsb");
  (void)Lookup(bank ? ordinal : 0, bank ? 0 : ordinal);
  return sonare_last_error_code();
}

SonareError probe_midi_bank_program(const std::string& param, double value) {
  const double ppq = is_param(param, "ppq") ? value : 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  int bank_msb = 0;
  int bank_lsb = 0;
  int program = 0;
  if (is_param(param, "group")) group = to_u8(value);
  if (is_param(param, "channel")) channel = to_u8(value);
  if (is_param(param, "bank_msb")) bank_msb = static_cast<int>(value);
  if (is_param(param, "bank_lsb")) bank_lsb = static_cast<int>(value);
  if (is_param(param, "program")) program = static_cast<int>(value);
  SonareMidiEventPod out[3] = {};
  size_t count = 0;
  return sonare_midi_bank_program(ppq, group, channel, bank_msb, bank_lsb, program, out, 3, &count);
}

SonareMidiEventPod note_on_pod() {
  SonareMidiEventPod pod{};
  pod.word0 = 0x20903C64u;
  return pod;
}

/// One note-on the event-list probes hand over; @p param sets its position to @p value.
SonareMidiEventPod event_for(const std::string& param, double value) {
  SonareMidiEventPod event = note_on_pod();
  if (is_param(param, "events[].ppq") || is_param(param, "event.ppq")) event.ppq = value;
  return event;
}

/// The event count a list probe passes: the probed count, one for a probed position, else none.
size_t event_count_for(const std::string& param, double value) {
  if (is_param(param, "count")) return to_count(value);
  return is_param(param, "events[].ppq") ? 1 : 0;
}

SonareError probe_midi_route_events(const std::string& param, double value) {
  const SonareMidiEventPod event = event_for(param, value);
  const size_t count = event_count_for(param, value);
  size_t out_count = 0;
  return sonare_midi_route_events(&event, count, nullptr, nullptr, 0, &out_count, nullptr, nullptr);
}

SonareMidiEventPod cc_pod(uint8_t cc, uint8_t amount) {
  SonareMidiEventPod event{};
  REQUIRE(sonare_midi_cc(0.0, 0, 0, cc, amount, &event) == SONARE_OK);
  return event;
}

SonareError probe_midi_cc_learn(const std::string& param, double value) {
  constexpr uint8_t kProbeController = 74;
  const size_t count = is_param(param, "count") ? to_count(value) : 2;
  SonareMidiEventPod events[] = {cc_pod(kProbeController, 0), cc_pod(kProbeController, 127)};
  if (is_param(param, "events[].ppq")) events[0].ppq = value;
  uint32_t param_id = 1;
  uint8_t min_movement = 1;
  if (is_param(param, "param_id")) param_id = to_u32(value);
  if (is_param(param, "min_movement")) min_movement = to_u8(value);
  SonareMidiCcBinding binding{};
  return sonare_midi_cc_learn(events, count, param_id, 0.0f, 1.0f, min_movement, &binding);
}

/// A binding every check accepts; @p param sets its cc_number or param_id to @p value.
SonareMidiCcBinding binding_for(const std::string& param, double value) {
  SonareMidiCcBinding binding{};
  binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  binding.cc_number = 1;
  binding.channel = 0;
  binding.param_id = 1;
  binding.min_value = 0.0f;
  binding.max_value = 1.0f;
  if (is_param(param, "bindings[].param_id")) binding.param_id = to_u32(value);
  if (is_param(param, "bindings[].cc_number")) {
    // The 14-bit controller number is the narrower domain, so the binding is a 14-bit one; a
    // value below the domain is probed just past its upper bound, since a byte cannot hold -1
    // below the 7-bit domain without being refused as past 127 instead.
    binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
    binding.cc_number = value < 0.0 ? static_cast<uint8_t>(32) : to_u8(value);
    binding.cc_lsb_number = static_cast<uint8_t>(binding.cc_number + 32u);
  }
  return binding;
}

SonareError probe_midi_cc_to_breakpoint(const std::string& param, double value) {
  const SonareMidiCcBinding binding = binding_for(param, value);
  SonareMidiEventPod event = cc_pod(binding.cc_number, 64);
  if (is_param(param, "event.ppq")) event.ppq = value;
  SonareAutomationPoint point{};
  return sonare_midi_cc_to_breakpoint(&binding, 1, &event, &point);
}

SonareError probe_midi_param_to_cc(const std::string& param, double value) {
  const SonareMidiCcBinding binding = binding_for(param, value);
  uint32_t param_id = 1;
  uint8_t group = 0;
  if (is_param(param, "param_id")) param_id = to_u32(value);
  if (is_param(param, "group")) group = to_u8(value);
  const double ppq = is_param(param, "ppq") ? value : 0.0;
  SonareMidiEventPod out{};
  return sonare_midi_param_to_cc(&binding, 1, param_id, 0.5f, group, ppq, &out);
}

SonareError probe_project_set_midi_events(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  const SonareMidiEventPod event = event_for(param, value);
  const size_t count = event_count_for(param, value);
  return sonare_project_set_midi_events(project.handle, clip, &event, count);
}

SonareError probe_project_set_program(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  uint8_t group = 0;
  uint8_t channel = 0;
  int program = 0;
  if (is_param(param, "group")) group = to_u8(value);
  if (is_param(param, "channel")) channel = to_u8(value);
  if (is_param(param, "program")) program = static_cast<int>(value);
  return sonare_project_set_program(project.handle, clip, group, channel, program, -1);
}

// ---- Project edits ----------------------------------------------------------------------

/// A lane desc every check accepts; @p param sets its target or its one breakpoint's position.
SonareAutomationLaneDesc lane_for(const std::string& param, double value,
                                  SonareAutomationPoint* point) {
  SonareAutomationLaneDesc desc;
  sonare_automation_lane_desc_init(&desc);
  desc.target_param_id = 1;
  if (is_param(param, "desc.target_param_id")) desc.target_param_id = to_u32(value);
  if (is_param(param, "desc.points[].ppq")) {
    *point = SonareAutomationPoint{value, 0.0f, 0};
    desc.points = point;
    desc.point_count = 1;
  }
  return desc;
}

SonareError probe_add_automation_lane(const std::string& param, double value) {
  Project project;
  const uint32_t track = project.add_audio_track();
  SonareAutomationPoint point{};
  const SonareAutomationLaneDesc desc = lane_for(param, value, &point);
  uint32_t lane_id = 0;
  return sonare_project_add_automation_lane(project.handle, track, &desc, &lane_id);
}

SonareError probe_edit_automation_lane(const std::string& param, double value) {
  Project project;
  const uint32_t track = project.add_audio_track();
  SonareAutomationLaneDesc existing;
  sonare_automation_lane_desc_init(&existing);
  existing.target_param_id = 1;
  uint32_t lane_id = 0;
  REQUIRE(sonare_project_add_automation_lane(project.handle, track, &existing, &lane_id) ==
          SONARE_OK);
  SonareAutomationPoint point{};
  const SonareAutomationLaneDesc desc = lane_for(param, value, &point);
  return sonare_project_edit_automation_lane(project.handle, track, lane_id, &desc);
}

/// Four frames of mono silence the audio-payload probes point at.
const float* payload_buffer() {
  static const std::vector<float> buffer(16, 0.0f);
  return buffer.data();
}

SonareError probe_add_clip(const std::string& param, double value) {
  Project project;
  SonareProjectClipDesc desc{};
  desc.track_id = project.add_audio_track();
  desc.length_ppq = 4.0;
  desc.gain = 1.0f;
  desc.audio_interleaved = payload_buffer();
  desc.audio_frames = 1;
  desc.audio_channels = 1;
  desc.audio_sample_rate = 48000;
  if (is_param(param, "desc.audio_frames")) desc.audio_frames = to_i64(value);
  if (is_param(param, "desc.audio_channels")) desc.audio_channels = static_cast<int>(value);
  if (is_param(param, "desc.audio_sample_rate")) desc.audio_sample_rate = static_cast<int>(value);
  if (is_param(param, "desc.start_ppq")) desc.start_ppq = value;
  if (is_param(param, "desc.length_ppq")) desc.length_ppq = value;
  if (is_param(param, "desc.source_offset_ppq")) desc.source_offset_ppq = value;
  uint32_t out = 0;
  return sonare_project_add_clip(project.handle, &desc, &out);
}

SonareError probe_add_loop_recording_takes(const std::string& param, double value) {
  Project project;
  SonareProjectLoopRecordingDesc desc{};
  desc.track_id = project.add_audio_track();
  desc.loop_length_ppq = 1.0;
  desc.audio_interleaved = payload_buffer();
  desc.audio_frames = 1;
  desc.audio_channels = 1;
  desc.audio_sample_rate = 48000;
  if (is_param(param, "desc.audio_frames")) desc.audio_frames = to_i64(value);
  if (is_param(param, "desc.audio_channels")) desc.audio_channels = static_cast<int>(value);
  if (is_param(param, "desc.audio_sample_rate")) desc.audio_sample_rate = static_cast<int>(value);
  if (is_param(param, "desc.start_ppq")) desc.start_ppq = value;
  if (is_param(param, "desc.loop_length_ppq")) desc.loop_length_ppq = value;
  uint32_t clip = 0;
  size_t takes = 0;
  return sonare_project_add_loop_recording_takes(project.handle, &desc, &clip, &takes);
}

SonareError probe_set_source_audio(const std::string& param, double value) {
  Project project;
  uint32_t source_id = 0;
  REQUIRE(sonare_project_add_audio_source(project.handle, "probe.wav", &source_id) == SONARE_OK);
  int64_t frames = 1;
  int channels = 1;
  if (is_param(param, "frames")) frames = to_i64(value);
  int sample_rate = 48000;
  if (is_param(param, "channels")) channels = static_cast<int>(value);
  if (is_param(param, "sample_rate")) sample_rate = static_cast<int>(value);
  return sonare_project_set_source_audio(project.handle, source_id, payload_buffer(), frames,
                                         channels, sample_rate);
}

SonareError probe_annotate_chords(const std::string& param, double value) {
  Project project;
  SonareProjectChordSymbol chord{};
  chord.end_ppq = 1.0;
  if (is_param(param, "chords[].start_ppq")) chord.start_ppq = value;
  if (is_param(param, "chords[].end_ppq")) chord.end_ppq = value;
  const size_t count = is_param(param, "count") ? to_count(value) : 1;
  return sonare_project_annotate_chords(project.handle, &chord, count);
}

SonareError probe_annotate_keys(const std::string& param, double value) {
  Project project;
  SonareProjectKeySegment key{};
  key.end_ppq = 1.0;
  if (is_param(param, "keys[].start_ppq")) key.start_ppq = value;
  if (is_param(param, "keys[].end_ppq")) key.end_ppq = value;
  const size_t count = is_param(param, "count") ? to_count(value) : 1;
  return sonare_project_annotate_keys(project.handle, &key, count);
}

SonareError probe_annotate_sections(const std::string& param, double value) {
  Project project;
  SonareProjectSection section{0.0, 1.0, "section"};
  if (is_param(param, "sections[].start_ppq")) section.start_ppq = value;
  if (is_param(param, "sections[].end_ppq")) section.end_ppq = value;
  const size_t count = is_param(param, "count") ? to_count(value) : 1;
  return sonare_project_annotate_sections(project.handle, &section, count);
}

SonareError probe_count_in_frames(const std::string& param, double value) {
  Project project;
  REQUIRE(sonare_project_set_sample_rate(project.handle, 48000) == SONARE_OK);
  int64_t frames = 0;
  const int32_t count_in_bars = is_param(param, "count_in_bars") ? static_cast<int32_t>(value) : 0;
  return sonare_project_count_in_frames(project.handle, count_in_bars, &frames);
}

SonareError probe_snap_to_grid(const std::string& param, double value) {
  Project project;
  SonareProjectSnapToGridOptions options;
  sonare_project_snap_to_grid_options_init(&options);
  if (is_param(param, "division")) options.division = static_cast<int>(value);
  const double ppq = is_param(param, "ppq") ? value : 0.0;
  double out = 0.0;
  return sonare_project_snap_to_grid(project.handle, ppq, 1.0, &options, &out);
}

SonareProjectMarker marker_for(const std::string& param, double value) {
  SonareProjectMarker marker{};
  marker.kind = SONARE_MARKER_KIND_MARKER;
  if (is_param(param, "marker.id")) marker.id = to_u32(value);
  if (is_param(param, "marker.ppq")) marker.ppq = value;
  if (is_param(param, "marker.key_fifths")) {
    marker.kind = SONARE_MARKER_KIND_KEY_SIGNATURE;
    marker.key_fifths = static_cast<int8_t>(value);
  }
  return marker;
}

SonareError probe_set_marker(const std::string& param, double value) {
  Project project;
  const SonareProjectMarker marker = marker_for(param, value);
  uint32_t out = 0;
  return sonare_project_set_marker(project.handle, &marker, "marker", &out);
}

SonareError probe_set_time_signatures(const std::string& param, double value) {
  Project project;
  SonareProjectTimeSignatureSegment segment{};
  segment.numerator = 4;
  segment.denominator = 4;
  if (is_param(param, "segments[].numerator")) segment.numerator = static_cast<int>(value);
  if (is_param(param, "segments[].denominator")) segment.denominator = static_cast<int>(value);
  if (is_param(param, "segments[].start_ppq")) segment.start_ppq = value;
  return sonare_project_set_time_signatures(project.handle, &segment, 1);
}

SonareError probe_set_tempo_segments(const std::string& param, double value) {
  Project project;
  SonareProjectTempoSegment segment{0.0, 120.0, 0.0};
  if (param == "segments[].start_ppq") segment.start_ppq = value;
  if (param == "segments[].bpm") segment.bpm = value;
  if (param == "segments[].end_bpm") segment.end_bpm = value;
  return sonare_project_set_tempo_segments(project.handle, &segment, 1);
}

SonareError probe_set_assist_sidecar(const std::string& param, double value) {
  Project project;
  const double start = is_param(param, "region_start_ppq") ? value : 0.0;
  const double end = is_param(param, "region_end_ppq") ? value : 1.0;
  return sonare_project_set_assist_sidecar(project.handle, "probe", 1, 0, start, end, nullptr, 0);
}

SonareError probe_add_midi_clip(const std::string& param, double value) {
  Project project;
  const double start = is_param(param, "start_ppq") ? value : 0.0;
  const double length = is_param(param, "length_ppq") ? value : 4.0;
  uint32_t track = 0;
  uint32_t clip = 0;
  return sonare_project_add_midi_clip(project.handle, start, length, &track, &clip);
}

SonareError probe_split_clip(const std::string&, double value) {
  Project project;
  const uint32_t clip = project.add_audio_clip();
  uint32_t out = 0;
  return sonare_project_split_clip(project.handle, clip, value, &out);
}

SonareError probe_trim_clip(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  const double start = is_param(param, "new_start_ppq") ? value : 0.0;
  const double length = is_param(param, "new_length_ppq") ? value : 2.0;
  return sonare_project_trim_clip(project.handle, clip, start, length);
}

SonareError probe_load_sound_font(const std::string&, double value) {
  Project project;
  const auto& data = probe_sound_font();
  return sonare_project_load_sound_font(project.handle, nullptr, data.data(), to_count(value));
}

SonareError probe_move_clip(const std::string&, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  return sonare_project_move_clip(project.handle, clip, value, 0);
}

SonareError probe_duplicate_clip(const std::string&, double value) {
  Project project;
  const uint32_t clip = project.add_audio_clip();
  // Leave space at PPQ zero for the duplicate.
  REQUIRE(sonare_project_move_clip(project.handle, clip, 4.0, 0) == SONARE_OK);
  uint32_t out = 0;
  return sonare_project_duplicate_clip(project.handle, clip, value, &out);
}

SonareError probe_set_clip_loop(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  const double length = is_param(param, "loop_length_ppq") ? value : 0.0;
  const double crossfade = is_param(param, "loop_crossfade_ppq") ? value : 0.0;
  return sonare_project_set_clip_loop(project.handle, clip, SONARE_LOOP_MODE_LOOP, length,
                                      crossfade);
}

SonareError probe_set_clip_fade(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  SonareProjectClipFade fade_in{};
  SonareProjectClipFade fade_out{};
  if (is_param(param, "fade_in.length_ppq")) fade_in.length_ppq = value;
  if (is_param(param, "fade_out.length_ppq")) fade_out.length_ppq = value;
  return sonare_project_set_clip_fade(project.handle, clip, &fade_in, &fade_out);
}

SonareError probe_set_clip_takes(const std::string&, double value) {
  Project project;
  const uint32_t clip = project.add_audio_clip();
  SonareProjectClipTake take{};
  take.id = 1;
  take.source_offset_ppq = value;
  return sonare_project_set_clip_takes(project.handle, clip, &take, 1, 1);
}

SonareError probe_set_clip_comp_segments(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_audio_clip();
  SonareProjectClipTake take{};
  take.id = 1;
  REQUIRE(sonare_project_set_clip_takes(project.handle, clip, &take, 1, take.id) == SONARE_OK);
  SonareProjectClipCompSegment segment{};
  segment.take_id = take.id;
  segment.end_ppq = 1.0;
  if (is_param(param, "segments[].start_ppq")) segment.start_ppq = value;
  if (is_param(param, "segments[].end_ppq")) segment.end_ppq = value;
  if (is_param(param, "segments[].crossfade_ppq")) segment.crossfade_ppq = value;
  return sonare_project_set_clip_comp_segments(project.handle, clip, &segment, 1);
}

SonareError probe_set_warp_map(const std::string&, double value) {
  Project project;
  const SonareProjectWarpAnchor anchors[] = {{0.0, 0.0}, {100.0, 100.0}};
  SonareProjectWarpMapDesc desc{};
  desc.id = to_u32(value);
  desc.anchors = anchors;
  desc.anchor_count = std::size(anchors);
  return sonare_project_set_warp_map(project.handle, &desc);
}

SonareError probe_chord_symbol_from_analysis(const std::string&, double value) {
  uint32_t root_pc = 0;
  uint32_t quality = 0;
  uint8_t extensions[32] = {};
  size_t extension_count = 0;
  uint32_t slash_bass = 0;
  return sonare_chord_symbol_from_analysis(static_cast<SonarePitchClass>(static_cast<int>(value)),
                                           SONARE_CHORD_MAJOR, SONARE_PITCH_C, &root_pc, &quality,
                                           extensions, 32, &extension_count, &slash_bass);
}

// ---- Bounce -----------------------------------------------------------------------------

/// Runs @p bounce over a fresh empty project with a 64-frame render and @p param set to @p value.
template <typename Bounce>
SonareError bounce_options_with(const std::string& param, double value, Bounce&& bounce) {
  Project project;
  REQUIRE(sonare_project_set_sample_rate(project.handle, 48000) == SONARE_OK);
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 64;
  options.block_size = 64;
  options.num_channels = 2;
  if (is_param(param, "options.total_frames")) options.total_frames = to_i64(value);
  if (is_param(param, "options.block_size")) options.block_size = static_cast<int>(value);
  if (is_param(param, "options.num_channels")) options.num_channels = static_cast<int>(value);
  if (is_param(param, "options.instrument_latency_samples")) {
    options.instrument_latency_samples = static_cast<int>(value);
  }
  if (is_param(param, "options.count_in_bars")) {
    options.count_in_bars = static_cast<int32_t>(value);
  }
  SonareSynthInstrumentBinding synth{};
  SonareProjectArticulationBinding articulation{};
  if (is_param(param, "options.articulations[].channel")) {
    sonare_synth_patch_init(&synth.patch);
    synth.patch.engine_mode = SONARE_SYNTH_ENGINE_REED;
    options.synth_instruments = &synth;
    options.synth_instrument_count = 1;
    articulation.channel = to_u32(value);
    articulation.articulation = SONARE_ARTICULATION_MONO_LEGATO;
    options.articulations = &articulation;
    options.articulation_count = 1;
  }
  float* out = nullptr;
  size_t out_len = 0;
  int channels = 0;
  const SonareError err = bounce(project.handle, &options, &out, &out_len, &channels);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_bounce(const std::string& param, double value) {
  return bounce_options_with(
      param, value,
      [](SonareProject* p, const SonareProjectBounceOptions* o, float** out, size_t* len, int* ch) {
        return sonare_test::bounce_interleaved(p, o, out, len, ch);
      });
}

// ---- External stems ---------------------------------------------------------------------

SonareError probe_import_external_stems(const std::string& param, double value) {
  Project project;
  REQUIRE(sonare_project_set_sample_rate(project.handle, 48000) == SONARE_OK);
  const float* planes[] = {payload_buffer()};
  SonareExternalStemDesc stem{};
  stem.name = "stem";
  stem.layout = SONARE_EXTERNAL_STEM_MONO;
  stem.planar_samples = planes;
  stem.frame_count = 4;
  if (is_param(param, "request.stems[].start_frame")) stem.start_frame = to_i64(value);
  SonareExternalStemImportRequest request;
  sonare_external_stem_import_request_init(&request);
  request.sample_rate = 48000;
  request.stems = &stem;
  request.stem_count = 1;
  if (is_param(param, "request.sample_rate")) request.sample_rate = static_cast<int>(value);
  SonareExternalStemImportResult result{};
  const SonareError err = sonare_project_import_external_stems(project.handle, &request, &result);
  sonare_free_external_stem_import_result(&result);
  return err;
}

#if defined(SONARE_WITH_PITCH_EDITOR)

// ---- Vocal edits ------------------------------------------------------------------------

constexpr size_t kVocalProbeFrames = 3200;
constexpr int kVocalProbeRate = 48000;
constexpr double kVocalProbeLengthPpq = 1.0;

struct VocalProbeState {
  std::vector<uint8_t> bytes;
  SonareVocalStateToken token{};
  std::array<uint8_t, 32> digest{};
};

VocalProbeState make_vocal_probe_state(const float* samples) {
  constexpr size_t kAnalysisFrames = 20;
  std::array<float, kAnalysisFrames> f0{};
  std::array<uint8_t, kAnalysisFrames> voiced{};
  f0.fill(440.0f);
  voiced.fill(1);
  SonareVocalAnalysis analysis;
  sonare_vocal_analysis_init(&analysis);
  analysis.samples_per_frame = kVocalProbeFrames / kAnalysisFrames;
  analysis.frame_length_samples = 2 * (kVocalProbeFrames / kAnalysisFrames);
  analysis.f0_hz = f0.data();
  analysis.voiced = voiced.data();
  analysis.frame_count = f0.size();

  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  options.analysis = &analysis;
  SonareVocalSession* raw = nullptr;
  REQUIRE(sonare_vocal_session_create(samples, kVocalProbeFrames, 1, kVocalProbeRate, &options,
                                      &raw) == SONARE_OK);
  const std::unique_ptr<SonareVocalSession, decltype(&sonare_vocal_session_destroy)> session(
      raw, sonare_vocal_session_destroy);
  VocalProbeState state;
  REQUIRE(sonare_vocal_session_token(session.get(), &state.token) == SONARE_OK);
  SonareVocalStateBytes bytes;
  sonare_vocal_state_bytes_init(&bytes);
  REQUIRE(sonare_vocal_session_export_state(session.get(), &bytes) == SONARE_OK);
  state.bytes.assign(bytes.data, bytes.data + bytes.size);
  sonare_free_vocal_state_bytes(&bytes);
  sonare::util::Sha256 hash;
  hash.update(reinterpret_cast<const uint8_t*>(samples), kVocalProbeFrames * sizeof(float));
  state.digest = hash.finalize();
  return state;
}

SonareError probe_apply_vocal_edit(const std::string& param, double value) {
  const float* samples = analysis_samples().data();
  const auto state = make_vocal_probe_state(samples);
  Project project;
  const uint32_t clip_id =
      project.add_audio_clip(kVocalProbeLengthPpq, samples, kVocalProbeFrames, kVocalProbeRate);
  SonareProjectClip clip{};
  REQUIRE(sonare_project_clip_by_index(project.handle, 0, &clip) == SONARE_OK);
  SonareProjectVocalEditApplyDesc desc;
  sonare_project_vocal_edit_apply_desc_init(&desc);
  desc.clip_id = clip_id;
  desc.expected_source_id = clip.source_id;
  desc.expected_source_sample_rate = kVocalProbeRate;
  desc.expected_source_sample_count = kVocalProbeFrames;
  desc.expected_clip_length_ppq = kVocalProbeLengthPpq;
  std::memcpy(desc.expected_source_sha256, state.digest.data(), state.digest.size());
  desc.rendered_mono = samples;
  desc.rendered_sample_count = kVocalProbeFrames;
  desc.rendered_sample_rate = kVocalProbeRate;
  desc.render_token = state.token;
  desc.sve1 = state.bytes.data();
  desc.sve1_size = state.bytes.size();
  if (is_param(param, "desc.clip_id")) desc.clip_id = to_u32(value);
  if (is_param(param, "desc.take_id")) desc.take_id = to_u32(value);
  if (is_param(param, "desc.expected_source_sample_rate")) {
    desc.expected_source_sample_rate = to_u32(value);
  }
  if (is_param(param, "desc.expected_source_sample_count")) {
    desc.expected_source_sample_count = to_count(value);
  }
  if (is_param(param, "desc.expected_clip_length_ppq")) desc.expected_clip_length_ppq = value;
  if (is_param(param, "desc.expected_source_offset_ppq")) desc.expected_source_offset_ppq = value;
  SonareProjectVocalEditApplyResult result;
  sonare_project_vocal_edit_apply_result_init(&result);
  return sonare_project_apply_vocal_edit(project.handle, &desc, &result);
}

SonareError probe_rehydrate_vocal_edits(const std::string& param, double value) {
  Project project;
  SonareProjectVocalOriginalSource original;
  sonare_project_vocal_original_source_init(&original);
  original.source_id = 1;
  original.mono = payload_buffer();
  original.sample_count = 1;
  original.sample_rate = 48000;
  if (is_param(param, "originals[].source_id")) original.source_id = to_u32(value);
  if (is_param(param, "originals[].sample_count")) original.sample_count = to_count(value);
  if (is_param(param, "originals[].sample_rate")) original.sample_rate = static_cast<int>(value);
  SonareProjectVocalRehydrateResult result;
  sonare_project_vocal_rehydrate_result_init(&result);
  const SonareError err =
      sonare_project_rehydrate_vocal_edits(project.handle, &original, 1, nullptr, nullptr, &result);
  sonare_free_project_vocal_rehydrate_result(&result);
  return err;
}

/// The transcription config with `param` (`config.<field>`) set to `value` and the rest at the
/// defaults. An UNSET integer is 0, so a value below a domain starting at 1 is probed at -1.
SonareTranscribeConfig transcribe_config(const std::string& param, double value) {
  SonareTranscribeConfig config{};
  sonare_transcribe_config_init(&config);
  const std::string field = param.substr(param.rfind('.') + 1);
  const auto set_value = static_cast<float>(value);
  const auto set_int = static_cast<int32_t>(value);
  if (field == "reference_hz") config.reference_hz = set_value;
  if (field == "f0_min_hz") config.f0_min_hz = set_value;
  if (field == "f0_max_hz") config.f0_max_hz = set_value;
  if (field == "min_note_ms") config.min_note_ms = set_value;
  if (field == "segmentation_threshold_cents") config.segmentation_threshold_cents = set_value;
  if (field == "velocity_floor_db") config.velocity_floor_db = set_value;
  if (field == "fixed_velocity") config.fixed_velocity = set_int;
  if (field == "group") config.group = static_cast<int32_t>(value);
  if (field == "channel") config.channel = static_cast<int32_t>(value);
  if (field == "min_note_division") config.min_note_division = set_int;
  const bool polyphonic_only = field == "max_polyphony" || field == "min_frame_peak_ratio" ||
                               field == "min_ridge_peak_ratio" || field == "reattack_ratio";
  if (polyphonic_only) config.polyphonic = 1;
  if (field == "max_polyphony") config.max_polyphony = set_int;
  if (field == "min_frame_peak_ratio") config.min_frame_peak_ratio = set_value;
  if (field == "min_ridge_peak_ratio") config.min_ridge_peak_ratio = set_value;
  if (field == "reattack_ratio") config.reattack_ratio = set_value;
  return config;
}

SonareError probe_transcribe(const std::string& param, double value) {
  const SonareTranscribeConfig config = transcribe_config(param, value);
  const std::vector<float>& samples = analysis_samples();
  SonareTranscribeResult result{};
  const float bpm = param == "tempo_bpm" ? static_cast<float>(value) : 120.0f;
  const SonareError err =
      sonare_transcribe(samples.data(), samples.size(), kAnalysisRate, bpm, &config, &result);
  sonare_free_transcribe_result(&result);
  return err;
}

SonareError probe_project_transcribe_to_clip(const std::string& param, double value) {
  Project project;
  const uint32_t clip = project.add_midi_clip();
  const SonareTranscribeConfig config = transcribe_config(param, value);
  const std::vector<float>& samples = analysis_samples();
  size_t note_count = 0;
  return sonare_project_transcribe_to_clip(project.handle, clip, samples.data(), samples.size(),
                                           kAnalysisRate, &config, &note_count);
}

#endif  // SONARE_WITH_PITCH_EDITOR

SonareError probe_bar_start_ppq(const std::string&, double value) {
  Project project;
  double ppq = 0;
  return sonare_project_bar_start_ppq(project.handle, static_cast<int64_t>(value), &ppq);
}

SonareError probe_position_at_ppq(const std::string&, double value) {
  Project project;
  SonareProjectPosition position{};
  return sonare_project_position_at_ppq(project.handle, value, &position);
}

SonareError probe_ppq_to_sec(const std::string&, double value) {
  Project project;
  double sec = 0;
  return sonare_project_ppq_to_sec(project.handle, value, &sec);
}

SonareError probe_sec_to_ppq(const std::string&, double value) {
  Project project;
  double ppq = 0;
  return sonare_project_sec_to_ppq(project.handle, value, &ppq);
}

#endif  // SONARE_WITH_ARRANGEMENT

const std::map<std::string, Probe>& probes() {
  static const std::map<std::string, Probe> table = {
      {"extract_percussive_events", probe_extract_percussive_events},
      {"scale_mask_for_mode", probe_scale_mask_for_mode},
#if defined(SONARE_WITH_PITCH_EDITOR)
      {"scale_correction_semitones", probe_scale_correction_semitones},
      {"scale_pitch_class_enabled", probe_scale_pitch_class_enabled},
      {"scale_quantize_midi", probe_scale_quantize_midi},
      {"vocal_session_create", probe_vocal_session_create},
#endif
#if defined(SONARE_WITH_MIXING)
      {"mixer_create", probe_mixer_create},
#if defined(SONARE_WITH_GRAPH)
      {"strip_schedule_fader_automation", probe_strip_fader},
      {"strip_schedule_insert_automation", probe_strip_insert},
      {"strip_schedule_pan_automation", probe_strip_pan},
      {"strip_schedule_send_automation", probe_strip_send},
      {"strip_schedule_width_automation", probe_strip_width},
#endif
#endif
#if defined(SONARE_WITH_MIXING_ASSISTANT)
      {"mixing_assistant_suggest", probe_mixing_assistant_suggest},
      {"validate_mixing_assistant_config", probe_validate_mixing_assistant_config},
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
      {"chord_symbol_from_analysis", probe_chord_symbol_from_analysis},
      {"midi_cc_name", probe_midi_lookup<sonare_midi_cc_name>},
      {"midi_gm_drum_name", probe_midi_lookup<sonare_midi_gm_drum_name>},
      {"midi_gm_family_first_program", probe_midi_lookup<sonare_midi_gm_family_first_program>},
      {"midi_gm_family_name", probe_midi_lookup<sonare_midi_gm_family_name>},
      {"midi_gm_instrument_name", probe_midi_lookup<sonare_midi_gm_instrument_name>},
      {"midi_gm2_drum_set_name", probe_midi_lookup<sonare_midi_gm2_drum_set_name>},
      {"midi_per_note_controller_name", probe_midi_lookup<sonare_midi_per_note_controller_name>},
      {"midi_gm2_drum_name", probe_midi_bank_lookup<sonare_midi_gm2_drum_name>},
      {"midi_gm2_instrument_name", probe_midi_bank_lookup<sonare_midi_gm2_instrument_name>},
      {"midi_bank_program", probe_midi_bank_program},
      {"midi_cc", probe_midi_cc},
      {"midi_cc_learn", probe_midi_cc_learn},
      {"midi_cc_to_breakpoint", probe_midi_cc_to_breakpoint},
      {"midi_channel_pressure", probe_midi_channel_pressure},
      {"midi_note_off", probe_midi_note_off},
      {"midi_note_on", probe_midi_note_on},
      {"midi_param_to_cc", probe_midi_param_to_cc},
      {"midi_pitch_bend", probe_midi_pitch_bend},
      {"midi_poly_pressure", probe_midi_poly_pressure},
      {"midi_program", probe_midi_program},
      {"midi_route_events", probe_midi_route_events},
      {"midi2_assignable_controller", probe_midi2_assignable_controller},
      {"midi2_assignable_per_note_controller", probe_midi2_assignable_per_note_controller},
      {"midi2_cc", probe_midi2_cc},
      {"midi2_channel_pressure", probe_midi2_channel_pressure},
      {"midi2_note_off", probe_midi2_note_off},
      {"midi2_note_on", probe_midi2_note_on},
      {"midi2_per_note_management", probe_midi2_per_note_management},
      {"midi2_per_note_pitch_bend", probe_midi2_per_note_pitch_bend},
      {"midi2_pitch_bend", probe_midi2_pitch_bend},
      {"midi2_poly_pressure", probe_midi2_poly_pressure},
      {"midi2_program", probe_midi2_program},
      {"midi2_registered_controller", probe_midi2_registered_controller},
      {"midi2_registered_per_note_controller", probe_midi2_registered_per_note_controller},
      {"midi2_relative_assignable_controller", probe_midi2_relative_assignable_controller},
      {"midi2_relative_registered_controller", probe_midi2_relative_registered_controller},
      {"project_add_automation_lane", probe_add_automation_lane},
      {"project_add_clip", probe_add_clip},
      {"project_add_loop_recording_takes", probe_add_loop_recording_takes},
      {"project_add_midi_clip", probe_add_midi_clip},
      {"project_annotate_chords", probe_annotate_chords},
      {"project_annotate_keys", probe_annotate_keys},
      {"project_annotate_sections", probe_annotate_sections},
      {"project_bar_start_ppq", probe_bar_start_ppq},
      {"project_bounce", probe_bounce},
      {"project_count_in_frames", probe_count_in_frames},
      {"project_duplicate_clip", probe_duplicate_clip},
      {"project_edit_automation_lane", probe_edit_automation_lane},
      {"project_import_external_stems", probe_import_external_stems},
      {"project_load_sound_font", probe_load_sound_font},
      {"project_move_clip", probe_move_clip},
      {"project_position_at_ppq", probe_position_at_ppq},
      {"project_ppq_to_sec", probe_ppq_to_sec},
      {"project_sec_to_ppq", probe_sec_to_ppq},
      {"project_set_assist_sidecar", probe_set_assist_sidecar},
      {"project_set_clip_comp_segments", probe_set_clip_comp_segments},
      {"project_set_clip_fade", probe_set_clip_fade},
      {"project_set_clip_loop", probe_set_clip_loop},
      {"project_set_clip_takes", probe_set_clip_takes},
      {"project_set_marker", probe_set_marker},
      {"project_set_midi_events", probe_project_set_midi_events},
      {"project_set_program", probe_project_set_program},
      {"project_set_source_audio", probe_set_source_audio},
      {"project_set_tempo_segments", probe_set_tempo_segments},
      {"project_set_time_signatures", probe_set_time_signatures},
      {"project_set_warp_map", probe_set_warp_map},
      {"project_snap_to_grid", probe_snap_to_grid},
      {"project_split_clip", probe_split_clip},
      {"project_trim_clip", probe_trim_clip},
#if defined(SONARE_WITH_PITCH_EDITOR)
      {"project_apply_vocal_edit", probe_apply_vocal_edit},
      {"project_rehydrate_vocal_edits", probe_rehydrate_vocal_edits},
      {"project_transcribe_to_clip", probe_project_transcribe_to_clip},
      {"transcribe", probe_transcribe},
#endif
#endif
  };
  return table;
}

/// The rows whose entry this build compiles.
std::vector<NumericDomainRow> built_rows() {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kProjectDomainRows) {
    if (probes().count(row.entry) != 0) rows.push_back(row);
  }
  return rows;
}

NumericDomainTable built_table() {
  static const std::vector<NumericDomainRow> rows = built_rows();
  return {"project", rows.data(), rows.size()};
}

/// The domain's lowest value, except where the entry cannot run on it: a sample rate must match
/// the project's or the mixer's, and an analysis frame of one sample is not an analysis.
double inside_value(const NumericDomainRow& row) {
  const std::string entry = row.entry;
  const std::string param = row.param;
  if (entry == "mixing_assistant_suggest") return 48000.0;
  if (entry == "project_load_sound_font") return static_cast<double>(probe_sound_font().size());
  // Splits must be interior; a comp segment must fit in its clip.
  if (entry == "project_split_clip" || param == "segments[].end_ppq") return 2.0;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  if (entry == "project_apply_vocal_edit") {
    if (param == "desc.expected_clip_length_ppq") return kVocalProbeLengthPpq;
    if (param == "desc.expected_source_sample_count") return kVocalProbeFrames;
  }
#endif
  if (entry == "project_import_external_stems" && param == "request.sample_rate") return 48000.0;
  if (param.find("sample_rate") != std::string::npos && entry != "mixer_create") return 48000.0;
  if (entry == "transcribe" || entry == "project_transcribe_to_clip") {
    // A frequency pair must stay ordered, and the level floor domain has no lower edge.
    if (param == "config.f0_min_hz") return 100.0;
    if (param == "config.f0_max_hz") return 2000.0;
    if (param == "config.reference_hz") return 440.0;
    if (param == "config.velocity_floor_db") return -60.0;
    if (param == "tempo_bpm") {
      return std::nextafterf(static_cast<float>(*row.domain.lo),
                             std::numeric_limits<float>::infinity());
    }
  }
  // An annotation span must end after its start, which the probe holds at 0.
  if (entry.rfind("project_annotate_", 0) == 0 && param.size() >= 7 &&
      param.compare(param.size() - 7, 7, "end_ppq") == 0) {
    return 1.0;
  }
  if (entry == "vocal_session_create") {
    return param == "options.frame_length_samples" ? 2048.0 : 512.0;
  }
  return lowest_inside_value(row);
}

}  // namespace

TEST_CASE("the project numeric-domain rows are sorted", "[numeric_domains][c_api]") {
  require_sorted(numeric_domain_table("project", kProjectDomainRows));
}

TEST_CASE("a project value outside its row's domain is refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_outside_refused(built_table(), probes());
}

TEST_CASE("a project value inside its row's domain is not refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_inside_accepted(built_table(), probes(), inside_value,
                          {{"midi_cc_learn", "count", SONARE_ERROR_INVALID_STATE, "",
                            "no MIDI CC binding could be learned from events"}});
}
