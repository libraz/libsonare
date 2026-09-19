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
#include "midi/articulation_mode.h"
#include "midi/controller_profile.h"
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
  const SonareError error = sonare_c_detail::midi_fx_chain_from_json(config_json.c_str(), chain);
  if (error == SONARE_ERROR_INVALID_FORMAT) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "invalid MIDI-FX JSON");
  }
  if (error != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI-FX configuration");
  }
}

#if defined(SONARE_WITH_ARRANGEMENT)

// The destination's bound instrument. WASM links neither the controller nor the
// engine-MIDI C-ABI unit, so their shared first refusal is reproduced here:
// InvalidParameter for a destination nothing is bound to, which every entry
// below has to keep apart from the instrument's own "no such capability".
sonare::midi::MidiInstrument* wasmBoundInstrument(const sonare::engine::RealtimeEngine& engine,
                                                  uint32_t destination_id) {
  sonare::midi::MidiInstrument* instrument = engine.midi_instrument(destination_id);
  if (instrument == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "no MIDI instrument is bound to this destination");
  }
  return instrument;
}

// The destination's current profile. NotImplemented (the C ABI's NOT_SUPPORTED)
// for a bound instrument that holds no profile.
sonare::midi::ControllerProfile wasmControllerProfile(const sonare::engine::RealtimeEngine& engine,
                                                      uint32_t destination_id) {
  const sonare::midi::ControllerProfile* profile =
      wasmBoundInstrument(engine, destination_id)->controller_profile();
  if (profile == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument holds no controller profile");
  }
  return *profile;
}

// Installs a profile back onto the destination. Every entry reads, changes one
// thing and installs, so the rule that installing drops the channels'
// accumulated axis values holds however the profile was reached.
void wasmInstallControllerProfile(const sonare::engine::RealtimeEngine& engine,
                                  uint32_t destination_id,
                                  const sonare::midi::ControllerProfile& profile) {
  if (!wasmBoundInstrument(engine, destination_id)->set_controller_profile(profile)) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument declined a controller profile");
  }
}

#endif  // SONARE_WITH_ARRANGEMENT

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
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "polyphony must be 0 (the library default) or a positive voice count");
    }
    cfg.polyphony = polyphony;
  }
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(sonare::midi::clamp_synth_config(cfg));
  bindInstrument(destination_id, std::move(synth));
#else
  (void)destination_id_val;
  (void)config;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "setMidiClips: clip startPpq must be finite");
    }
    clip.length_samples = int64Property(clip_val, "lengthSamples", 0);
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
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "setMidiClips: event group must be in [0,15]");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
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
  const SonareSynthPatch c_patch = sonare_wasm_synth::synthPatchFromVal(patch);
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(c_patch, &cfg, &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  error != nullptr ? error : "invalid synth patch");
  }
  SonareSampleBank* bank = SampleBankWasm::fromDescriptor(patch);
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  if (bank != nullptr) {
    synth->set_sample_bank(std::shared_ptr<const sonare::midi::synth::SampleBank>(bank->bank));
  }
  bindInstrument(destination_id, std::move(synth));
#else
  (void)destination_id_val;
  (void)patch;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Resolves a hosted instrument's continuous parameter (JSON-key name, e.g.
// "cutoffHz") to the reserved instrument-automation id passed to
// setAutomationLane / setParameter. Returns -1 when the destination has no
// bound instrument, the instrument exposes no automatable parameters, or the
// key is unknown. Like the insert resolvers, the id is returned as a double so
// the full 32-bit unsigned reserved id survives the JS boundary.
double RealtimeEngineWasm::resolveInstrumentAutomationId(const val& destination_id_val,
                                                         const std::string& param_name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  return static_cast<double>(engine_.resolve_instrument_automation_id(destination_id, param_name));
#else
  (void)destination_id_val;
  (void)param_name;
  return -1.0;
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
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (bytes.empty() || !soundfont->parse(bytes.data(), bytes.size(), &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "failed to load SoundFont: " + error);
  }
  soundfont_ = std::move(soundfont);
#else
  (void)data;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
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
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "polyphony must be 0 (the library default) or a positive voice count");
    }
    if (polyphony != 0) cfg.polyphony = polyphony;
    cfg.prefer_model_for_modeled_families =
        boolProperty(config, "preferModelForModeledFamilies", false);
    cfg.bank_rig_binding = !boolProperty(config, "clearBankRig", false);
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
// Binds (or replaces) an engine-owned instrument on a destination, keeping
// the ownership table and the engine's instrument rack in sync. Shared by
// the built-in synth and SF2 instrument entries.
void RealtimeEngineWasm::bindInstrument(uint32_t destination_id,
                                        std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : builtin_instruments_) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine_.set_midi_instrument(destination_id, raw)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                      "failed to bind MIDI instrument");
      }
      entry.second = std::move(instrument);
      return;
    }
  }
  builtin_instruments_.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = builtin_instruments_.back().second.get();
  if (!engine_.set_midi_instrument(destination_id, raw)) {
    builtin_instruments_.pop_back();
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to bind MIDI instrument");
  }
}
#endif

