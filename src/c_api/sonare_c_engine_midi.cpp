#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "rt/command.h"
#include "sonare_c_internal.h"
#include "util/resource_limits.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/midi_fx_json.h"
#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/midi_clip.h"
#include "midi/midi_fx.h"
#include "midi/synth/sf2_player.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

sonare::midi::BuiltinSynthConfig engine_synth_config_from_c(
    const SonareEngineBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

// Binds (or replaces) an engine-owned instrument on a destination, keeping the
// ownership table and the engine's instrument rack in sync. Shared by the
// built-in synth and SF2 instrument entries.
SonareError bind_engine_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                   std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : engine->builtin_instruments) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine->engine.set_midi_instrument(destination_id, raw)) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      entry.second = std::move(instrument);
      return SONARE_OK;
    }
  }
  engine->builtin_instruments.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = engine->builtin_instruments.back().second.get();
  if (!engine->engine.set_midi_instrument(destination_id, raw)) {
    engine->builtin_instruments.pop_back();
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

uint64_t pack_midi_note(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
         (static_cast<uint64_t>(channel) << 16) | (static_cast<uint64_t>(group) << 24);
}

bool valid_midi_note_args(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return group <= 15 && channel <= 15 && note <= 127 && velocity <= 127;
}

uint8_t infer_ump_word_count(const SonareEngineMidiEvent& event) noexcept {
  if (event.word_count >= 1 && event.word_count <= 4) return event.word_count;
  if (event.word3 != 0) return 4;
  if (event.word2 != 0) return 3;
  if (event.word1 != 0) return 2;
  return 1;
}

bool midi_event_from_c(const SonareEngineMidiEvent& src, midi::MidiEvent* out) noexcept {
  // src.group carries no information the UMP does not already hold in word0, so
  // it is validated as struct well-formedness (like `reserved`) and not read for
  // its value: the group is taken from word0, the copy that leaves the process.
  // MidiSequencer::set_midi_clips re-derives it for every publisher, so the
  // assignment here is only what keeps this conversion self-consistent.
  if (!out || src.group > 15 || src.reserved != 0) return false;
  midi::Ump ump{};
  ump.words[0] = src.word0;
  ump.words[1] = src.word1;
  ump.words[2] = src.word2;
  ump.words[3] = src.word3;
  ump.word_count = infer_ump_word_count(src);
  ump.group = midi::ump_group_from_word0(src.word0);
  ump.sysex_handle = src.sysex_handle;
  out->render_frame = src.render_frame;
  out->ump = ump;
  out->sysex_payload = nullptr;
  out->sysex_payload_size = 0;
  return true;
}

}  // namespace
#endif

