#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "c_api/engine_config_domains.h"
#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "midi/live_push_args.h"
#include "rt/command.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/midi_fx_json.h"
#include "c_api/part_rig_json.h"
#include "c_api/sample_bank_internal.h"
#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/articulation_mode.h"
#include "midi/builtin_synth.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_fx.h"
#include "midi/part_rig.h"
#include "midi/synth/sf2_player.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
static_assert(sonare::midi::kArticulationModeCount == SONARE_ARTICULATION_COUNT,
              "articulation mode count out of step with the C ABI");

namespace {

// Maps a refused set_midi_instrument() to its C status: a refused instrument is
// an invalid parameter; a full rack, or a scheduled or queued SysEx the new
// instrument refuses, is invalid state; a failed allocation is out of memory.
SonareError midi_instrument_bind_error(const sonare::engine::RealtimeEngine& engine) noexcept {
  switch (engine.last_midi_instrument_bind_status()) {
    case sonare::engine::MidiInstrumentBindStatus::kAlreadyBoundElsewhere:
      return run_validation([&] {
        util::check::refuse("instrument", "must not already be bound to another destination");
      });
    case sonare::engine::MidiInstrumentBindStatus::kInstrumentPrepareFailed:
      return run_validation([&] {
        util::check::refuse("instrument",
                            "must be preparable at the engine's sample rate and block size");
      });
    case sonare::engine::MidiInstrumentBindStatus::kRackFull:
    case sonare::engine::MidiInstrumentBindStatus::kPreparationFailed:
      return SONARE_ERROR_INVALID_STATE;
    default:
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
}

// Maps a refused bind_midi_cc() whose arguments were already validated. A rebind
// of a bound (cc, channel) fits a full table, so a refusal there is the table
// earlier bindings filled; otherwise the snapshot failed to allocate.
SonareError midi_cc_bind_error(const sonare::engine::RealtimeEngine& engine) noexcept {
  if (engine.midi_cc_binding_count() >= sonare::midi::CcMap::kMaxBindings) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "MIDI CC binding table is full");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_ERROR_OUT_OF_MEMORY;
}

// Binds (or replaces) an engine-owned instrument on a destination, keeping the
// ownership table and the engine's instrument rack in sync. Shared by the
// built-in synth and SF2 instrument entries.
SonareError bind_engine_instrument(SonareEngine* engine, uint32_t destination_id,
                                   std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : engine->builtin_instruments) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine->engine.set_midi_instrument(destination_id, raw)) {
        return midi_instrument_bind_error(engine->engine);
      }
      entry.second = std::move(instrument);
      return SONARE_OK;
    }
  }
  engine->builtin_instruments.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = engine->builtin_instruments.back().second.get();
  if (!engine->engine.set_midi_instrument(destination_id, raw)) {
    engine->builtin_instruments.pop_back();
    return midi_instrument_bind_error(engine->engine);
  }
  return SONARE_OK;
}

// Refuses a destination_id that no bound MIDI instrument answers to.
SonareError refuse_unknown_instrument_destination() noexcept {
  return run_validation([&] {
    util::check::refuse_unknown_id("destination_id", "a destination with a MIDI instrument",
                                   "engine");
  });
}

// Maps an engine instrument-control outcome to the articulation entries' C status.
SonareError articulation_status(
    sonare::engine::RealtimeEngine::InstrumentControlStatus status) noexcept {
  using Status = sonare::engine::RealtimeEngine::InstrumentControlStatus;
  switch (status) {
    case Status::kOk:
      return SONARE_OK;
    case Status::kNoInstrument:
      return refuse_unknown_instrument_destination();
    case Status::kNotSupported:
      break;
  }
  set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                 "destination instrument does not support articulation modes");
  return SONARE_ERROR_NOT_SUPPORTED;
}

uint64_t pack_midi_note(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
         (static_cast<uint64_t>(channel) << 16) | (static_cast<uint64_t>(group) << 24);
}

// Returns INVALID_PARAMETER with the rule's text recorded when the live push argument
// rule refused an argument; SONARE_OK otherwise.
SonareError refuse_live_push_arg(const char* error) noexcept {
  if (error == nullptr) return SONARE_OK;
  set_last_error(SONARE_ERROR_INVALID_PARAMETER, error);
  return SONARE_ERROR_INVALID_PARAMETER;
}