void RealtimeEngineWasm::clearMidiInstrument(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  engine_.set_midi_instrument(destination_id, nullptr);
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

void RealtimeEngineWasm::bindMidiCc(const val& channel_val, const val& controller_val,
                                    const val& param_id_val, const val& min_value_val,
                                    const val& max_value_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int controller = checkedIntFromVal(controller_val, "controller");
  const uint32_t param_id = checkedUintFromVal(param_id_val, "paramId");
  const float min_value = checkedFloatFromVal(min_value_val, "minValue");
  const float max_value = checkedFloatFromVal(max_value_val, "maxValue");
  if (channel < 0 || channel > 15 || controller < 0 || controller > 127 || param_id == 0 ||
      max_value < min_value) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "bindMidiCc: channel in [0,15], controller in [0,127], paramId "
                                  "non-zero, maxValue >= minValue");
  }
  if (!engine_.bind_midi_cc(static_cast<uint8_t>(controller), static_cast<uint8_t>(channel),
                            param_id, min_value, max_value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to bind MIDI CC");
  }
#else
  (void)channel_val;
  (void)controller_val;
  (void)param_id_val;
  (void)min_value_val;
  (void)max_value_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::bindMidiCcBinding(val object) {
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::midi::CcBinding binding{};
  binding.cc_number = checkedByteFromVal(object["ccNumber"], "ccNumber");
  const val channel = object["channel"];
  binding.channel = channel.isUndefined() || channel.isNull()
                        ? sonare::midi::kCcAnyChannel
                        : checkedByteFromVal(channel, "channel");
  // Only ccNumber and paramId are required; every other field falls back to the
  // CcBinding default (any channel, 7-bit Control Change, unit output range),
  // matching the Node addon reader and the Project-side descriptor reader. A
  // field that IS supplied is still range-checked below, so the leniency covers
  // omission only and a non-finite range is still rejected.
  binding.kind = static_cast<sonare::midi::CcBindingKind>(byteProperty(object, "kind", 0u));
  binding.cc_lsb_number = byteProperty(object, "ccLsbNumber", 0u);
  binding.selector_msb = byteProperty(object, "selectorMsb", 0u);
  binding.selector_lsb = byteProperty(object, "selectorLsb", 0u);
  binding.param_id = checkedUintFromVal(object["paramId"], "paramId");
  binding.min_value = floatProperty(object, "minValue", 0.0f);
  binding.max_value = floatProperty(object, "maxValue", 1.0f);
  if (binding.cc_number > 127 || binding.param_id == 0 ||
      static_cast<uint8_t>(binding.kind) >
          static_cast<uint8_t>(sonare::midi::CcBindingKind::kNrpn) ||
      (binding.channel != sonare::midi::kCcAnyChannel && binding.channel > 15) ||
      !std::isfinite(binding.min_value) || !std::isfinite(binding.max_value) ||
      binding.max_value < binding.min_value ||
      (binding.kind == sonare::midi::CcBindingKind::kControlChange14 &&
       (binding.cc_number > 31 || binding.cc_lsb_number != binding.cc_number + 32u))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid full MIDI CC binding");
  }
  if (!engine_.bind_midi_cc(binding)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to bind full MIDI CC descriptor");
  }
#else
  (void)object;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::clearMidiCcBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.clear_midi_cc_bindings();
#endif
}

size_t RealtimeEngineWasm::midiCcBindingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.midi_cc_binding_count();
#else
  return 0;
#endif
}

