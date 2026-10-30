/// @file realtime_engine_midi.cpp
/// @brief Embind realtime-engine facade: MIDI instruments, control & events.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "c_api/midi_fx_json.h"
#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/midi_fx.h"
#include "realtime_engine_wasm.h"
#include "util/zero_is_default.h"
#include "wasm/bindings/common/synth_patch_val.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/sample_bank_internal.h"
#include "wasm/bindings/common/sample_bank_wasm.h"
#endif

namespace {

void wasmMidiFxChainFromJson(const std::string& config_json, sonare::midi::MidiFxChain* chain) {
  const SonareError error =
      sonare_c_detail::midi_fx_chain_from_json(wasmCString(config_json, "configJson"), chain);
  if (error == SONARE_ERROR_INVALID_FORMAT) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "invalid MIDI-FX JSON");
  }
  if (error != SONARE_OK) throw WasmRangeError("invalid MIDI-FX configuration");
}

}  // namespace

void RealtimeEngineWasm::setBuiltinInstrument(const val& destination_id_val, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::BuiltinSynthConfig cfg;
  if (!config.isUndefined() && !config.isNull()) {
    if (hasProperty(config, "waveform")) {
      cfg.waveform =
          static_cast<sonare::midi::SynthWaveform>(builtinWaveformFromVal(config["waveform"]));
    }
    // clamp_synth_config reads a non-positive or non-finite field as "use the
    // built-in default" (see positive_or_default in midi/builtin_synth.cpp) and
    // reports nothing, so a value handed through came back as a successful call
    // at a level nobody chose. 0 stays the way to ask for the default.
    cfg.gain = sonare::ZeroIsDefault(floatProperty(config, "gain", 0.0f))
                   .checked_non_negative(0.0f, "gain");
    cfg.attack_ms = sonare::ZeroIsDefault(floatProperty(config, "attackMs", 0.0f))
                        .checked_non_negative(0.0f, "attackMs");
    cfg.decay_ms = sonare::ZeroIsDefault(floatProperty(config, "decayMs", 0.0f))
                       .checked_non_negative(0.0f, "decayMs");
    cfg.sustain = sonare::ZeroIsDefault(floatProperty(config, "sustain", 0.0f))
                      .checked_non_negative(0.0f, "sustain");
    cfg.release_ms = sonare::ZeroIsDefault(floatProperty(config, "releaseMs", 0.0f))
                         .checked_non_negative(0.0f, "releaseMs");
    const int polyphony = intProperty(config, "polyphony", 0);
    if (polyphony < 0) {
      throw WasmRangeError("polyphony must be 0 (the library default) or a positive voice count");
    }
    cfg.polyphony = polyphony;
  }
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(sonare::midi::clamp_synth_config(cfg));
  bindInstrument(destination_id, std::move(synth));
#else
  (void)destination_id_val;
  (void)config;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::setMidiClips(val clips_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t count = static_cast<uint32_t>(wasmArrayLikeLength(clips_val, "MIDI clips"));
  std::vector<sonare::midi::MidiClipSchedule> clips;
  clips.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    val clip_val = clips_val[i];
    sonare::midi::MidiClipSchedule clip;
    clip.id = uintProperty(clip_val, "id", 0);
    clip.track_id = uintProperty(clip_val, "trackId", 0);
    clip.start_sample = int64Property(clip_val, "startSample", 0);
    clip.start_ppq = doubleProperty(clip_val, "startPpq", 0.0);
    // Match the C ABI: reject a non-finite clip start (WASM bypasses the C-ABI
    // guard), otherwise ppq->sample placement is undefined in the sequencer.
    if (!std::isfinite(clip.start_ppq)) {
      throw WasmRangeError("setMidiClips: clip startPpq must be finite");
    }
    clip.length_samples = int64Property(clip_val, "lengthSamples", 0);
    // Linear gain envelope over the destination's rendered audio (matches
    // SonareEngineMidiClipSchedule.gain / fade_in_samples / fade_out_samples).
    // Absent gain defaults to unity, absent fades default to none.
    clip.gain = floatProperty(clip_val, "gain", 1.0f);
    if (!(std::isfinite(clip.gain) && clip.gain >= 0.0f)) {
      throw WasmRangeError("setMidiClips: clip gain must be a finite non-negative number");
    }
    clip.fade_in_samples = int64Property(clip_val, "fadeInSamples", 0);
    clip.fade_out_samples = int64Property(clip_val, "fadeOutSamples", 0);
    if (clip.fade_in_samples < 0 || clip.fade_out_samples < 0) {
      throw WasmRangeError("setMidiClips: clip fade lengths must be non-negative");
    }
    // An open-ended clip (length_samples <= 0) has no end to fade out towards.
    if (clip.fade_out_samples > 0 && clip.length_samples <= 0) {
      throw WasmRangeError("setMidiClips: fade-out requires a bounded clip length");
    }
    clip.loop_mode = boolProperty(clip_val, "loop", false) ? sonare::midi::MidiLoopMode::kLoop
                                                           : sonare::midi::MidiLoopMode::kOneShot;
    clip.loop_length_samples = int64Property(clip_val, "loopLengthSamples", 0);
    clip.destination_id = uintProperty(clip_val, "destinationId", clip.track_id);
    val events_val = clip_val["events"];
    const uint32_t event_count = static_cast<uint32_t>(wasmArrayLikeLength(events_val, "events"));
    clip.events.reserve(event_count);
    for (uint32_t j = 0; j < event_count; ++j) {
      val event_val = events_val[j];
      sonare::midi::MidiEvent event;
      event.render_frame = int64Property(event_val, "renderFrame", 0);
      sonare::midi::Ump ump;
      ump.words[0] = wordProperty(event_val, "word0", wordProperty(event_val, "data0", 0));
      ump.words[1] = wordProperty(event_val, "word1", wordProperty(event_val, "data1", 0));
      ump.words[2] = wordProperty(event_val, "word2", 0);
      ump.words[3] = wordProperty(event_val, "word3", 0);
      const uint32_t word_count = uintProperty(event_val, "wordCount", 0);
      if (word_count >= 1 && word_count <= 4) {
        ump.word_count = static_cast<uint8_t>(word_count);
      } else if (ump.words[3] != 0) {
        ump.word_count = 4;
      } else if (ump.words[2] != 0) {
        ump.word_count = 3;
      } else if (ump.words[1] != 0) {
        ump.word_count = 2;
      } else {
        ump.word_count = 1;
      }
      // `group` is redundant with word0 bits 24..27 and defaults to 0 here, so
      // an event authored only through word0 would otherwise arrive with the two
      // disagreeing. word0 wins, matching the C ABI; the supplied value is still
      // range-checked so a malformed event is rejected rather than masked.
      //
      // THIS IS NOT THE ENFORCING SITE. MidiSequencer::set_midi_clips re-derives
      // the group for every publisher, and that is the line the invariant
      // actually rests on -- reverting it turns tests red, while reverting the
      // assignment below leaves every test green because the sequencer
      // normalizes afterwards. The assignment is kept so this boundary
      // conversion yields a self-consistent Ump on its own terms, not because
      // anything downstream depends on it. Anyone changing the sequencer's
      // derivation must not read this line as coverage.
      const uint32_t group = uintProperty(event_val, "group", 0);
      if (group > 15) {
        throw WasmRangeError("setMidiClips: event group must be in [0,15]");
      }
      ump.group = sonare::midi::ump_group_from_word0(ump.words[0]);
      ump.sysex_handle = uintProperty(event_val, "sysexHandle", 0);
      event.ump = ump;
      clip.events.push_back(event);
    }
    // Stable sort with the off-before-on tiebreak so a same-frame re-trigger
    // releases before re-attacking, matching the offline clip path.
    sonare::midi::sort_render_events_stable(clip.events);
    clips.push_back(std::move(clip));
  }
  engine_.set_midi_clips(std::move(clips));
#else
  (void)clips_val;
  throwMidiEngineUnavailable();
#endif
}