// The input-source family's state check, applied after its arguments.
SonareError require_midi_input_source(const SonareEngine* engine) noexcept {
  if (engine->midi_input_source_enabled) return SONARE_OK;
  set_last_error(SONARE_ERROR_INVALID_STATE, "the MIDI input source is not enabled");
  return SONARE_ERROR_INVALID_STATE;
}

// Queues @p ump on the engine-owned input source after the state check.
SonareError push_midi_input_event(SonareEngine* engine, const midi::Ump& ump,
                                  int64_t offset_in_block_samples) {
  if (const SonareError state = require_midi_input_source(engine); state != SONARE_OK) {
    return state;
  }
  return engine->midi_input_source.push_event(ump, offset_in_block_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
}

// Queues one single-word MIDI 1.0 channel-voice UMP to a destination. The three
// per-note expression dimensions take this path rather than the packed scalar
// command the note and CC entry points use, because a bend is 14 bits and that
// encoding has no room for it.
SonareError push_ump_command(SonareEngine* engine, uint32_t destination_id, const midi::Ump& ump,
                             int64_t render_frame) noexcept {
  rt::Command command{};
  command.type = rt::CommandType::kMidiUmpImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(ump.words[0]);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

uint8_t infer_ump_word_count(const SonareEngineMidiEvent& event) noexcept {
  if (event.word_count >= 1 && event.word_count <= 4) return event.word_count;
  if (event.word3 != 0) return 4;
  if (event.word2 != 0) return 3;
  if (event.word1 != 0) return 2;
  return 1;
}

void midi_event_from_c(const SonareEngineMidiEvent& src, const std::string& prefix,
                       midi::MidiEvent* out) {
  // src.group carries no information the UMP does not already hold in word0, so
  // it is validated as struct well-formedness (like `reserved`) and not read for
  // its value: the group is taken from word0, the copy that leaves the process.
  //
  // THIS IS NOT THE ENFORCING SITE. MidiSequencer::set_midi_clips re-derives the
  // group for every publisher, and that is the line the invariant actually rests
  // on -- reverting it turns tests red, while reverting the assignment below
  // leaves every test green because the sequencer normalizes afterwards. The
  // assignment is kept so this boundary conversion yields a self-consistent Ump
  // on its own terms, not because anything downstream depends on it. Anyone
  // changing the sequencer's derivation must not read this line as coverage.
  util::check::integer_within(src.group, kEngineUmpGroupDomain, (prefix + "group").c_str());
  if (src.reserved != 0) util::check::refuse((prefix + "reserved").c_str(), "must be 0");
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
}

}  // namespace
#endif

void sonare_engine_midi_clip_schedule_init(SonareEngineMidiClipSchedule* clip) {
  if (clip == nullptr) return;
  *clip = SonareEngineMidiClipSchedule{};
  clip->struct_size = sizeof(SonareEngineMidiClipSchedule);
  clip->length_samples = SONARE_ENGINE_MIDI_CLIP_OPEN_ENDED;
  clip->gain = 1.0f;
}

void sonare_engine_builtin_synth_config_init(SonareEngineBuiltinSynthConfig* config) {
  if (config == nullptr) return;
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare_c_detail::builtin_synth_config_init(config);
#else
  *config = SonareEngineBuiltinSynthConfig{};
  config->struct_size = sizeof(SonareEngineBuiltinSynthConfig);
#endif
}

void sonare_engine_sf2_instrument_config_init(SonareEngineSf2InstrumentConfig* config) {
  if (config == nullptr) return;
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare_c_detail::sf2_instrument_config_init(config);
#else
  *config = SonareEngineSf2InstrumentConfig{};
  config->struct_size = sizeof(SonareEngineSf2InstrumentConfig);
#endif
}

SonareError sonare_engine_set_midi_clips(SonareEngine* engine,
                                         const SonareEngineMidiClipSchedule* clips,
                                         size_t clip_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(clips, clip_count);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, clips, clip_count);
#else
  SONARE_C_TRY
  std::vector<midi::MidiClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineMidiClipSchedule& src = clips[i];
    const std::string subject_prefix = "clips[" + std::to_string(i) + "].";
    const auto subject = [&](const char* member) { return subject_prefix + member; };
    if (const SonareError err = run_validation([&] {
          require_struct_size(src.struct_size, sizeof(SonareEngineMidiClipSchedule),
                              "clips[" + std::to_string(i) + "]",
                              "sonare_engine_midi_clip_schedule_init");
          util::check::finite(src.start_ppq, subject("start_ppq").c_str());
          if (src.length_samples != SONARE_ENGINE_MIDI_CLIP_OPEN_ENDED) {
            util::check::integer_within(src.length_samples, kEngineClipLengthDomain,
                                        subject("length_samples").c_str());
          }
          util::check::integer_within(src.loop, kEngineMidiClipLoopDomain, subject("loop").c_str());
          if (src.loop != 0) {
            util::check::integer_within(src.loop_length_samples, kEngineMidiClipLoopLengthDomain,
                                        subject("loop_length_samples").c_str());
          }
          util::check::non_null_when_count(src.events, src.event_count, subject("events").c_str(),
                                           subject("event_count").c_str());
          util::check::at_least(src.gain, 0.0, subject("gain").c_str());
          util::check::integer_within(src.fade_in_samples, kEngineClipFadeDomain,
                                      subject("fade_in_samples").c_str());
          util::check::integer_within(src.fade_out_samples, kEngineClipFadeDomain,
                                      subject("fade_out_samples").c_str());
          if (src.fade_out_samples > 0 &&
              src.length_samples == SONARE_ENGINE_MIDI_CLIP_OPEN_ENDED) {
            util::check::refuse_relation({subject("fade_out_samples"), subject("length_samples")},
                                         "{0} must be 0 when {1} is open-ended");
          }
        });
        err != SONARE_OK) {
      return err;
    }
    midi::MidiClipSchedule dst;
    dst.id = src.id;
    dst.track_id = src.track_id;
    dst.start_sample = src.start_sample;
    dst.start_ppq = src.start_ppq;
    // The core spells an open-ended clip, and the loop period of a one-shot clip, as 0.
    dst.length_samples =
        src.length_samples == SONARE_ENGINE_MIDI_CLIP_OPEN_ENDED ? 0 : src.length_samples;
    dst.loop_mode = src.loop ? midi::MidiLoopMode::kLoop : midi::MidiLoopMode::kOneShot;
    dst.loop_length_samples = src.loop ? src.loop_length_samples : 0;
    dst.destination_id = src.destination_id;
    dst.gain = src.gain;
    dst.fade_in_samples = src.fade_in_samples;
    dst.fade_out_samples = src.fade_out_samples;
    dst.events.reserve(src.event_count);
    for (size_t j = 0; j < src.event_count; ++j) {
      midi::MidiEvent event;
      midi_event_from_c(src.events[j], subject_prefix + "events[" + std::to_string(j) + "].",
                        &event);
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

SonareError sonare_engine_set_builtin_instrument(SonareEngine* engine, uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(config);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  if (const SonareError err = run_validation([&] {
        require_struct_size(config->struct_size, sizeof(SonareEngineBuiltinSynthConfig), "config",
                            "sonare_engine_builtin_synth_config_init");
      });
      err != SONARE_OK) {
    return err;
  }
  if (!valid_builtin_waveform(config->waveform)) {
    return run_validation([&] {
      util::check::one_of(config->waveform, std::array<int, 4>{0, 1, 2, 3}, "config.waveform");
    });
  }
  SONARE_C_TRY
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(
      sonare_c_detail::builtin_synth_config_from_c(*config, "config."));
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// The engine takes a share of @p bank, so the caller may destroy its handle.
SonareError bind_native_synth(SonareEngine* engine, uint32_t destination_id,
                              const SonareSynthPatch& patch, SonareSampleBank* bank,
                              bool use_gm_programs) {
  if (const SonareError err = run_validation([&] {
        require_struct_size(patch.struct_size, sizeof(SonareSynthPatch), "patch",
                            "sonare_synth_patch_init");
      });
      err != SONARE_OK) {
    return err;
  }
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error)) {
    if (error != nullptr) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER, error);
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    return run_validation([&] { util::check::refuse("patch", "must be a valid synth patch"); });
  }
  cfg.use_gm_programs = use_gm_programs;
#if defined(SONARE_WITH_MASTERING)
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
#endif
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  if (bank != nullptr) {
    synth->set_sample_bank(std::shared_ptr<const sonare::midi::synth::SampleBank>(bank->bank));
  }
  return bind_engine_instrument(engine, destination_id, std::move(synth));
}

}  // namespace
#endif