SonareError sonare_engine_set_midi_clips(SonareRealtimeEngine* engine,
                                         const SonareEngineMidiClipSchedule* clips,
                                         size_t clip_count) {
  SONARE_C_API_ENTRY;
  if (!engine || (clip_count > 0 && clips == nullptr)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)clips;
  (void)clip_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  std::vector<midi::MidiClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineMidiClipSchedule& src = clips[i];
    if (!std::isfinite(src.start_ppq) || src.loop < 0 ||
        (src.event_count > 0 && src.events == nullptr)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    midi::MidiClipSchedule dst;
    dst.id = src.id;
    dst.track_id = src.track_id;
    dst.start_sample = src.start_sample;
    dst.start_ppq = src.start_ppq;
    dst.length_samples = src.length_samples;
    dst.loop_mode = src.loop ? midi::MidiLoopMode::kLoop : midi::MidiLoopMode::kOneShot;
    dst.loop_length_samples = src.loop_length_samples;
    dst.destination_id = src.destination_id;
    dst.events.reserve(src.event_count);
    for (size_t j = 0; j < src.event_count; ++j) {
      midi::MidiEvent event;
      if (!midi_event_from_c(src.events[j], &event)) return SONARE_ERROR_INVALID_PARAMETER;
      dst.events.push_back(event);
    }
    // Stable sort with the off-before-on tiebreak so a same-frame re-trigger
    // releases before re-attacking, matching the offline clip path.
    midi::sort_render_events_stable(dst.events);
    schedules.push_back(std::move(dst));
  }
  engine->engine.set_midi_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config) {
  SONARE_C_API_ENTRY;
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(engine_synth_config_from_c(*config));
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_synth_instrument(SonareRealtimeEngine* engine,
                                               uint32_t destination_id,
                                               const SonareSynthPatch* patch) {
  SONARE_C_API_ENTRY;
  if (!engine || !patch) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(*patch, &cfg, &error)) {
    set_last_error(error != nullptr ? error : "invalid synth patch");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_instrument_automation_id(SonareRealtimeEngine* engine,
                                                           uint32_t destination_id,
                                                           const char* param_name,
                                                           uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  if (!engine || !param_name || param_name[0] == '\0' || !out_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_instrument_automation_id(destination_id, param_name);
  if (id < 0) return SONARE_ERROR_INVALID_PARAMETER;
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size) {
  SONARE_C_API_ENTRY;
  if (!engine || !data || size == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!sonare::resource::sf2_file_fits(size)) {
    set_last_error("sf2: file resource limit exceeded");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  engine->soundfont = std::move(soundfont);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config) {
  SONARE_C_API_ENTRY;
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (config->struct_version > 2) return SONARE_ERROR_INVALID_PARAMETER;
  // A missing SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor, so live MIDI stays audible with zero data.
  SONARE_C_TRY
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (config->gain > 0.0f) cfg.gain = config->gain;
  if (config->polyphony > 0) cfg.polyphony = config->polyphony;
  if (config->struct_version >= 2) {
    cfg.prefer_model_for_modeled_families = config->prefer_model_for_modeled_families != 0;
  }
  // Make the live player EFX-capable: a GS insertion-effect SysEx pushed via
  // sonare_engine_push_midi_sysex is realised on the control thread and swapped
  // in wait-free (realize_efx_inline stays false, the live default). An unknown
  // name or an FX-less build yields a null insert that is bypassed.
#if defined(SONARE_WITH_MASTERING)
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
#endif
  auto player = std::make_unique<sonare::midi::synth::Sf2Player>(cfg);
  player->set_soundfont(engine->soundfont);
  return bind_engine_instrument(engine, destination_id, std::move(player));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_instrument(destination_id, nullptr);
  engine->builtin_instruments.erase(
      std::remove_if(engine->builtin_instruments.begin(), engine->builtin_instruments.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      engine->builtin_instruments.end());
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_instrument_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value) {
  SONARE_C_API_ENTRY;
  if (!engine || channel > 15 || controller > 127 || param_id == 0 || !std::isfinite(min_value) ||
      !std::isfinite(max_value) || max_value < min_value) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sonare::engine::RealtimeEngine::parameter_target_reserved(param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.bind_midi_cc(controller, channel, param_id, min_value, max_value)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_bind_midi_cc_binding(SonareRealtimeEngine* engine,
                                               const SonareMidiCcBinding* binding) {
  SONARE_C_API_ENTRY;
  // Argument validation is expressed purely against the C surface — the public
  // ordinals and SONARE_MIDI_CC_ANY_CHANNEL, never sonare::midi::CcBinding —
  // so a caller passing garbage gets INVALID_PARAMETER in every build. The
  // native types below live in the arrangement library, so touching them here
  // would make this whole function unbuildable without it and would turn a
  // rejected argument into NOT_SUPPORTED.
  if (!engine || !binding || binding->cc_number > 127 || binding->param_id == 0 ||
      binding->kind > SONARE_MIDI_CC_NRPN ||
      (binding->channel != SONARE_MIDI_CC_ANY_CHANNEL && binding->channel > 15) ||
      !std::isfinite(binding->min_value) || !std::isfinite(binding->max_value) ||
      binding->max_value < binding->min_value ||
      sonare::engine::RealtimeEngine::parameter_target_reserved(binding->param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (binding->kind == SONARE_MIDI_CC_CONTROL_CHANGE_14 &&
      (binding->cc_number > 31 || binding->cc_lsb_number != binding->cc_number + 32u)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  static_assert(SONARE_MIDI_CC_ANY_CHANNEL == sonare::midi::kCcAnyChannel,
                "SonareMidiCcBinding wildcard channel must mirror midi::kCcAnyChannel");
  sonare::midi::CcBinding native{};
  native.cc_number = binding->cc_number;
  native.channel = binding->channel;
  native.kind = static_cast<sonare::midi::CcBindingKind>(binding->kind);
  native.cc_lsb_number = binding->cc_lsb_number;
  native.selector_msb = binding->selector_msb;
  native.selector_lsb = binding->selector_lsb;
  native.param_id = binding->param_id;
  native.min_value = binding->min_value;
  native.max_value = binding->max_value;
  return engine->engine.bind_midi_cc(native) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_clear_midi_cc_bindings(SonareRealtimeEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_cc_bindings();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_cc_binding_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_cc_binding_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id,
                                      const char* config_json) {
  SONARE_C_API_ENTRY;
  if (!engine || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  const SonareError parse_err = midi_fx_chain_from_json(config_json, &chain);
  if (parse_err != SONARE_OK) return parse_err;
  return engine->engine.set_midi_fx(destination_id, chain) ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_fx(destination_id);
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_input_source(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(&engine->midi_input_source, destination_id);
  engine->midi_input_source_enabled = true;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_clear_midi_input_source(SonareRealtimeEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(nullptr, 0);
  engine->midi_input_source_enabled = false;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_input_pending_count(SonareRealtimeEngine* engine,
                                                   size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->midi_input_source.pending_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_push_midi_input_note_on(SonareRealtimeEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t port_time_samples) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_on(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_note_off(SonareRealtimeEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t port_time_samples) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_off(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_cc(SonareRealtimeEngine* engine, uint8_t group,
                                             uint8_t channel, uint8_t controller, uint8_t value,
                                             int64_t port_time_samples) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)controller;
  (void)value;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || group > 15 || channel > 15 || controller > 127 ||
      value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_control_change(group, channel, controller, value), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_on(SonareRealtimeEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOnImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_off(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOffImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_cc(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t group, uint8_t channel, uint8_t controller,
                                       uint8_t value, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  if (group > 15 || channel > 15 || controller > 127 || value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  // Pack the scalar MIDI fields into arg.i using the encoding documented in
  // rt/command.h and decoded by RealtimeEngine::apply_command.
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  rt::Command command{};
  command.type = rt::CommandType::kMidiCcImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_panic(SonareRealtimeEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_sysex(SonareRealtimeEngine* engine, uint32_t destination_id,
                                          const uint8_t* data, size_t size, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine || !data || size == 0 || size > 512) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  // Copies the bytes into the engine's bounded SysEx store and enqueues a
  // scalar-only kMidiSysExImmediate command. The C-ABI guard above rejects an
  // oversized payload; a failure here is transient queue back-pressure.
  if (!engine->engine.push_midi_sysex(destination_id, data, size, render_frame)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_destination_external(SonareRealtimeEngine* engine,
                                                        uint32_t destination_id, int external) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)external;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!engine->engine.set_midi_destination_external(destination_id, external != 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_external_midi_clock_enabled(SonareRealtimeEngine* engine,
                                                          int enabled) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)enabled;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  engine->engine.set_external_midi_clock_enabled(enabled != 0);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_external_midi_dropped_count(SonareRealtimeEngine* engine,
                                                      uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  *out_count = engine->engine.external_midi_dropped_count();
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_drain_external_midi(SonareRealtimeEngine* engine,
                                              SonareExternalMidiEvent* out, size_t max_events,
                                              size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  // Initialise the count before any early error return so a defensive consumer
  // never reads an uninitialised value on the rejected-buffer path.
  *out_count = 0;
  // A single queue record lowers to at most 3 MIDI-1 messages, so the buffer
  // must hold at least 3 to guarantee forward progress without losing a record.
  if (max_events < 3) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)max_events;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  size_t written = 0;
  // Lower one queue record at a time, consuming it only when its (up to 3)
  // lowered MIDI-1 messages all fit in the remaining output capacity, so the
  // destructive drain never loses a record that could not be emitted.
  while (written + 3 <= max_events) {
    sonare::host::ExternalMidiRecord record{};
    if (engine->engine.drain_external_midi(&record, 1) == 0) break;
    const sonare::host::ExternalMidi1Lowered lowered =
        sonare::host::lower_external_midi_record(record);
    for (uint8_t m = 0; m < lowered.count; ++m) {
      const sonare::host::ExternalMidi1Message& msg = lowered.messages[m];
      SonareExternalMidiEvent& dst = out[written++];
      dst.destination_id = record.destination_id;
      dst.byte_count = msg.byte_count;
      dst.render_frame = record.event.render_frame;
      dst.bytes[0] = msg.bytes[0];
      dst.bytes[1] = msg.bytes[1];
      dst.bytes[2] = msg.bytes[2];
      dst.reserved[0] = 0;
      dst.reserved[1] = 0;
      dst.reserved[2] = 0;
      dst.reserved[3] = 0;
      dst.reserved[4] = 0;
    }
  }
  *out_count = written;
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}