// Replaces the destination instrument's controller profile with a named preset
// (controllerProfileNames lists them). Mirrors the C ABI
// sonare_engine_set_controller_profile: the name is validated before the engine
// is touched, so an unknown preset reads as a bad argument whether or not the
// destination has an instrument, and is never resolved to a default -- a
// default that silently replaced the device's spelling would still play, just
// not the gestures that were sent.
void RealtimeEngineWasm::setControllerProfile(const val& destination_id_val,
                                              const std::string& preset_name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::ControllerProfile profile;
  if (!sonare::midi::ControllerProfile::preset(preset_name, &profile)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unknown controller profile preset: '" + preset_name + "'");
  }
  wasmInstallControllerProfile(engine_, destination_id, profile);
#else
  (void)destination_id_val;
  (void)preset_name;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Adds one binding on top of the destination instrument's current profile.
// `binding` is { input, index?, axis, lo?, hi?, curve? }; input and axis are the
// canonical enum names (or their ordinals). Mirrors
// sonare_engine_bind_controller, including which refusals come before the
// destination is looked up.
void RealtimeEngineWasm::bindController(const val& destination_id_val, val binding) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::ControllerBinding entry;
  // Ordinals out of range are refused rather than clamped: a value the caller
  // meant as "poly pressure" arriving as "control change" is a binding that
  // works and listens to the wrong thing.
  // Required rather than defaulted: ordinal 0 is a working value on both enums
  // (`control-change`, `none`), so an omitted key would bind something the
  // caller never asked for.
  int input = 0;
  int axis = 0;
  sonare_wasm_synth::requiredEnumProperty(binding, "input", sonare_wasm_synth::kControllerInputs,
                                          SONARE_CONTROLLER_INPUT_COUNT, "controller input",
                                          &input);
  sonare_wasm_synth::requiredEnumProperty(binding, "axis", sonare_wasm_synth::kControllerAxes,
                                          SONARE_CONTROLLER_AXIS_COUNT, "controller axis", &axis);
  entry.input = static_cast<sonare::midi::ControllerInput>(input);
  entry.axis = static_cast<sonare::midi::ControllerAxis>(axis);
  const int index = typedIntProperty(binding, "index", 0);
  requireOrdinalInRange(index, 0, 127, "controller binding index");
  entry.index = static_cast<uint8_t>(index);
  const float lo = typedFloatProperty(binding, "lo", 0.0f);
  const float hi = typedFloatProperty(binding, "hi", 1.0f);
  const float curve = typedFloatProperty(binding, "curve", 1.0f);

  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  // A non-finite range or curve would reach the audio thread and stay there, so
  // it is refused here rather than substituted: a mapping silently replaced by a
  // default is a mapping the caller believes it installed.
  if (!std::isfinite(lo) || !std::isfinite(hi)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "binding range must be finite");
  }
  if (!std::isfinite(curve) || curve <= 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "binding curve must be finite and positive");
  }
  entry.lo = lo;
  entry.hi = hi;
  entry.curve = curve;

  if (!profile.bind(entry)) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "controller binding refused: the table is full, the axis is none, or a poly-pressure "
        "binding named a channel-level axis");
  }
  wasmInstallControllerProfile(engine_, destination_id, profile);
#else
  (void)destination_id_val;
  (void)binding;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Drops every binding of the destination instrument's controller profile. The