SonareError sonare_engine_set_synth_instrument(SonareEngine* engine,
                                               const SonareSynthInstrumentBinding* binding) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(binding);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  SONARE_C_TRY
  return bind_native_synth(engine, binding->destination_id, binding->patch, binding->sample_bank,
                           binding->use_gm_programs != 0);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_instrument_automation_id(SonareEngine* engine,
                                                           uint32_t destination_id,
                                                           const char* param_name,
                                                           uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(param_name);
  if (param_name[0] == '\0') {
    return run_validation([&] { util::check::non_empty(param_name, "param_name"); });
  }
  SONARE_C_REQUIRE_NONNULL(out_id);
  // Defined on every exit path, so an unresolved name and a feature-disabled
  // build both leave the caller a readable id rather than uninitialised memory.
  *out_id = 0;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_instrument_automation_id(destination_id, param_name);
  if (id < 0) {
    if (engine->engine.midi_instrument(destination_id) == nullptr) {
      return refuse_unknown_instrument_destination();
    }
    return run_validation([&] {
      util::check::refuse(
          "param_name",
          "must be the name of a parameter of the instrument bound to destination_id");
    });
  }
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_load_soundfont(SonareEngine* engine, const uint8_t* data, size_t size) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(data);
  if (size == 0) {
    return run_validation(
        [&] { util::check::integer_within(size, kEngineSoundFontSizeDomain, "size"); });
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  SONARE_C_TRY
  if (!sonare::resource::sf2_file_fits(size)) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, "sf2: file resource limit exceeded");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, error.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  engine->soundfont = std::move(soundfont);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_sf2_instrument(SonareEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(config);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  if (const SonareError err = run_validation([&] {
        require_struct_size(config->struct_size, sizeof(SonareEngineSf2InstrumentConfig), "config",
                            "sonare_engine_sf2_instrument_config_init");
      });
      err != SONARE_OK) {
    return err;
  }
  // A missing SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor, so live MIDI stays audible with zero data.
  SONARE_C_TRY
  auto cfg = sonare_c_detail::sf2_player_config_from_c(*config, "config.");
  // Prepare GS insertion-effect processors on CONTROL; scheduled SysEx selects
  // and updates them at its render frame. An unknown name or an FX-less build
  // yields a null insert that is bypassed.
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

SonareError sonare_engine_clear_midi_instrument(SonareEngine* engine, uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  SONARE_C_TRY
  if (!engine->engine.set_midi_instrument(destination_id, nullptr)) {
    return midi_instrument_bind_error(engine->engine);
  }
  engine->builtin_instruments.erase(
      std::remove_if(engine->builtin_instruments.begin(), engine->builtin_instruments.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      engine->builtin_instruments.end());
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_midi_instrument_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  *out_count = engine->engine.midi_instrument_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_articulation(SonareEngine* engine, uint32_t destination_id,
                                           uint8_t channel, int articulation) {
  SONARE_C_API_ENTRY;
  // The ordinal is checked against the C surface alone, so a caller passing
  // garbage is refused in every build rather than being told the feature is
  // absent. An out-of-range mode is not clamped: kPoly substituted for a
  // misspelled kMonoLegato plays every note and slurs none of them, which the
  // caller cannot tell from a request that took.
  SONARE_C_REQUIRE_NONNULL(engine);
  if (channel > 15) {
    return run_validation(
        [&] { util::check::integer_within(channel, kEngineMidiChannelDomain, "channel"); });
  }
  if (!valid_c_enum(articulation, SONARE_ARTICULATION_COUNT)) {
    return run_validation([&] {
      util::check::one_of(articulation, std::array<int, SONARE_ARTICULATION_COUNT>{0, 1, 2},
                          "articulation");
    });
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  return articulation_status(engine->engine.set_instrument_articulation(
      destination_id, channel,
      admitted_ordinal_to_enum<sonare::midi::ArticulationMode>(articulation)));
#endif
}

SonareError sonare_engine_articulation(SonareEngine* engine, uint32_t destination_id,
                                       uint8_t channel, int* out_articulation) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_articulation);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (channel > 15) {
    return run_validation(
        [&] { util::check::integer_within(channel, kEngineMidiChannelDomain, "channel"); });
  }
  SONARE_C_REQUIRE_NONNULL(out_articulation);
  // Defined on every exit path, so a feature-disabled build and an instrument
  // without articulation both leave the caller a readable ordinal.
  *out_articulation = SONARE_ARTICULATION_POLY;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  sonare::midi::ArticulationMode mode = sonare::midi::ArticulationMode::kPoly;
  const SonareError status =
      articulation_status(engine->engine.instrument_articulation(destination_id, channel, &mode));
  if (status == SONARE_OK) *out_articulation = static_cast<int>(mode);
  return status;
#endif
}

SonareError sonare_engine_legato_fallback_count(SonareEngine* engine, uint32_t destination_id,
                                                size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = 0;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  SONARE_C_TRY
  sonare::midi::MidiInstrument* instrument = engine->engine.midi_instrument(destination_id);
  if (instrument == nullptr) return refuse_unknown_instrument_destination();
  uint64_t counted = 0;
  if (!instrument->legato_fallback_count(&counted)) {
    set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                   "destination instrument does not count legato fallbacks");
    return SONARE_ERROR_NOT_SUPPORTED;
  }
  *out_count = static_cast<size_t>(counted);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_part_rig(SonareEngine* engine, uint32_t destination_id, uint8_t part,
                                       int mode, const char* inserts_json) {
  SONARE_C_API_ENTRY;
  // Shape is checked against the C surface alone, so garbage is refused in every
  // build rather than being told the feature is absent. 2 is the chain mode.
  constexpr int kChainMode = 2;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (part >= 16 && part != 0xFF) {
    return run_validation(
        [&] { util::check::refuse("part", "must be an integer in [0, 15] or 255"); });
  }
  if (!valid_c_enum(mode, 0, kChainMode)) {
    return run_validation(
        [&] { util::check::one_of(mode, std::array<int, 3>{0, 1, kChainMode}, "mode"); });
  }
  if (mode == kChainMode && inserts_json == nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"inserts_json", "mode"},
                                   "{0} must not be NULL when {1} is chain");
    });
  }
  if (mode != kChainMode && inserts_json != nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"inserts_json", "mode"},
                                   "{0} must be NULL unless {1} is chain");
    });
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "part rigs require a build with arrangement");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::PartRig rig;
  rig.mode = admitted_ordinal_to_enum<sonare::midi::PartRigMode>(mode);
  if (inserts_json != nullptr) {
    const SonareError parsed = parse_part_rig_inserts(inserts_json, &rig.stages);
    if (parsed != SONARE_OK) return parsed;
  }
  // The part was admitted above, so what is left to refuse is the content of inserts_json.
  if (!sonare::midi::validate_part_rig(part, rig)) return SONARE_ERROR_INVALID_FORMAT;
