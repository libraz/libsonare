/// @file realtime_engine_midi_control.cpp
/// @brief Embind realtime-engine facade: MIDI CC bindings, controller profiles & part rigs.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <cstdint>
#include <string>

#include "c_api/part_rig_json.h"
#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/articulation_mode.h"
#include "midi/controller_profile.h"
#include "midi/part_rig.h"
#include "realtime_engine_wasm.h"
#include "wasm/bindings/common/synth_patch_val.h"

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)

// The destination's bound instrument. WASM links neither the controller nor the
// engine-MIDI C-ABI unit, so their shared first refusal is reproduced here:
// a RangeError for a destination nothing is bound to, which every entry below
// has to keep apart from the instrument's own "no such capability".
sonare::midi::MidiInstrument* wasmBoundInstrument(const sonare::engine::RealtimeEngine& engine,
                                                  uint32_t destination_id) {
  sonare::midi::MidiInstrument* instrument = engine.midi_instrument(destination_id);
  if (instrument == nullptr) {
    throw WasmRangeError("no MIDI instrument is bound to this destination");
  }
  return instrument;
}

// Maps an engine instrument-control outcome onto the exceptions the C ABI's statuses become:
// a RangeError for a destination nothing is bound to, NotImplemented (the C ABI's
// NOT_SUPPORTED) for a bound instrument without the control.
void wasmRequireControlOk(sonare::engine::RealtimeEngine::InstrumentControlStatus status,
                          const char* unsupported_message) {
  using Status = sonare::engine::RealtimeEngine::InstrumentControlStatus;
  switch (status) {
    case Status::kOk:
      return;
    case Status::kNoInstrument:
      throw WasmRangeError("no MIDI instrument is bound to this destination");
    case Status::kNotSupported:
      break;
  }
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented, unsupported_message);
}

// The destination's current profile, read through the engine rather than from the instrument.
sonare::midi::ControllerProfile wasmControllerProfile(const sonare::engine::RealtimeEngine& engine,
                                                      uint32_t destination_id) {
  sonare::midi::ControllerProfile profile;
  wasmRequireControlOk(engine.instrument_controller_profile(destination_id, &profile),
                       "the bound instrument holds no controller profile");
  return profile;
}

// Installs a profile back onto the destination, adopted by the instrument on the audio thread.
// Every entry reads, changes one thing and installs, so the rule that installing drops the
// channels' accumulated axis values holds however the profile was reached.
void wasmInstallControllerProfile(sonare::engine::RealtimeEngine& engine, uint32_t destination_id,
                                  const sonare::midi::ControllerProfile& profile) {
  wasmRequireControlOk(engine.set_instrument_controller_profile(destination_id, profile),
                       "the bound instrument declined a controller profile");
}

// The tracking field a dimension ordinal names. A pointer rather than a copy so
// the setter and the getter share one mapping: three fields spelled out twice is
// where the two would drift apart. The ordinal has already been checked by the
// shared enum reader.
sonare::midi::NoteTracking* wasmNoteTrackingField(sonare::midi::ControllerProfile* profile,
                                                  int dimension) {
  switch (dimension) {
    case SONARE_MPE_DIMENSION_PRESSURE:
      return &profile->pressure_tracking;
    case SONARE_MPE_DIMENSION_TIMBRE:
      return &profile->timbre_tracking;
    default:
      return &profile->bend_tracking;
  }
}

// Mirrors the C ABI's mapping for a refused bind_midi_cc() whose arguments were
// already validated: a full table is InvalidState, otherwise the snapshot failed
// to allocate.
[[noreturn]] void throwMidiCcBindFailure(const sonare::engine::RealtimeEngine& engine,
                                         const char* message) {
  if (engine.midi_cc_binding_count() >= sonare::midi::CcMap::kMaxBindings) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  std::string(message) + ": the MIDI CC binding table is full");
  }
  throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, message);
}