// Binds the patch-driven NativeSynth (the full synthesizer) on a realtime
// MIDI destination. patch is a SynthPatch object or a preset-name string
// ("saw-lead" / "va:saw-lead"), resolving exactly like
// Project.bounceWithSynthInstrument. Unknown preset names throw. A sample patch
// carries its bank as `sampleBankId`, the same key the bounce reads; the synth
// takes a share, so the caller may release its handle right afterwards.
void RealtimeEngineWasm::setSynthInstrument(const val& destination_id_val, val patch) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  // Owns the keys behind c_patch.engine_params; alive across the config call.
  const sonare_wasm_synth::SynthPatchStorage patch_storage =
      sonare_wasm_synth::synthPatchFromVal(patch);
  const SonareSynthPatch& c_patch = patch_storage.patch;
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(c_patch, &cfg, &error)) {
    throw WasmRangeError(error != nullptr ? error : "invalid synth patch");
  }
  // `useGmPrograms` is a binding convenience beside the patch fields. Keep
  // its strict boolean contract here, where realtime and offline synth
  // bindings meet, and leave the NativeSynth default disabled when omitted.
  cfg.use_gm_programs = boolProperty(patch, "useGmPrograms", false);
  // Same injection as setSf2Instrument: the rig stages need the mastering inserts.
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  SonareSampleBank* bank = SampleBankWasm::fromDescriptor(patch);
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  if (bank != nullptr) {
    synth->set_sample_bank(std::shared_ptr<const sonare::midi::synth::SampleBank>(bank->bank));
  }
  bindInstrument(destination_id, std::move(synth));