#if defined(SONARE_WITH_MASTERING)
  if (!validate_part_rig_chain(rig.stages)) return SONARE_ERROR_INVALID_FORMAT;
#else
  if (rig.mode == sonare::midi::PartRigMode::kChain) {
    set_last_error(SONARE_ERROR_NOT_SUPPORTED, "chain part rigs require a build with mastering");
    return SONARE_ERROR_NOT_SUPPORTED;
  }
#endif
  sonare::midi::MidiInstrument* instrument = engine->engine.midi_instrument(destination_id);
  if (instrument == nullptr) return refuse_unknown_instrument_destination();
  if (instrument->set_part_rig(part, rig)) return SONARE_OK;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "destination instrument does not support part rigs");
  return SONARE_ERROR_NOT_SUPPORTED;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_bind_midi_cc(SonareEngine* engine, const SonareMidiCcBinding* binding) {
  SONARE_C_API_ENTRY;
  // Argument validation is expressed purely against the C surface — the public
  // ordinals and SONARE_MIDI_CC_ANY_CHANNEL, never sonare::midi::CcBinding —
  // so a caller passing garbage gets INVALID_PARAMETER in every build. The
  // native types below live in the arrangement library, so touching them here
  // would make this whole function unbuildable without it and would turn a
  // rejected argument into NOT_SUPPORTED.
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(binding);
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(binding->cc_number, kEngineMidiControllerDomain,
                                    "binding.cc_number");
        if (binding->channel != SONARE_MIDI_CC_ANY_CHANNEL && binding->channel > 15) {
          util::check::refuse("binding.channel", "must be an integer in [0, 15] or 255");
        }
        util::check::one_of(binding->kind, std::array<int, 4>{0, 1, 2, 3}, "binding.kind");
        util::check::integer_within(binding->param_id, kEngineParameterIdDomain,
                                    "binding.param_id");
        util::check::finite_float(binding->min_value, "binding.min_value");
        util::check::finite_float(binding->max_value, "binding.max_value");
        if (binding->max_value < binding->min_value) {
          util::check::refuse_relation({"binding.max_value", "binding.min_value"},
                                       "{0} must be at least {1}");
        }
        if (sonare::engine::RealtimeEngine::parameter_target_reserved(binding->param_id)) {
          util::check::refuse("binding.param_id", "must not be a reserved engine parameter id");
        }
        if (binding->kind == SONARE_MIDI_CC_CONTROL_CHANGE_14) {
          util::check::integer_within(binding->cc_number, kEngineMidiCc14NumberDomain,
                                      "binding.cc_number");
          if (binding->cc_lsb_number != binding->cc_number + 32u) {
            util::check::refuse_relation({"binding.cc_lsb_number", "binding.cc_number"},
                                         "{0} must be {1} + 32");
          }
        }
      });
      err != SONARE_OK) {
    return err;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  static_assert(SONARE_MIDI_CC_ANY_CHANNEL == sonare::midi::kCcAnyChannel,
                "SonareMidiCcBinding wildcard channel must mirror midi::kCcAnyChannel");
  sonare::midi::CcBinding native{};
  native.cc_number = binding->cc_number;
  native.channel = binding->channel;
  native.kind = admitted_ordinal_to_enum<sonare::midi::CcBindingKind>(binding->kind);
  native.cc_lsb_number = binding->cc_lsb_number;
  native.selector_msb = binding->selector_msb;
  native.selector_lsb = binding->selector_lsb;
  native.param_id = binding->param_id;
  native.min_value = binding->min_value;
  native.max_value = binding->max_value;
  if (engine->engine.bind_midi_cc(native)) return SONARE_OK;
  return midi_cc_bind_error(engine->engine);
#endif
}