#endif  // SONARE_WITH_ARRANGEMENT

}  // namespace

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
    throw WasmRangeError(
        "bindMidiCc: channel in [0,15], controller in [0,127], paramId "
        "non-zero, maxValue >= minValue");
  }
  if (!engine_.bind_midi_cc(static_cast<uint8_t>(controller), static_cast<uint8_t>(channel),
                            param_id, min_value, max_value)) {
    throwMidiCcBindFailure(engine_, "failed to bind MIDI CC");
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
    throw WasmRangeError("invalid full MIDI CC binding");
  }
  if (!engine_.bind_midi_cc(binding)) {
    throwMidiCcBindFailure(engine_, "failed to bind full MIDI CC descriptor");
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
    throw WasmRangeError("unknown controller profile preset: '" + preset_name + "'");
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
  const int index = intProperty(binding, "index", 0);
  requireOrdinalInRange(index, 0, 127, "controller binding index");
  entry.index = static_cast<uint8_t>(index);
  const float lo = floatProperty(binding, "lo", 0.0f);
  const float hi = floatProperty(binding, "hi", 1.0f);
  const float curve = floatProperty(binding, "curve", 1.0f);

  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  // A non-finite range or curve would reach the audio thread and stay there, so
  // it is refused here rather than substituted: a mapping silently replaced by a
  // default is a mapping the caller believes it installed.
  if (!std::isfinite(lo) || !std::isfinite(hi)) {
    throw WasmRangeError("binding range must be finite");
  }
  if (!std::isfinite(curve) || curve <= 0.0f) {
    throw WasmRangeError("binding curve must be finite and positive");
  }
  entry.lo = lo;
  entry.hi = hi;
  entry.curve = curve;

  // The binding itself is checked before the table, so a full table never hides a bad binding.
  if (entry.axis == sonare::midi::ControllerAxis::kNone) {
    throw WasmRangeError("controller binding: the axis is none");
  }
  if (entry.input == sonare::midi::ControllerInput::kPolyPressure &&
      !sonare::midi::controller_axis_is_excitation(entry.axis)) {
    throw WasmRangeError("controller binding: a poly-pressure binding named a channel-level axis");
  }
  if (!profile.bind(entry)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "controller binding table is full");
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

// Says which note a value addressed to the whole channel belongs to when several
// are sounding on it, for one per-note dimension. Set per dimension because the
// useful answers differ: pressure following the newest note while bend reaches
// every one is a real configuration, not a mistake. Read only inside an MPE
// zone, and only while more than one note is sounding on the channel, which an
// MPE sender avoids by giving each note its own member channel.
void RealtimeEngineWasm::setControllerNoteTracking(const val& destination_id_val, val dimension,
                                                   val tracking) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  // Both refused rather than clamped by the shared enum reader: a misspelling
  // resolved to a default would configure a dimension the caller never named
  // with a rule they never asked for, and neither shows until two notes share a
  // member channel.
  const int which = sonare_wasm_synth::enumFromVal(dimension, sonare_wasm_synth::kMpeDimensions,
                                                   SONARE_MPE_DIMENSION_COUNT, "dimension");
  const int rule = sonare_wasm_synth::enumFromVal(tracking, sonare_wasm_synth::kNoteTrackings,
                                                  SONARE_NOTE_TRACKING_COUNT, "tracking");
  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  *wasmNoteTrackingField(&profile, which) = static_cast<sonare::midi::NoteTracking>(rule);
  wasmInstallControllerProfile(engine_, destination_id, profile);
#else
  (void)destination_id_val;
  (void)dimension;
  (void)tracking;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

// Reads back setControllerNoteTracking, as the canonical name. Spelled the way
// every other enum leaves this surface (synthPatchToVal), so a value handed back
// can be passed straight to the setter.
val RealtimeEngineWasm::controllerNoteTracking(const val& destination_id_val, val dimension) const {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int which = sonare_wasm_synth::enumFromVal(dimension, sonare_wasm_synth::kMpeDimensions,
                                                   SONARE_MPE_DIMENSION_COUNT, "dimension");
  sonare::midi::ControllerProfile profile = wasmControllerProfile(engine_, destination_id);
  return sonare_wasm_synth::enumNameVal(static_cast<int>(*wasmNoteTrackingField(&profile, which)),
                                        sonare_wasm_synth::kNoteTrackings,
                                        SONARE_NOTE_TRACKING_COUNT);
#else
  (void)destination_id_val;
  (void)dimension;
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
  wasmRequireControlOk(
      engine_.set_instrument_articulation(destination_id, static_cast<uint8_t>(channel),
                                          static_cast<sonare::midi::ArticulationMode>(mode)),
      "the bound instrument has no articulation of its own");
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
  wasmRequireControlOk(
      engine_.instrument_articulation(destination_id, static_cast<uint8_t>(channel), &mode),
      "the bound instrument has no articulation of its own");
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

// Rebuilds one part's (or, for kPartRigAllParts, the destination default's) rig
// on the destination's instrument. Mirrors sonare_engine_set_part_rig: shape
// first, then the chain's processors, then the instrument, whose refusal is
// NotImplemented (NOT_SUPPORTED) because a rig the instrument cannot hold would
// otherwise read as one that took.
void RealtimeEngineWasm::setPartRig(const val& destination_id_val, const val& part_val, val mode,
                                    val inserts_json) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static_assert(static_cast<int>(sonare::midi::PartRigMode::kChain) == 2,
                "WASM PartRigMode table drifted from C");
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int part = checkedIntFromVal(part_val, "part");
  if (part != static_cast<int>(sonare::midi::kPartRigAllParts)) {
    requireOrdinalInRange(part, 0, 15, "part");
  }
  sonare::midi::PartRig rig;
  rig.mode = static_cast<sonare::midi::PartRigMode>(
      sonare_wasm_synth::enumFromVal(mode, sonare_wasm_synth::kPartRigModes,
                                     sonare_wasm_synth::kPartRigModeCount, "part rig mode"));
  const bool has_inserts = !inserts_json.isUndefined() && !inserts_json.isNull();
  if ((rig.mode == sonare::midi::PartRigMode::kChain) != has_inserts) {
    throw WasmRangeError("insertsJson is required for the chain mode and only then");
  }
  if (has_inserts) {
    const std::string json = inserts_json.as<std::string>();
    const SonareError parsed =
        sonare_c_detail::parse_part_rig_inserts(wasmCString(json, "insertsJson"), &rig.stages);
    if (parsed == SONARE_ERROR_INVALID_FORMAT) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "invalid part rig inserts");
    }
  }
  if (!sonare::midi::validate_part_rig(static_cast<uint8_t>(part), rig)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "invalid part rig");
  }