// instrument keeps a profile; it resolves nothing until something is bound
// again.
void RealtimeEngineWasm::clearControllerBindings(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  profile.clear();
  wasmInstallControllerProfile(engine_, destination_id, profile);
#else
  (void)destination_id_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

size_t RealtimeEngineWasm::controllerBindingCount(const val& destination_id_val) const {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  return wasmControllerProfile(engine_, destination_id).binding_count();
#else
  (void)destination_id_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Whether note-on velocity is expression for this instrument. No fixed default
// is possible -- a wind controller ships sending breath-derived velocity on one
// model and a constant on the next -- so each preset states it and a host
// building its own profile sets it.
void RealtimeEngineWasm::setControllerVelocityMeaningful(const val& destination_id_val,
                                                         bool meaningful) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  profile.velocity_meaningful = meaningful;
  wasmInstallControllerProfile(engine_, destination_id, profile);
#else
  (void)destination_id_val;
  (void)meaningful;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

bool RealtimeEngineWasm::controllerVelocityMeaningful(const val& destination_id_val) const {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  return wasmControllerProfile(engine_, destination_id).velocity_meaningful;
#else
  (void)destination_id_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Sets how one channel of the destination's instrument treats a note-on while
// another note on that channel is still held. `articulation` is a canonical
// name ("poly" / "mono-retrigger" / "mono-legato") or its ordinal. Mirrors
// sonare_engine_set_articulation, including which refusals stay apart: an
// instrument with no articulation of its own reports NotImplemented (the C ABI's
// NOT_SUPPORTED) rather than succeeding quietly, because a discarded mode is
// indistinguishable from one that took until two notes overlap.
void RealtimeEngineWasm::setArticulation(const val& destination_id_val, const val& channel_val,
                                         val articulation) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  // Checked before the byte it narrows into: channel 256 would otherwise arrive
  // as channel 0 and slur a part the caller never named. Out-of-range modes are
  // refused rather than clamped by the shared enum reader, for the reason the C
  // ABI gives -- poly substituted for a misspelled mono-legato plays every note
  // and slurs none of them.
  const int channel = checkedIntFromVal(channel_val, "channel");
  requireOrdinalInRange(channel, 0, 15, "channel");
  const int mode = sonare_wasm_synth::enumFromVal(articulation, sonare_wasm_synth::kArticulations,
                                                  SONARE_ARTICULATION_COUNT, "articulation");
  if (!wasmBoundInstrument(engine_, destination_id)
           ->set_articulation(static_cast<uint8_t>(channel),
                              static_cast<sonare::midi::ArticulationMode>(mode))) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument has no articulation of its own");
  }
#else
  (void)destination_id_val;
  (void)channel_val;
  (void)articulation;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Reads back setArticulation, as the canonical name. Spelled the way every
// other enum leaves this surface (synthPatchToVal), so a value handed back can
// be passed straight to the setter.
val RealtimeEngineWasm::articulation(const val& destination_id_val, const val& channel_val) const {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int channel = checkedIntFromVal(channel_val, "channel");
  requireOrdinalInRange(channel, 0, 15, "channel");
  sonare::midi::ArticulationMode mode = sonare::midi::ArticulationMode::kPoly;
  if (!wasmBoundInstrument(engine_, destination_id)
           ->articulation(static_cast<uint8_t>(channel), &mode)) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument has no articulation of its own");
  }
  return sonare_wasm_synth::enumNameVal(static_cast<int>(mode), sonare_wasm_synth::kArticulations,
                                        SONARE_ARTICULATION_COUNT);
#else
  (void)destination_id_val;
  (void)channel_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// How many times a legato continuation was asked for and refused, so the note
// started a voice of its own instead. An instrument with no articulation of its
// own reports NotImplemented on the same terms as the two entries above rather
// than answering 0: this counter exists because a refusal sounds like an
// ordinary note, and a 0 from an instrument that was never asked reads as
// "every slur took" -- the exact reading it is here to prevent. Saturated at
// UINT32_MAX rather than wrapped, matching sonare_engine_legato_fallback_count,
// so the same phrase reports the same number on every surface.
uint32_t RealtimeEngineWasm::legatoFallbackCount(const val& destination_id_val) const {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  uint64_t counted = 0;
  if (!wasmBoundInstrument(engine_, destination_id)->legato_fallback_count(&counted)) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument has no articulation of its own");
  }
  return counted > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(counted);
#else
  (void)destination_id_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::clearMidiFx(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.clear_midi_fx(checkedUintFromVal(destination_id_val, "destinationId"));
#else
  (void)destination_id_val;
#endif
}

void RealtimeEngineWasm::setMidiInputSource(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_input_source(&midi_input_source_,
                                checkedUintFromVal(destination_id_val, "destinationId"));
  midi_input_source_enabled_ = true;
#else
  (void)destination_id_val;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::clearMidiInputSource() {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_input_source(nullptr, 0);
  midi_input_source_enabled_ = false;
#endif
}

size_t RealtimeEngineWasm::midiInputPendingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return midi_input_source_.pending_count();
#else
  return 0;
#endif
}

// Route the MIDI of `destination_id` (a track lane) to the external output
// queue instead of the internal instrument rack, so the track plays an
// external device. Clearing it restores internal-synth playback.
void RealtimeEngineWasm::setMidiDestinationExternal(const val& destination_id_val, bool external) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  if (!engine_.set_midi_destination_external(destination_id, external)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "external MIDI destination table is full");
  }
#else
  (void)destination_id_val;
  (void)external;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
// the external output queue so external gear stays tempo-synced.
void RealtimeEngineWasm::setExternalMidiClockEnabled(bool enabled) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_external_midi_clock_enabled(enabled);
#else
  (void)enabled;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Count of external-MIDI events dropped because the output queue was full.
uint32_t RealtimeEngineWasm::externalMidiDroppedCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.external_midi_dropped_count();
#else
  return 0;
#endif
}