#else
  (void)destination_id_val;
  (void)patch;
  throwMidiEngineUnavailable();
#endif
}

// Resolves a hosted instrument's continuous parameter (JSON-key name, e.g.
// "cutoffHz") to the reserved instrument-automation id passed to
// setAutomationLane / setParameter. A destination with no bound instrument, an
// instrument exposing no automatable parameters, or an unknown key is a RangeError. Like the insert
// resolvers, the id is returned as a double so the full 32-bit unsigned reserved id survives the JS
// boundary.
double RealtimeEngineWasm::resolveInstrumentAutomationId(const val& destination_id_val,
                                                         const std::string& param_name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  return resolvedAutomationId(engine_.resolve_instrument_automation_id(destination_id, param_name),
                              param_name);
#else
  (void)destination_id_val;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement support is not compiled in");
#endif
}

// Loads (parses) SoundFont 2 bytes into the engine so SF2 instruments can be
// bound with setSf2Instrument. The host copies the .sf2 bytes into linear
// memory as a Uint8Array; they are not referenced after the call. Replaces
// any previously loaded SoundFont (already-bound SF2 players keep the
// SoundFont they were created with).
void RealtimeEngineWasm::loadSoundFont(val data) {
#if defined(SONARE_WITH_ARRANGEMENT)
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  // Empty input is a parameter error and oversized or unparsable bytes a format
  // error, as in sonare_engine_load_soundfont; parse() applies the same SF2 file
  // size limit, and the engine's SoundFont is replaced only after it succeeds.
  if (bytes.empty()) {
    throw WasmRangeError("SoundFont data must not be empty");
  }
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(bytes.data(), bytes.size(), &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "failed to load SoundFont: " + error);
  }
  soundfont_ = std::move(soundfont);
#else
  (void)data;
  throwMidiEngineUnavailable();
#endif
}

// Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
// destination, fed by the engine's loaded SoundFont. Without a loaded
// SoundFont the player's NativeSynth GM fallback is the data-free floor
// (live MIDI stays audible). config is { gain?, polyphony? }
// ("0 / omit => default").
void RealtimeEngineWasm::setSf2Instrument(const val& destination_id_val, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (!config.isUndefined() && !config.isNull()) {
    // 0 selects the player's own default; a value the player would replace in
    // silence is refused here instead.
    cfg.gain = sonare::ZeroIsDefault(floatProperty(config, "gain", 0.0f))
                   .checked_non_negative(cfg.gain, "gain");
    const int polyphony = intProperty(config, "polyphony", 0);
    if (polyphony < 0) {
      throw WasmRangeError("polyphony must be 0 (the library default) or a positive voice count");
    }
    if (polyphony != 0) cfg.polyphony = polyphony;
    cfg.prefer_model_for_modeled_families =
        boolProperty(config, "preferModelForModeledFamilies", false);
    cfg.bank_rig_binding = !boolProperty(config, "clearBankRig", false);
    if (gsEfxRealizationProperty(config, "gsEfxRealization") == 1) {
      cfg.gs_efx_realization = sonare::midi::synth::GsEfxRealization::kClassic;
    }
  }
  // Inject the mastering insert factory so live GS insertion effects (EFX)
  // realise their processing chain on the control thread (mirrors the C-ABI
  // path). realize_efx_inline stays false (the live default) so the swap is
  // wait-free via the RtPublisher snapshot; without a factory the chain could
  // not be built and live EFX would be silent.
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  auto player = std::make_unique<sonare::midi::synth::Sf2Player>(cfg);
  player->set_soundfont(soundfont_);
  bindInstrument(destination_id, std::move(player));
#else
  (void)destination_id_val;
  (void)config;
  throwMidiEngineUnavailable();
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// Mirrors the C ABI's bind mapping: a refused instrument is a RangeError; a full
// rack, or a scheduled or queued SysEx the new instrument refuses, is
// InvalidState; a failed allocation is OutOfMemory.
[[noreturn]] void throwMidiInstrumentBindFailure(const sonare::engine::RealtimeEngine& engine,
                                                 const char* message) {
  switch (engine.last_midi_instrument_bind_status()) {
    case sonare::engine::MidiInstrumentBindStatus::kAlreadyBoundElsewhere:
    case sonare::engine::MidiInstrumentBindStatus::kInstrumentPrepareFailed:
      throw WasmRangeError(message);
    case sonare::engine::MidiInstrumentBindStatus::kRackFull:
    case sonare::engine::MidiInstrumentBindStatus::kPreparationFailed:
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, message);
    default:
      throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, message);
  }
}

}  // namespace