#if defined(SONARE_WITH_MASTERING)
  if (!sonare_c_detail::validate_part_rig_chain(rig.stages)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "invalid part rig insert chain");
  }
#else
  if (rig.mode == sonare::midi::PartRigMode::kChain) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "insert chains are not available in this build");
  }
#endif
  if (!wasmBoundInstrument(engine_, destination_id)
           ->set_part_rig(static_cast<uint8_t>(part), rig)) {
    throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                  "the bound instrument has no part rigs");
  }
#else
  (void)destination_id_val;
  (void)part_val;
  (void)mode;
  (void)inserts_json;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "arrangement/MIDI engine is not available in this build");
#endif
}

void registerRealtimeEngineMidiControl(class_<RealtimeEngineWasm>& cls) {
  cls.function("bindMidiCc", &RealtimeEngineWasm::bindMidiCc)
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
      // destinationId, dimension, tracking -- the C ABI's own argument order
      // minus the engine handle, checked against
      // sonare_engine_set_controller_note_tracking and the Python facade rather
      // than assumed, since embind argument order on this surface has
      // historically diverged from the siblings.
      .function("setControllerNoteTracking", &RealtimeEngineWasm::setControllerNoteTracking)
      .function("controllerNoteTracking", &RealtimeEngineWasm::controllerNoteTracking)
      // destinationId, channel, articulation -- the C ABI's own argument order
      // minus the engine handle, checked against sonare_engine_set_articulation
      // and the Python facade rather than assumed, since embind argument order
      // on this surface has historically diverged from the siblings.
      .function("setArticulation", &RealtimeEngineWasm::setArticulation)
      .function("articulation", &RealtimeEngineWasm::articulation)
      .function("legatoFallbackCount", &RealtimeEngineWasm::legatoFallbackCount)
      // destinationId, part, mode, insertsJson -- the C ABI's own argument order
      // minus the engine handle, checked against sonare_engine_set_part_rig.
      .function("setPartRig", &RealtimeEngineWasm::setPartRig);
}

#endif  // __EMSCRIPTEN__
