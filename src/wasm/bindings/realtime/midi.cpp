/// @file realtime_engine_midi.cpp
/// @brief Embind realtime-engine facade: MIDI instruments, control & events.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <string>
#include <type_traits>

#include "c_api/midi_fx_json.h"
#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/midi_fx.h"
#include "realtime_engine_wasm.h"
#include "wasm/bindings/common/synth_patch_val.h"

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

}  // namespace

void RealtimeEngineWasm::setBuiltinInstrument(uint32_t destination_id, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::midi::BuiltinSynthConfig cfg;
  if (!config.isUndefined() && !config.isNull()) {
    if (hasProperty(config, "waveform")) {
      val wf = config["waveform"];
      if (wf.typeOf().as<std::string>() == "string") {
        const std::string name = wf.as<std::string>();
        const int mapped = sonare_synth_builtin_waveform_from_name(name.c_str());
        if (mapped < 0) {
          throw sonare::SonareException(
              sonare::ErrorCode::InvalidParameter,
              "Unknown synth waveform name: '" + name +
                  "' (expected sine, saw, sawtooth, square, or triangle)");
        }
        cfg.waveform = static_cast<sonare::midi::SynthWaveform>(mapped);
      } else {
        cfg.waveform = static_cast<sonare::midi::SynthWaveform>(wf.as<int>());
      }
    }
    cfg.gain = floatProperty(config, "gain", 0.0f);
    cfg.attack_ms = floatProperty(config, "attackMs", 0.0f);
    cfg.decay_ms = floatProperty(config, "decayMs", 0.0f);
    cfg.sustain = floatProperty(config, "sustain", 0.0f);
    cfg.release_ms = floatProperty(config, "releaseMs", 0.0f);
    cfg.polyphony = intProperty(config, "polyphony", 0);
  }
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(sonare::midi::clamp_synth_config(cfg));
  bindInstrument(destination_id, std::move(synth));
#else
  (void)destination_id;
  (void)config;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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
    const uint32_t event_count = events_val["length"].as<uint32_t>();
    clip.events.reserve(event_count);
    for (uint32_t j = 0; j < event_count; ++j) {
      val event_val = events_val[j];
      sonare::midi::MidiEvent event;
      event.render_frame = int64Property(event_val, "renderFrame", 0);
      sonare::midi::Ump ump;
      ump.words[0] = uintProperty(event_val, "word0", uintProperty(event_val, "data0", 0));
      ump.words[1] = uintProperty(event_val, "word1", uintProperty(event_val, "data1", 0));
      ump.words[2] = uintProperty(event_val, "word2", 0);
      ump.words[3] = uintProperty(event_val, "word3", 0);
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
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Binds the patch-driven NativeSynth (the full synthesizer) on a realtime
// MIDI destination. patch is a SynthPatch object or a preset-name string
// ("saw-lead" / "va:saw-lead"), resolving exactly like
// Project.bounceWithSynthInstrument. Unknown preset names throw.
void RealtimeEngineWasm::setSynthInstrument(uint32_t destination_id, val patch) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const SonareSynthPatch c_patch = sonare_wasm_synth::synthPatchFromVal(patch);
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(c_patch, &cfg, &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  error != nullptr ? error : "invalid synth patch");
  }
  bindInstrument(destination_id, std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
#else
  (void)destination_id;
  (void)patch;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Resolves a hosted instrument's continuous parameter (JSON-key name, e.g.
// "cutoffHz") to the reserved instrument-automation id passed to
// setAutomationLane / setParameter. Returns -1 when the destination has no
// bound instrument, the instrument exposes no automatable parameters, or the
// key is unknown. Like the insert resolvers, the id is returned as a double so
// the full 32-bit unsigned reserved id survives the JS boundary.
double RealtimeEngineWasm::resolveInstrumentAutomationId(uint32_t destination_id,
                                                         const std::string& param_name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return static_cast<double>(engine_.resolve_instrument_automation_id(destination_id, param_name));
#else
  (void)destination_id;
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
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
// destination, fed by the engine's loaded SoundFont. Without a loaded
// SoundFont the player's NativeSynth GM fallback is the data-free floor
// (live MIDI stays audible). config is { gain?, polyphony? }
// ("0 / omit => default").
void RealtimeEngineWasm::setSf2Instrument(uint32_t destination_id, val config) {
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (!config.isUndefined() && !config.isNull()) {
    const float gain = floatProperty(config, "gain", 0.0f);
    if (gain > 0.0f) cfg.gain = gain;
    const int polyphony = intProperty(config, "polyphony", 0);
    if (polyphony > 0) cfg.polyphony = polyphony;
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
  (void)destination_id;
  (void)config;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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

void RealtimeEngineWasm::clearMidiInstrument(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_instrument(destination_id, nullptr);
  builtin_instruments_.erase(
      std::remove_if(builtin_instruments_.begin(), builtin_instruments_.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      builtin_instruments_.end());
#else
  (void)destination_id;
#endif
}

size_t RealtimeEngineWasm::midiInstrumentCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.midi_instrument_count();
#else
  return 0;
#endif
}

void RealtimeEngineWasm::bindMidiCc(int channel, int controller, uint32_t param_id, float min_value,
                                    float max_value) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (channel < 0 || channel > 15 || controller < 0 || controller > 127 || param_id == 0 ||
      !std::isfinite(min_value) || !std::isfinite(max_value) || max_value < min_value) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "bindMidiCc: channel in [0,15], controller in [0,127], paramId non-zero, range finite");
  }
  if (!engine_.bind_midi_cc(static_cast<uint8_t>(controller), static_cast<uint8_t>(channel),
                            param_id, min_value, max_value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to bind MIDI CC");
  }
#else
  (void)channel;
  (void)controller;
  (void)param_id;
  (void)min_value;
  (void)max_value;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::bindMidiCcBinding(val object) {
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::midi::CcBinding binding{};
  binding.cc_number = object["ccNumber"].as<uint8_t>();
  const val channel = object["channel"];
  binding.channel = channel.isUndefined() || channel.isNull() ? sonare::midi::kCcAnyChannel
                                                              : channel.as<uint8_t>();
  // Only ccNumber and paramId are required; every other field falls back to the
  // CcBinding default (any channel, 7-bit Control Change, unit output range),
  // matching the Node addon reader and the Project-side descriptor reader. A
  // field that IS supplied is still range-checked below, so the leniency covers
  // omission only and a non-finite range is still rejected.
  binding.kind = static_cast<sonare::midi::CcBindingKind>(
      static_cast<uint8_t>(uintProperty(object, "kind", 0u)));
  const val cc_lsb = object["ccLsbNumber"];
  const val selector_msb = object["selectorMsb"];
  const val selector_lsb = object["selectorLsb"];
  binding.cc_lsb_number = cc_lsb.isUndefined() ? 0u : cc_lsb.as<uint8_t>();
  binding.selector_msb = selector_msb.isUndefined() ? 0u : selector_msb.as<uint8_t>();
  binding.selector_lsb = selector_lsb.isUndefined() ? 0u : selector_lsb.as<uint8_t>();
  binding.param_id = object["paramId"].as<uint32_t>();
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
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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

void RealtimeEngineWasm::setMidiFx(uint32_t destination_id, const std::string& config_json) {
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::midi::MidiFxChain chain;
  wasmMidiFxChainFromJson(config_json, &chain);
  if (!engine_.set_midi_fx(destination_id, chain)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to install MIDI-FX insert");
  }
#else
  (void)destination_id;
  (void)config_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::clearMidiFx(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.clear_midi_fx(destination_id);
#else
  (void)destination_id;
#endif
}

void RealtimeEngineWasm::setMidiInputSource(uint32_t destination_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_input_source(&midi_input_source_, destination_id);
  midi_input_source_enabled_ = true;
#else
  (void)destination_id;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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
void RealtimeEngineWasm::setMidiDestinationExternal(uint32_t destination_id, bool external) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!engine_.set_midi_destination_external(destination_id, external)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "external MIDI destination table is full");
  }
#else
  (void)destination_id;
  (void)external;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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
val RealtimeEngineWasm::drainExternalMidi(int max_records) {
  val out = val::array();
#if defined(SONARE_WITH_ARRANGEMENT)
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
  (void)max_records;
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

void RealtimeEngineWasm::pushMidiInputNoteOn(int group, int channel, int note, int velocity,
                                             int64_t port_time_samples) {
  pushMidiInputEvent(group, channel, note, velocity, port_time_samples, true);
}

void RealtimeEngineWasm::pushMidiInputNoteOff(int group, int channel, int note, int velocity,
                                              int64_t port_time_samples) {
  pushMidiInputEvent(group, channel, note, velocity, port_time_samples, false);
}

void RealtimeEngineWasm::pushMidiInputCc(int group, int channel, int controller, int value,
                                         int64_t port_time_samples) {
#if defined(SONARE_WITH_ARRANGEMENT)
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
  (void)group;
  (void)channel;
  (void)controller;
  (void)value;
  (void)port_time_samples;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void RealtimeEngineWasm::pushMidiNoteOn(uint32_t destination_id, int group, int channel, int note,
                                        int velocity, int64_t render_frame) {
  pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
               sonare::rt::CommandType::kMidiNoteOnImmediate);
}

void RealtimeEngineWasm::pushMidiNoteOff(uint32_t destination_id, int group, int channel, int note,
                                         int velocity, int64_t render_frame) {
  pushMidiNote(destination_id, group, channel, note, velocity, render_frame,
               sonare::rt::CommandType::kMidiNoteOffImmediate);
}

// Queues an immediate (live) MIDI control change to a MIDI destination. Mirrors
// the C ABI sonare_engine_push_midi_cc: the synthesized MIDI 1.0 CC reaches the
// registered host instrument at @p render_frame (-1 = immediate). Values are
// 7-bit; channel 0..15, group 0..15. The scalar fields are packed into arg.i
// using the encoding documented in rt/command.h (kMidiCcImmediate).
void RealtimeEngineWasm::pushMidiCc(uint32_t destination_id, int group, int channel, int controller,
                                    int value, int64_t render_frame) {
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
void RealtimeEngineWasm::pushMidiUmp(uint32_t destination_id, uint32_t word0,
                                     int64_t render_frame) {
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
// the full message including the leading 0xF0 and trailing 0xF7 (1..512 bytes);
// its bytes are copied out of the Uint8Array before the call returns. Reaches
// the registered host instrument at @p render_frame (-1 = immediate). Mirrors
// the C ABI sonare_engine_push_midi_sysex.
void RealtimeEngineWasm::pushMidiSysex(uint32_t destination_id, val data, int64_t render_frame) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  // Distinguish the two rejection classes the C ABI reports (it bypasses the
  // C-ABI translation unit here, so the mapping is reproduced): malformed or
  // oversized requests are InvalidParameter, while a full command queue is
  // transient OutOfMemory back-pressure.
  if (bytes.empty() || bytes.size() > 512) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "pushMidiSysex: data must contain 1..512 bytes");
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
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
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