SonareError sonare_engine_clear_midi_cc_bindings(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  engine->engine.clear_midi_cc_bindings();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_cc_binding_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  *out_count = engine->engine.midi_cc_binding_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_fx(SonareEngine* engine, uint32_t destination_id,
                                      const char* config_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(config_json);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  const SonareError parse_err = midi_fx_chain_from_json(config_json, &chain);
  if (parse_err != SONARE_OK) return parse_err;
  return engine->engine.set_midi_fx(destination_id, chain) ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_fx(SonareEngine* engine, uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  engine->engine.clear_midi_fx(destination_id);
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_input_source(SonareEngine* engine, uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id);
#else
  engine->engine.set_midi_input_source(&engine->midi_input_source, destination_id);
  engine->midi_input_source_enabled = true;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_clear_midi_input_source(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  engine->engine.set_midi_input_source(nullptr, 0);
  engine->midi_input_source_enabled = false;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_input_pending_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  *out_count = engine->midi_input_source.pending_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_push_midi_input_note_on(SonareEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, note, velocity,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_note_error(group, channel, note, velocity));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine, midi::make_midi1_note_on(group, channel, note, velocity),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_input_note_off(SonareEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, note, velocity,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_note_error(group, channel, note, velocity));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine, midi::make_midi1_note_off(group, channel, note, velocity),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_input_cc(SonareEngine* engine, uint8_t group, uint8_t channel,
                                             uint8_t controller, uint8_t value,
                                             int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, controller, value,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_cc_error(group, channel, controller, value));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine,
                               midi::make_midi1_control_change(group, channel, controller, value),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_input_pitch_bend(SonareEngine* engine, uint8_t group,
                                                     uint8_t channel, uint16_t bend14,
                                                     int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, bend14,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_pitch_bend_error(group, channel, bend14));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine, midi::make_midi1_pitch_bend(group, channel, bend14),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_input_channel_pressure(SonareEngine* engine, uint8_t group,
                                                           uint8_t channel, uint8_t pressure,
                                                           int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, pressure,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_channel_pressure_error(group, channel, pressure));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine, midi::make_midi1_channel_pressure(group, channel, pressure),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_input_poly_pressure(SonareEngine* engine, uint8_t group,
                                                        uint8_t channel, uint8_t note,
                                                        uint8_t pressure,
                                                        int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, note, pressure,
                                offset_in_block_samples);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_poly_pressure_error(group, channel, note, pressure));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(engine,
                               midi::make_midi1_poly_pressure(group, channel, note, pressure),
                               offset_in_block_samples);
#endif
}

SonareError sonare_engine_push_midi_note_on(SonareEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, note,
                                velocity, render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_note_error(group, channel, note, velocity));
      refused != SONARE_OK) {
    return refused;
  }
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOnImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_off(SonareEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, note,
                                velocity, render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_note_error(group, channel, note, velocity));
      refused != SONARE_OK) {
    return refused;
  }
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOffImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_cc(SonareEngine* engine, uint32_t destination_id, uint8_t group,
                                       uint8_t channel, uint8_t controller, uint8_t value,
                                       int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, controller,
                                value, render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_cc_error(group, channel, controller, value));
      refused != SONARE_OK) {
    return refused;
  }
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