size_t RealtimeEngineWasm::externalMidiPendingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.external_midi_pending_count();
#else
  return 0;
#endif
}

// Drain queued external-MIDI events, already lowered to MIDI 1.0 byte
// messages so the host can write them straight to a Web MIDI output port.
// Each returned item is { destinationId, renderFrame, bytes: number[] };
// transport/clock bytes carry destinationId === kTransportDestination
// (0xFFFFFFFF). A single queued channel-voice UMP may lower to more than one
// item (e.g. a MIDI 2.0 program change with bank select). `max_records` caps
// the number of OUTPUT events produced -- the unit shared by every surface.
// To keep that cap lossless we drain one queue record at a time, consuming it
// only while at least 3 output slots (the most one record can lower to) remain
// in the budget; records that do not fit stay queued for the next call. UMP
// types that do not lower to MIDI 1.0 (SysEx/Data, Utility, MIDI-2-only
// controllers) emit no bytes and are skipped.
//
// renderFrame coordinate: channel-voice events use the timeline sample
// position; clock/transport bytes use the monotonic device render frame. They
// coincide during straight playback and diverge across a loop/seek -- see
// RealtimeEngine::drain_external_midi. Reconcile via the telemetry block's
// renderFrame/timelineSample pair when scheduling sample-accurately.
val RealtimeEngineWasm::drainExternalMidi(const val& max_records_val) {
  val out = val::array();
#if defined(SONARE_WITH_ARRANGEMENT)
  const int max_records = checkedIntFromVal(max_records_val, "maxRecords");
  // One queue record lowers to at most this many MIDI-1 messages, so a smaller
  // budget can never consume a record and the drain would report nothing while
  // the queue keeps growing. The bound is read from the shared lowering type so
  // it cannot drift from the lowering rules or from the C ABI's identical guard.
  constexpr int kMaxLoweredMessages =
      static_cast<int>(std::extent<decltype(sonare::host::ExternalMidi1Lowered::messages)>::value);
  if (max_records > 0 && max_records < kMaxLoweredMessages) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "drainExternalMidi: maxRecords must be at least " +
                                      std::to_string(kMaxLoweredMessages) +
                                      " to guarantee forward progress");
  }
  if (max_records <= 0 || engine_.external_midi_pending_count() == 0) return out;
  sonare::host::ExternalMidiRecord record{};
  int out_count = 0;
  while (out_count + kMaxLoweredMessages <= max_records) {
    if (engine_.drain_external_midi(&record, 1) == 0) break;
    // Shared lowering: identical MIDI-1 rules across every host surface. With a
    // full record's worth of slots free, every lowered message fits the budget.
    const sonare::host::ExternalMidi1Lowered lowered =
        sonare::host::lower_external_midi_record(record);
    for (uint8_t m = 0; m < lowered.count; ++m) {
      const sonare::host::ExternalMidi1Message& msg = lowered.messages[m];
      val item = val::object();
      item.set("destinationId", static_cast<double>(record.destination_id));
      item.set("renderFrame", static_cast<double>(record.event.render_frame));
      val arr = val::array();
      for (uint8_t b = 0; b < msg.byte_count; ++b) arr.set(b, msg.bytes[b]);
      item.set("bytes", arr);
      out.set(out_count++, item);
    }
  }
#else
  (void)max_records_val;
#endif
  return out;
}