// Binds (or replaces) an engine-owned instrument on a destination, keeping
// the ownership table and the engine's instrument rack in sync. Shared by
// the built-in synth and SF2 instrument entries.
void RealtimeEngineWasm::bindInstrument(uint32_t destination_id,
                                        std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : builtin_instruments_) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine_.set_midi_instrument(destination_id, raw)) {
        throwMidiInstrumentBindFailure(engine_, "failed to bind MIDI instrument");
      }
      entry.second = std::move(instrument);
      return;
    }
  }
  builtin_instruments_.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = builtin_instruments_.back().second.get();
  if (!engine_.set_midi_instrument(destination_id, raw)) {
    builtin_instruments_.pop_back();
    throwMidiInstrumentBindFailure(engine_, "failed to bind MIDI instrument");
  }
}
#endif

void RealtimeEngineWasm::clearMidiInstrument(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  // A failed clear leaves the instrument bound, so its owner must stay alive.
  if (!engine_.set_midi_instrument(destination_id, nullptr)) {
    throwMidiInstrumentBindFailure(engine_, "failed to clear MIDI instrument");
  }
  builtin_instruments_.erase(
      std::remove_if(builtin_instruments_.begin(), builtin_instruments_.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      builtin_instruments_.end());
#else
  (void)destination_id_val;
#endif
}

size_t RealtimeEngineWasm::midiInstrumentCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.midi_instrument_count();
#else
  return 0;
#endif
}

void RealtimeEngineWasm::setMidiFx(const val& destination_id_val, const std::string& config_json) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::MidiFxChain chain;
  wasmMidiFxChainFromJson(config_json, &chain);
  if (!engine_.set_midi_fx(destination_id, chain)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to install MIDI-FX insert");
  }
#else
  (void)destination_id_val;
  (void)config_json;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::clearMidiFx(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.clear_midi_fx(checkedUintFromVal(destination_id_val, "destinationId"));
#else
  (void)destination_id_val;
#endif
}

void registerRealtimeEngineMidi(class_<RealtimeEngineWasm>& cls) {
  cls.function("setMidiClips", &RealtimeEngineWasm::setMidiClips)
      .function("setBuiltinInstrument", &RealtimeEngineWasm::setBuiltinInstrument)
      .function("setSynthInstrument", &RealtimeEngineWasm::setSynthInstrument)
      .function("resolveInstrumentAutomationId", &RealtimeEngineWasm::resolveInstrumentAutomationId)
      .function("loadSoundFont", &RealtimeEngineWasm::loadSoundFont)
      .function("setSf2Instrument", &RealtimeEngineWasm::setSf2Instrument)
      .function("clearMidiInstrument", &RealtimeEngineWasm::clearMidiInstrument)
      .function("midiInstrumentCount", &RealtimeEngineWasm::midiInstrumentCount)
      .function("setMidiFx", &RealtimeEngineWasm::setMidiFx)
      .function("clearMidiFx", &RealtimeEngineWasm::clearMidiFx);
}

#endif  // __EMSCRIPTEN__