SonareError sonare_engine_push_midi_pitch_bend(SonareEngine* engine, uint32_t destination_id,
                                               uint8_t group, uint8_t channel, uint16_t bend14,
                                               int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, bend14,
                                render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_pitch_bend_error(group, channel, bend14));
      refused != SONARE_OK) {
    return refused;
  }
  return push_ump_command(engine, destination_id,
                          midi::make_midi1_pitch_bend(group, channel, bend14), render_frame);
#endif
}

SonareError sonare_engine_push_midi_channel_pressure(SonareEngine* engine, uint32_t destination_id,
                                                     uint8_t group, uint8_t channel,
                                                     uint8_t pressure, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, pressure,
                                render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_channel_pressure_error(group, channel, pressure));
      refused != SONARE_OK) {
    return refused;
  }
  return push_ump_command(engine, destination_id,
                          midi::make_midi1_channel_pressure(group, channel, pressure),
                          render_frame);
#endif
}

SonareError sonare_engine_push_midi_poly_pressure(SonareEngine* engine, uint32_t destination_id,
                                                  uint8_t group, uint8_t channel, uint8_t note,
                                                  uint8_t pressure, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, note,
                                pressure, render_frame);
#else
  if (const SonareError refused =
          refuse_live_push_arg(midi::live_push_poly_pressure_error(group, channel, note, pressure));
      refused != SONARE_OK) {
    return refused;
  }
  return push_ump_command(engine, destination_id,
                          midi::make_midi1_poly_pressure(group, channel, note, pressure),
                          render_frame);