bool RealtimeEngineWasm::popExternalMidiToScratch() {
#if defined(SONARE_WITH_ARRANGEMENT)
  while (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) {
    if (engine_.drain_external_midi(&external_midi_record_scratch_, 1) == 0) return false;
    external_midi_lowered_scratch_ =
        sonare::host::lower_external_midi_record(external_midi_record_scratch_);
    external_midi_lowered_index_ = 0;
  }
  return true;
#else
  return false;
#endif
}

uint32_t RealtimeEngineWasm::externalMidiScratchDestinationId() const {
  return external_midi_record_scratch_.destination_id;
}

int64_t RealtimeEngineWasm::externalMidiScratchRenderFrame() const {
  return external_midi_record_scratch_.event.render_frame;
}

uint32_t RealtimeEngineWasm::externalMidiScratchByteWord() const {
  if (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) return 0;
  const auto& message = external_midi_lowered_scratch_.messages[external_midi_lowered_index_];
  return static_cast<uint32_t>(message.bytes[0]) | (static_cast<uint32_t>(message.bytes[1]) << 8u) |
         (static_cast<uint32_t>(message.bytes[2]) << 16u);
}

uint32_t RealtimeEngineWasm::externalMidiScratchByteCount() const {
  if (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) return 0;
  return external_midi_lowered_scratch_.messages[external_midi_lowered_index_].byte_count;
}

void RealtimeEngineWasm::consumeExternalMidiScratch() {
  if (external_midi_lowered_index_ < external_midi_lowered_scratch_.count) {
    ++external_midi_lowered_index_;
  }
}

void RealtimeEngineWasm::pushMidiInputNoteOn(const val& group_val, const val& channel_val,
                                             const val& note_val, const val& velocity_val,
                                             int64_t port_time_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiInputEvent(group, channel, note, velocity, port_time_samples, true);
}

void RealtimeEngineWasm::pushMidiInputNoteOff(const val& group_val, const val& channel_val,
                                              const val& note_val, const val& velocity_val,
                                              int64_t port_time_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiInputEvent(group, channel, note, velocity, port_time_samples, false);
}

void RealtimeEngineWasm::pushMidiInputCc(const val& group_val, const val& channel_val,
                                         const val& controller_val, const val& value_val,
                                         int64_t port_time_samples) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int controller = checkedIntFromVal(controller_val, "controller");
  const int value = checkedIntFromVal(value_val, "value");
  if (!midi_input_source_enabled_ || group < 0 || group > 15 || channel < 0 || channel > 15 ||
      controller < 0 || controller > 127 || value < 0 || value > 127) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiInputCc: source enabled, group/channel in [0,15], controller/value in [0,127]");
  }
  if (!midi_input_source_.push_event(
          sonare::midi::make_midi1_control_change(
              static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
              static_cast<uint8_t>(controller), static_cast<uint8_t>(value)),
          port_time_samples)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to enqueue MIDI input CC");
  }
#else
  (void)group_val;
  (void)channel_val;
  (void)controller_val;
  (void)value_val;
  (void)port_time_samples;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::pushMidiNoteOn(const val& destination_id_val, const val& group_val,
                                        const val& channel_val, const val& note_val,
                                        const val& velocity_val, int64_t render_frame) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
               sonare::rt::CommandType::kMidiNoteOnImmediate);
}

void RealtimeEngineWasm::pushMidiNoteOff(const val& destination_id_val, const val& group_val,
                                         const val& channel_val, const val& note_val,
                                         const val& velocity_val, int64_t render_frame) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
               sonare::rt::CommandType::kMidiNoteOffImmediate);
}

// Queues an immediate (live) MIDI control change to a MIDI destination. Mirrors
// the C ABI sonare_engine_push_midi_cc: the synthesized MIDI 1.0 CC reaches the
// registered host instrument at @p render_frame (-1 = immediate). Values are
// 7-bit; channel 0..15, group 0..15. The scalar fields are packed into arg.i
// using the encoding documented in rt/command.h (kMidiCcImmediate).
void RealtimeEngineWasm::pushMidiCc(const val& destination_id_val, const val& group_val,
                                    const val& channel_val, const val& controller_val,
                                    const val& value_val, int64_t render_frame) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int controller = checkedIntFromVal(controller_val, "controller");
  const int value = checkedIntFromVal(value_val, "value");
  if (group < 0 || group > 15 || channel < 0 || channel > 15 || controller < 0 ||
      controller > 127 || value < 0 || value > 127) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiCc: group/channel in [0,15], controller/value in [0,127]");
  }
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiCcImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue MIDI CC command");
  }
}