#endif
}

SonareError sonare_engine_push_midi_panic(SonareEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, render_frame);
#else
  rt::Command command{};
  command.type = rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_sysex(SonareEngine* engine, uint32_t destination_id,
                                          const uint8_t* data, size_t size, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(data);
  if (size == 0 || size > sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes) {
    return run_validation([&] {
      util::check::integer_within(static_cast<long long>(size), kEngineSysExSizeDomain, "size");
    });
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, render_frame);
#else
  // Preparation precedes publication into the bounded payload/command stores.
  if (!engine->engine.push_midi_sysex(destination_id, data, size, render_frame)) {
    switch (engine->engine.last_midi_sysex_push_status()) {
      case sonare::engine::MidiSysExPushStatus::kInvalidPayload:
        return run_validation([&] {
          util::check::integer_within(static_cast<long long>(size), kEngineSysExSizeDomain, "size");
        });
      case sonare::engine::MidiSysExPushStatus::kPreparationFailed:
        return run_validation([&] {
          util::check::refuse("data", "must be a SysEx payload the destination accepts");
        });
      case sonare::engine::MidiSysExPushStatus::kPayloadSlotsFull:
      case sonare::engine::MidiSysExPushStatus::kQueueFull:
        set_last_error(SONARE_ERROR_OUT_OF_MEMORY, "the SysEx queue is full");
        return SONARE_ERROR_OUT_OF_MEMORY;
      default:
        set_last_error(SONARE_ERROR_OUT_OF_MEMORY,
                       "out of memory while queueing the SysEx payload");
        return SONARE_ERROR_OUT_OF_MEMORY;
    }
  }
  return SONARE_OK;
#endif
}

SonareError sonare_engine_push_midi_ump(SonareEngine* engine, uint32_t destination_id,
                                        const uint32_t* words, size_t word_count,
                                        int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, words, word_count,
                                render_frame);
#else
  switch (engine->engine.push_midi_ump(destination_id, words, word_count, render_frame)) {
    case sonare::engine::MidiUmpPushResult::kQueued:
      return SONARE_OK;
    case sonare::engine::MidiUmpPushResult::kInvalidMessage:
      return refuse_live_push_arg(midi::kLivePushUmpShapeError);
    case sonare::engine::MidiUmpPushResult::kSlotsFull:
    case sonare::engine::MidiUmpPushResult::kQueueFull:
      set_last_error(SONARE_ERROR_OUT_OF_MEMORY, "the UMP queue is full");
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
  set_last_error(SONARE_ERROR_INVALID_STATE, "the engine reported an unknown UMP push result");
  return SONARE_ERROR_INVALID_STATE;
#endif
  SONARE_C_CATCH
}