// Queues one single-word MIDI 1.0 channel-voice UMP to a MIDI destination at
// @p render_frame (-1 = immediate).
//
// A control-change word reaches the CC binding table exactly as it would through
// pushMidiCc or a live input source: the engine resolves every live entry point
// through one kind-aware decoder, so a controller bound to automation is driven
// whichever call the host used. It used to reach the sequencer only, which made
// this the one live path that silently skipped the CC -> automation mapping.
void RealtimeEngineWasm::pushMidiUmp(const val& destination_id_val, const val& word0_val,
                                     int64_t render_frame) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  // A UMP word is idiomatically spelled `(0x2 << 28) | …` in JS, which is a
  // signed int once bit 31 is set, so the whole 32-bit range is legal here.
  const uint32_t word0 = checkedWordFromVal(word0_val, "word0");
  if (((word0 >> 28) & 0x0Fu) != 0x2u) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiUmp: only single-word MIDI 1.0 channel-voice UMP messages are supported");
  }
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiUmpImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(word0);
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue MIDI UMP command");
  }
}

// Queues an immediate (live) MIDI SysEx frame to a MIDI destination. @p data is
// the full message including the leading 0xF0 and trailing 0xF7, 1 byte up to
// RealtimeEngine::kMaxSysExPayloadBytes; its bytes are copied out of the
// Uint8Array before the call returns. Reaches the registered host instrument at
// @p render_frame (-1 = immediate). Mirrors the C ABI
// sonare_engine_push_midi_sysex.
void RealtimeEngineWasm::pushMidiSysex(const val& destination_id_val, val data,
                                       int64_t render_frame) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  // Distinguish the two rejection classes the C ABI reports (it bypasses the
  // C-ABI translation unit here, so the mapping is reproduced): malformed or
  // oversized requests are InvalidParameter, while a full command queue is
  // transient OutOfMemory back-pressure. The ceiling is the engine's own
  // constant, not a copy of its value, so raising it moves this guard with it.
  constexpr size_t kMaxSysExBytes = sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes;
  if (bytes.empty() || bytes.size() > kMaxSysExBytes) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiSysex: data must contain 1.." + std::to_string(kMaxSysExBytes) + " bytes");
  }
  if (!engine_.push_midi_sysex(destination_id, bytes.data(), bytes.size(), render_frame)) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                  "failed to queue MIDI SysEx command");
  }
}

// Queues a MIDI panic (all-notes-off) releasing every sounding note at
// @p render_frame (-1 = immediate). Mirrors the C ABI
// sonare_engine_push_midi_panic.
void RealtimeEngineWasm::pushMidiPanic(int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = render_frame;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue MIDI panic command");
  }
}

void RealtimeEngineWasm::pushMidiNote(uint32_t destination_id, int group, int channel, int note,
                                      int velocity, int64_t render_frame,
                                      sonare::rt::CommandType type) {
  if (group < 0 || group > 15 || channel < 0 || channel > 15 || note < 0 || note > 127 ||
      velocity < 0 || velocity > 127) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiNote: group/channel in [0,15], note/velocity in [0,127]");
  }
  const uint64_t packed = static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  sonare::rt::Command command{};
  command.type = type;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue MIDI note command");
  }
}

void RealtimeEngineWasm::pushMidiInputEvent(int group, int channel, int note, int velocity,
                                            int64_t port_time_samples, bool note_on) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!midi_input_source_enabled_ || group < 0 || group > 15 || channel < 0 || channel > 15 ||
      note < 0 || note > 127 || velocity < 0 || velocity > 127) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "pushMidiInputNote: source enabled, group/channel in [0,15], note/velocity in [0,127]");
  }
  const sonare::midi::Ump ump =
      note_on ? sonare::midi::make_midi1_note_on(
                    static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(note), static_cast<uint8_t>(velocity))
              : sonare::midi::make_midi1_note_off(
                    static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(note), static_cast<uint8_t>(velocity));
  if (!midi_input_source_.push_event(ump, port_time_samples)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to enqueue MIDI input note");
  }