SonareError sonare_engine_push_midi_input_ump(SonareEngine* engine, const uint32_t* words,
                                              size_t word_count, int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, words, word_count, offset_in_block_samples);
#else
  if (!sonare::engine::RealtimeEngine::is_pushable_midi_ump(words, word_count)) {
    return refuse_live_push_arg(midi::kLivePushUmpShapeError);
  }
  midi::Ump ump{};
  for (size_t i = 0; i < word_count; ++i) ump.words[i] = words[i];
  ump.word_count = static_cast<uint8_t>(word_count);
  ump.group = midi::ump_group_from_word0(words[0]);
  return push_midi_input_event(engine, ump, offset_in_block_samples);
#endif
  SONARE_C_CATCH
}

SonareError sonare_engine_push_midi_program(SonareEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t program,
                                            int bank_valid, uint8_t bank_msb, uint8_t bank_lsb,
                                            int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, group, channel, program,
                                bank_valid, bank_msb, bank_lsb, render_frame);
#else
  if (const SonareError refused = refuse_live_push_arg(midi::live_push_program_error(
          group, channel, program, bank_valid != 0, bank_msb, bank_lsb));
      refused != SONARE_OK) {
    return refused;
  }
  const midi::Ump ump =
      midi::make_midi2_program_change(group, channel, program, bank_msb, bank_lsb, bank_valid != 0);
  return sonare_engine_push_midi_ump(engine, destination_id, ump.words, ump.word_count,
                                     render_frame);
#endif
}

SonareError sonare_engine_push_midi_input_program(SonareEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t program, int bank_valid,
                                                  uint8_t bank_msb, uint8_t bank_lsb,
                                                  int64_t offset_in_block_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, group, channel, program, bank_valid,
                                bank_msb, bank_lsb, offset_in_block_samples);
#else
  if (const SonareError refused = refuse_live_push_arg(midi::live_push_program_error(
          group, channel, program, bank_valid != 0, bank_msb, bank_lsb));
      refused != SONARE_OK) {
    return refused;
  }
  return push_midi_input_event(
      engine,
      midi::make_midi2_program_change(group, channel, program, bank_msb, bank_lsb, bank_valid != 0),
      offset_in_block_samples);
#endif
}

SonareError sonare_engine_set_midi_destination_external(SonareEngine* engine,
                                                        uint32_t destination_id, int external) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, destination_id, external);
#else
  SONARE_C_TRY
  if (!engine->engine.set_midi_destination_external(destination_id, external != 0)) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "external MIDI destination table is full");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_external_midi_clock_enabled(SonareEngine* engine, int enabled) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, enabled);
#else
  SONARE_C_TRY
  engine->engine.set_external_midi_clock_enabled(enabled != 0);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_external_midi_dropped_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return refuse_not_compiled_in(Feature::kArrangement);
#else
  SONARE_C_TRY
  *out_count = engine->engine.external_midi_dropped_count();
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_drain_external_midi(SonareEngine* engine, SonareExternalMidiEvent* out,
                                              size_t max_events, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out, max_events);
  SONARE_C_REQUIRE_NONNULL(out_count);
  // Initialise the count before any early error return so a defensive consumer
  // never reads an uninitialised value on the rejected-buffer path.
  *out_count = 0;
  if (max_events == 0) return SONARE_OK;
  // A single queue record lowers to at most this many MIDI-1 messages, so the
  // buffer must hold that many to guarantee forward progress without losing a
  // record. Read from the shared lowering type so it cannot drift from it.
  constexpr size_t kMaxLoweredMessages =
      std::extent<decltype(sonare::host::ExternalMidi1Lowered::messages)>::value;
  if (max_events < kMaxLoweredMessages) {
    return run_validation([&] {
      util::check::refuse("max_events", "must be 0 or an integer of at least " +
                                            std::to_string(kMaxLoweredMessages));
    });
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return refuse_not_compiled_in(Feature::kArrangement, max_events);
#else
  SONARE_C_TRY
  size_t written = 0;
  // Lower one queue record at a time, consuming it only when its lowered MIDI-1
  // messages all fit in the remaining output capacity, so the destructive drain
  // never loses a record that could not be emitted.
  while (written + kMaxLoweredMessages <= max_events) {
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