#else
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  (void)note_on;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
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
      .function("bindMidiCc", &RealtimeEngineWasm::bindMidiCc)
      .function("bindMidiCcBinding", &RealtimeEngineWasm::bindMidiCcBinding)
      .function("clearMidiCcBindings", &RealtimeEngineWasm::clearMidiCcBindings)
      .function("midiCcBindingCount", &RealtimeEngineWasm::midiCcBindingCount)
      // destinationId first, matching the C ABI's own argument order minus the
      // engine handle -- stated because embind argument order on this surface
      // has historically diverged from the siblings rather than followed them.
      .function("setControllerProfile", &RealtimeEngineWasm::setControllerProfile)
      .function("bindController", &RealtimeEngineWasm::bindController)
      .function("clearControllerBindings", &RealtimeEngineWasm::clearControllerBindings)
      .function("controllerBindingCount", &RealtimeEngineWasm::controllerBindingCount)
      .function("setControllerVelocityMeaningful",
                &RealtimeEngineWasm::setControllerVelocityMeaningful)
      .function("controllerVelocityMeaningful", &RealtimeEngineWasm::controllerVelocityMeaningful)
      // destinationId, channel, articulation -- the C ABI's own argument order
      // minus the engine handle, checked against sonare_engine_set_articulation
      // and the Python facade rather than assumed, since embind argument order
      // on this surface has historically diverged from the siblings.
      .function("setArticulation", &RealtimeEngineWasm::setArticulation)
      .function("articulation", &RealtimeEngineWasm::articulation)
      .function("legatoFallbackCount", &RealtimeEngineWasm::legatoFallbackCount)
      .function("setMidiFx", &RealtimeEngineWasm::setMidiFx)
      .function("clearMidiFx", &RealtimeEngineWasm::clearMidiFx)
      .function("setMidiInputSource", &RealtimeEngineWasm::setMidiInputSource)
      .function("clearMidiInputSource", &RealtimeEngineWasm::clearMidiInputSource)
      .function("midiInputPendingCount", &RealtimeEngineWasm::midiInputPendingCount)
      .function("pushMidiInputNoteOn", &RealtimeEngineWasm::pushMidiInputNoteOn)
      .function("pushMidiInputNoteOff", &RealtimeEngineWasm::pushMidiInputNoteOff)
      .function("pushMidiInputCc", &RealtimeEngineWasm::pushMidiInputCc)
      .function("pushMidiNoteOn", &RealtimeEngineWasm::pushMidiNoteOn)
      .function("pushMidiNoteOff", &RealtimeEngineWasm::pushMidiNoteOff)
      .function("pushMidiCc", &RealtimeEngineWasm::pushMidiCc)
      .function("pushMidiUmp", &RealtimeEngineWasm::pushMidiUmp)
      .function("pushMidiSysex", &RealtimeEngineWasm::pushMidiSysex)
      .function("pushMidiPanic", &RealtimeEngineWasm::pushMidiPanic)
      .function("setMidiDestinationExternal", &RealtimeEngineWasm::setMidiDestinationExternal)
      .function("setExternalMidiClockEnabled", &RealtimeEngineWasm::setExternalMidiClockEnabled)
      .function("drainExternalMidi", &RealtimeEngineWasm::drainExternalMidi)
      .function("popExternalMidiToScratch", &RealtimeEngineWasm::popExternalMidiToScratch)
      .function("externalMidiScratchDestinationId",
                &RealtimeEngineWasm::externalMidiScratchDestinationId)
      .function("externalMidiScratchRenderFrame",
                &RealtimeEngineWasm::externalMidiScratchRenderFrame)
      .function("externalMidiScratchByteWord", &RealtimeEngineWasm::externalMidiScratchByteWord)
      .function("externalMidiScratchByteCount", &RealtimeEngineWasm::externalMidiScratchByteCount)
      .function("consumeExternalMidiScratch", &RealtimeEngineWasm::consumeExternalMidiScratch)
      .function("externalMidiDroppedCount", &RealtimeEngineWasm::externalMidiDroppedCount)
      .function("externalMidiPendingCount", &RealtimeEngineWasm::externalMidiPendingCount);
}

#endif  // __EMSCRIPTEN__
