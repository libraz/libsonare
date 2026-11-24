/// @file project_midi.cpp
/// @brief Embind project facade: MIDI content (events, SMF/clip-file IO,
/// program/bank, MIDI FX, note validation) plus the standalone MIDI helper /
/// GM-table free functions.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "analysis/music_analyzer.h"
#include "c_api/midi_input_domains.h"
#include "c_api/ppq_domains.h"
#include "c_api/project_input_domains.h"
#include "c_api/synth_patch_fields.h"
#include "project_wasm.h"
#include "wasm/bindings/common/midi_cc_kind.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

// A non-NULL pointer to @p bytes, also when it is empty.
const uint8_t* nonNullBytes(const std::vector<uint8_t>& bytes) {
  static constexpr uint8_t kNoBytes = 0;
  return bytes.empty() ? &kNoBytes : bytes.data();
}

struct MidiEventListGuard {
  SonareMidiEventList value{};

  ~MidiEventListGuard() { sonare_free_midi_event_list(&value); }
};

struct ProjectInstrumentListGuard {
  SonareProjectInstrumentList value{};

  ~ProjectInstrumentListGuard() {
    if (value.items != nullptr) sonare_free_project_instrument_list(&value);
  }
};

std::string projectInstrumentEnumName(const char* const* names, size_t count, int ordinal,
                                      const char* noun) {
  if (ordinal < 0 || static_cast<size_t>(ordinal) >= count) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidFormat,
        std::string("unknown ") + noun + " ordinal: " + std::to_string(ordinal));
  }
  return names[ordinal];
}

val projectSynthPatchToSparseVal(const SonareSynthPatch& patch) {
  val out = val::object();
  if (patch.preset[0] != '\0') out.set("preset", std::string(patch.preset));
  if (patch.engine_mode != SONARE_SYNTH_ENGINE_DEFAULT) {
    out.set("engineMode", projectInstrumentEnumName(sonare_wasm_synth::kEngineModes,
                                                    SONARE_SYNTH_ENGINE_MODE_COUNT,
                                                    patch.engine_mode, "synth engine mode"));
  }

  for (const auto& field : sonare_c_detail::kSynthPatchEnumFields) {
    const int ordinal = patch.*field.member;
    if (ordinal == 0) continue;
    out.set(field.name,
            projectInstrumentEnumName(field.values.names, field.values.count, ordinal,
                                      (std::string("synth patch ") + field.name).c_str()));
  }

  for (const auto& field : sonare_c_detail::kSynthPatchNumericFields) {
    const double value = std::visit(
        [&](const auto member) { return static_cast<double>(patch.*member); }, field.member);
    const uint32_t present = field.presence_fields2 ? patch.present_fields2 : patch.present_fields;
    if (value == 0.0 && (field.presence_bit == 0 || (present & field.presence_bit) == 0)) {
      continue;
    }
    if (std::string(field.name) == "oneShot") {
      out.set(field.name, value != 0.0);
    } else {
      out.set(field.name, value);
    }
  }

  if (patch.engine_param_count != 0) {
    if (patch.engine_params == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "destination instrument has missing synth engine params");
    }
    val engine_params = val::object();
    for (size_t i = 0; i < patch.engine_param_count; ++i) {
      const SonareSynthEngineParam& param = patch.engine_params[i];
      if (param.key == nullptr || param.key[0] == '\0') {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "destination instrument has an invalid synth engine key");
      }
      engine_params.set(std::string(param.key), param.value);
    }
    out.set("engineParams", engine_params);
  }

  if (patch.num_mod_routings < 0 || patch.num_mod_routings > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "destination instrument has an invalid mod routing count");
  }
  if (patch.num_mod_routings != 0 ||
      (patch.present_fields & SONARE_SYNTH_FIELD_MOD_ROUTINGS) != 0) {
    val routings = val::array();
    for (int i = 0; i < patch.num_mod_routings; ++i) {
      const SonareSynthModRouting& route = patch.mod_routings[i];
      val row = val::object();
      row.set("source", projectInstrumentEnumName(sonare_wasm_synth::kModSources,
                                                  SONARE_SYNTH_MOD_SOURCE_COUNT, route.source,
                                                  "synth modulation source"));
      row.set("destination",
              projectInstrumentEnumName(sonare_wasm_synth::kModDestinations,
                                        SONARE_SYNTH_MOD_DESTINATION_COUNT, route.destination,
                                        "synth modulation destination"));
      row.set("depth", route.depth);
      routings.set(static_cast<unsigned>(i), row);
    }
    out.set("modRoutings", routings);
  }
  return out;
}

val projectInstrumentToVal(const SonareProjectInstrument& source) {
  if (source.struct_size != sizeof(SonareProjectInstrument)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "destination instrument has an invalid struct size");
  }
  val instrument = val::object();
  switch (source.kind) {
    case SONARE_PROJECT_INSTRUMENT_SYNTH: {
      if (source.synth == nullptr || source.builtin != nullptr || source.sf2 != nullptr ||
          source.sound_font_uri != nullptr) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "destination synth instrument has invalid config pointers");
      }
      instrument.set("kind", "synth");
      instrument.set("patch", projectSynthPatchToSparseVal(*source.synth));
      instrument.set("useGmPrograms", source.use_gm_programs != 0);
      break;
    }
    case SONARE_PROJECT_INSTRUMENT_BUILTIN: {
      if (source.builtin == nullptr || source.synth != nullptr || source.sf2 != nullptr ||
          source.sound_font_uri != nullptr ||
          source.builtin->struct_size != sizeof(SonareBuiltinSynthConfig)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "destination built-in instrument has invalid config data");
      }
      const SonareBuiltinSynthConfig& config = *source.builtin;
      val fields = val::object();
      if (config.waveform < 0 || config.waveform >= SONARE_SYNTH_WAVEFORM_COUNT) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidFormat,
            "unknown built-in synth waveform ordinal: " + std::to_string(config.waveform));
      }
      constexpr size_t kWaveformNameIndices[] = {0, 1, 3, 4};
      fields.set(
          "waveform",
          sonare_wasm_synth::synth_names::kBuiltinWaveforms[kWaveformNameIndices[config.waveform]]);
      fields.set("gain", config.gain);
      fields.set("attackMs", config.attack_ms);
      fields.set("decayMs", config.decay_ms);
      fields.set("sustain", config.sustain);
      fields.set("releaseMs", config.release_ms);
      fields.set("polyphony", config.polyphony);
      instrument.set("kind", "builtin");
      instrument.set("config", fields);
      break;
    }
    case SONARE_PROJECT_INSTRUMENT_SF2: {
      if (source.sf2 == nullptr || source.synth != nullptr || source.builtin != nullptr ||
          source.sf2->struct_size != sizeof(SonareSf2InstrumentConfig)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "destination SoundFont instrument has invalid config data");
      }
      const SonareSf2InstrumentConfig& config = *source.sf2;
      if (config.gs_efx_realization < 0 || config.gs_efx_realization > 1) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                      "unknown SoundFont EFX realization ordinal: " +
                                          std::to_string(config.gs_efx_realization));
      }
      val fields = val::object();
      fields.set("gain", config.gain);
      fields.set("polyphony", config.polyphony);
      fields.set("preferModelForModeledFamilies", config.prefer_model_for_modeled_families != 0);
      fields.set("clearBankRig", config.clear_bank_rig != 0);
      fields.set("gsEfxRealization",
                 config.gs_efx_realization == 0 ? std::string("modern") : std::string("classic"));
      instrument.set("kind", "sf2");
      instrument.set("config", fields);
      if (source.sound_font_uri != nullptr) {
        instrument.set("soundFontUri", std::string(source.sound_font_uri));
      }
      break;
    }
    default:
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidFormat,
          "unknown project instrument kind ordinal: " + std::to_string(source.kind));
  }
  return instrument;
}

void destinationInstrumentMidiOptions(val options, SonareProjectInstrument* instrument) {
  if (options.isUndefined() || options.isNull()) return;
  if (options.typeOf().as<std::string>() != "object" ||
      val::global("Array").call<bool>("isArray", options)) {
    throw WasmTypeError("options must be an object");
  }
  if (hasProperty(options, "bank")) {
    val bank = options["bank"];
    if (bank.typeOf().as<std::string>() != "object" || bank.isNull() ||
        val::global("Array").call<bool>("isArray", bank)) {
      throw WasmTypeError("options.bank must be an object");
    }
    instrument->bank_msb = domainNumberFromVal<uint8_t>(bank["msb"], "options.bank.msb",
                                                        sonare_c_detail::kMidiDataByteDomain);
    instrument->bank_lsb = domainNumberFromVal<uint8_t>(bank["lsb"], "options.bank.lsb",
                                                        sonare_c_detail::kMidiDataByteDomain);
  }
  if (hasProperty(options, "program")) {
    instrument->program = domainNumberFromVal<uint8_t>(options["program"], "options.program",
                                                       sonare_c_detail::kMidiDataByteDomain);
  }
}

val destinationInstrumentToVal(const SonareProjectInstrumentBinding& source) {
  val binding = val::object();
  binding.set("destinationId", static_cast<double>(source.destination_id));
  binding.set("instrument", projectInstrumentToVal(source.instrument));
  if ((source.instrument.bank_msb == SONARE_PROJECT_INSTRUMENT_FROM_CLIP) !=
      (source.instrument.bank_lsb == SONARE_PROJECT_INSTRUMENT_FROM_CLIP)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "destination instrument has an incomplete bank override");
  }
  if (source.instrument.bank_msb != SONARE_PROJECT_INSTRUMENT_FROM_CLIP) {
    if (source.instrument.bank_msb < 0 || source.instrument.bank_msb > 127 ||
        source.instrument.bank_lsb < 0 || source.instrument.bank_lsb > 127) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "destination instrument has an invalid bank override");
    }
    val bank = val::object();
    bank.set("msb", source.instrument.bank_msb);
    bank.set("lsb", source.instrument.bank_lsb);
    binding.set("bank", bank);
  }
  if (source.instrument.program != SONARE_PROJECT_INSTRUMENT_FROM_CLIP) {
    if (source.instrument.program < 0 || source.instrument.program > 127) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "destination instrument has an invalid program override");
    }
    binding.set("program", source.instrument.program);
  }
  return binding;
}

}  // namespace

void ProjectWasm::setMidiEvents(const val& clip_id_val, val events) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const size_t count = wasmArrayLength(events, "events");
  std::vector<SonareMidiEventPod> pods(count);
  std::vector<std::vector<uint8_t>> sysex_storage(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    if (val::global("Array").call<bool>("isArray", entry)) {
      pods[i].ppq = numberFromVal(entry[0], "ppq");
      pods[i].word0 = checkedWordFromVal(entry[1], "word0");
      pods[i].word1 = checkedWordFromVal(entry[2], "word1");
    } else {
      pods[i].ppq = numberFromVal(entry["ppq"], "ppq");
      pods[i].word0 = checkedWordFromVal(entry["word0"], "word0");
      pods[i].word1 = wordProperty(entry, "word1", pods[i].word1);
      val sysex = entry["sysex"];
      if (!sysex.isUndefined() && !sysex.isNull()) {
        sysex_storage[i] = uint8ArrayToVector(sysex, "sysex");
        pods[i].sysex = nonNullBytes(sysex_storage[i]);
        pods[i].sysex_size = sysex_storage[i].size();
      }
    }
  }
  const SonareError err = sonare_project_set_midi_events(
      project_.get(), clip_id, pods.empty() ? nullptr : pods.data(), pods.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::midiEvents(const val& clip_id_val) const {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  MidiEventListGuard list;
  const SonareError err = sonare_project_midi_events(project_.get(), clip_id, &list.value);
  if (err != SONARE_OK) throwCodedCError(err);
  if (list.value.count != 0 && list.value.items == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "midiEvents returned a missing event array");
  }

  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareMidiEventPod& source = list.value.items[i];
    const bool has_sysex = source.sysex != nullptr;
    if (has_sysex != (source.sysex_size != 0)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "midiEvents returned an invalid SysEx payload");
    }
    val event = val::object();
    event.set("ppq", source.ppq);
    event.set("word0", static_cast<double>(source.word0));
    event.set("word1", static_cast<double>(source.word1));
    if (has_sysex) {
      event.set("sysex", vectorToUint8Array(source.sysex, source.sysex_size));
    }
    out.set(static_cast<unsigned>(i), event);
  }
  return out;
}

void ProjectWasm::setDestinationInstrument(const val& destination_id_val, val instrument,
                                           val options) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const bool clear = instrument.isUndefined() || instrument.isNull();
  if (clear && !options.isUndefined() && !options.isNull()) {
    throw WasmTypeError("options require an instrument");
  }

  SonareProjectInstrument stored{};
  sonare_wasm_synth::SynthPatchStorage synth_patch;
  SonareBuiltinSynthConfig builtin{};
  SonareSf2InstrumentBinding sf2{};
  std::string sound_font_uri;
  if (!clear) {
    if (instrument.typeOf().as<std::string>() != "object" ||
        val::global("Array").call<bool>("isArray", instrument)) {
      throw WasmTypeError("instrument must be an object");
    }
    val kind_value = instrument["kind"];
    if (kind_value.typeOf().as<std::string>() != "string") {
      throw WasmTypeError("instrument.kind must be a string token");
    }
    const std::string kind = kind_value.as<std::string>();
    sonare_project_instrument_init(&stored);
    if (kind == "synth") {
      val patch = instrument["patch"];
      if (patch.isUndefined() || patch.isNull() || patch.typeOf().as<std::string>() != "object" ||
          val::global("Array").call<bool>("isArray", patch)) {
        throw WasmTypeError("instrument.patch must be an object");
      }
      if (hasProperty(patch, "sampleBank") && !patch["sampleBank"].isUndefined() &&
          !patch["sampleBank"].isNull()) {
        throw sonare::SonareException(
            sonare::ErrorCode::NotSupported,
            "sample-bank patches cannot be stored as destination instruments",
            sonare::ErrorReason::NotPersistable);
      }
      synth_patch = sonare_wasm_synth::synthPatchFromVal(patch);
      stored.kind = SONARE_PROJECT_INSTRUMENT_SYNTH;
      stored.synth = &synth_patch.patch;
      val patch_use_gm = patch["useGmPrograms"];
      val instrument_use_gm = instrument["useGmPrograms"];
      const bool has_patch_use_gm = !patch_use_gm.isUndefined() && !patch_use_gm.isNull();
      const bool has_instrument_use_gm =
          !instrument_use_gm.isUndefined() && !instrument_use_gm.isNull();
      const bool patch_uses_gm =
          has_patch_use_gm ? boolProperty(patch, "useGmPrograms", false) : false;
      const bool instrument_uses_gm = has_instrument_use_gm ? instrument_use_gm.as<bool>() : false;
      if (has_patch_use_gm && has_instrument_use_gm && patch_uses_gm != instrument_uses_gm) {
        throw WasmRangeError(
            "instrument.useGmPrograms conflicts with instrument.patch.useGmPrograms");
      }
      stored.use_gm_programs =
          has_instrument_use_gm ? (instrument_uses_gm ? 1 : 0) : (patch_uses_gm ? 1 : 0);
    } else if (kind == "builtin") {
      val config = instrument["config"];
      if (config.isUndefined() || config.isNull() ||
          config.typeOf().as<std::string>() != "object" ||
          val::global("Array").call<bool>("isArray", config)) {
        throw WasmTypeError("instrument.config must be an object");
      }
      SonareBuiltinInstrumentBinding parsed = builtinBindingFromVal(config);
      builtin = parsed.config;
      stored.kind = SONARE_PROJECT_INSTRUMENT_BUILTIN;
      stored.builtin = &builtin;
    } else if (kind == "sf2") {
      val config = instrument["config"];
      if (config.isUndefined() || config.isNull() ||
          config.typeOf().as<std::string>() != "object" ||
          val::global("Array").call<bool>("isArray", config)) {
        throw WasmTypeError("instrument.config must be an object");
      }
      sf2 = sf2BindingFromVal(config);
      stored.kind = SONARE_PROJECT_INSTRUMENT_SF2;
      stored.sf2 = &sf2.config;
      if (hasProperty(instrument, "soundFontUri")) {
        sound_font_uri = stringFromVal(instrument["soundFontUri"], "soundFontUri");
        stored.sound_font_uri = wasmCString(sound_font_uri, "soundFontUri");
      }
    } else if (kind == "sample" || kind == "sampler" || kind == "callback") {
      throw sonare::SonareException(
          sonare::ErrorCode::NotSupported,
          kind + " instruments cannot be stored as destination instruments",
          sonare::ErrorReason::NotPersistable);
    } else {
      throw WasmRangeError("unknown instrument kind token: " + kind);
    }
    destinationInstrumentMidiOptions(options, &stored);
  }

  const SonareError err = sonare_project_set_destination_instrument(project_.get(), destination_id,
                                                                    clear ? nullptr : &stored);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
}

val ProjectWasm::destinationInstruments() const {
  ProjectInstrumentListGuard list;
  const SonareError err = sonare_project_destination_instruments(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  if (list.value.count != 0 && list.value.items == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "destinationInstruments returned a missing binding array");
  }
  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    out.set(static_cast<unsigned>(i), destinationInstrumentToVal(list.value.items[i]));
  }
  return out;
}

uint32_t ProjectWasm::importSmf(val data) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data, "data");
  uint32_t first_clip = 0;
  // An empty buffer reaches the C entry as one, which refuses it as empty rather than as NULL.
  const SonareError err =
      sonare_project_import_smf(project_.get(), nonNullBytes(bytes), bytes.size(), &first_clip);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return first_clip;
}

val ProjectWasm::exportSmf() {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_export_smf(project_.get(), &bytes, &len);
  if (err != SONARE_OK) {
    sonare_free_bytes(bytes);
    throwCodedCError(err);
  }
  std::vector<uint8_t> out(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return vectorToUint8Array(out);
}

uint32_t ProjectWasm::importClipFile(val data) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data, "data");
  uint32_t first_clip = 0;
  // An empty buffer reaches the C entry as one, which refuses it as empty rather than as NULL.
  const SonareError err = sonare_project_import_clip_file(project_.get(), nonNullBytes(bytes),
                                                          bytes.size(), &first_clip);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return first_clip;
}

val ProjectWasm::exportClipFile() {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_export_clip_file(project_.get(), &bytes, &len);
  if (err != SONARE_OK) {
    sonare_free_bytes(bytes);
    throwCodedCError(err);
  }
  std::vector<uint8_t> out(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return vectorToUint8Array(out);
}

void ProjectWasm::setProgram(const val& clip_id_val, const val& program_val, const val& bank_val,
                             const val& group_val, const val& channel_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  // An absent group or channel is group 0, channel 0.
  const uint8_t group =
      optionalDomainValue<uint8_t>(group_val, "group", sonare_c_detail::kMidiUmpGroupDomain)
          .value_or(0);
  const uint8_t channel =
      optionalDomainValue<uint8_t>(channel_val, "channel", sonare_c_detail::kMidiChannelDomain)
          .value_or(0);
  const int program = checkedIntFromVal(program_val, "program");
  const int bank = checkedIntFromVal(bank_val, "bank");
  const SonareError err =
      sonare_project_set_program(project_.get(), clip_id, group, channel, program, bank);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::bakeMidiFx(const val& clip_id_val, const std::string& config_json,
                            const val& with_source_index_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const bool with_source_index =
      optionalBool(with_source_index_val, "withSourceIndex").value_or(false);
  if (!with_source_index) {
    const SonareError err = sonare_project_bake_midi_fx(
        project_.get(), clip_id, wasmCString(config_json, "configJson"), nullptr, 0, nullptr);
    if (err != SONARE_OK) {
      throwCodedCError(err);
    }
    return val::undefined();
  }
  // Sized from the non-destructive preview so the bake is a single pass with an
  // exactly-fitting buffer, rather than a bake that has to be repeated when the
  // guess is short.
  size_t expected = 0;
  const SonareError count_err = sonare_project_preview_midi_fx_count(
      project_.get(), clip_id, wasmCString(config_json, "configJson"), &expected);
  if (count_err != SONARE_OK) {
    throwCodedCError(count_err);
  }
  static_assert(sizeof(int) == sizeof(int32_t), "vectorToInt32Array assumes a 32-bit int");
  std::vector<int> source_index(expected, -1);
  size_t written = 0;
  const SonareError err =
      sonare_project_bake_midi_fx(project_.get(), clip_id, wasmCString(config_json, "configJson"),
                                  elementData(source_index), source_index.size(), &written);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  source_index.resize(std::min(written, source_index.size()));
  return vectorToInt32Array(source_index);
}

uint32_t ProjectWasm::previewMidiFxCount(const val& clip_id_val, const std::string& config_json) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  size_t count = 0;
  const SonareError err = sonare_project_preview_midi_fx_count(
      project_.get(), clip_id, wasmCString(config_json, "configJson"), &count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<uint32_t>(count);
}

val ProjectWasm::validateMidiNotes(const val& clip_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  SonareNotePairValidation result{};
  const SonareError err = sonare_project_validate_midi_notes(project_.get(), clip_id, &result);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::object();
  out.set("ok", result.ok != 0);
  out.set("unmatchedNoteOns", static_cast<double>(result.unmatched_note_ons));
  out.set("unmatchedNoteOffs", static_cast<double>(result.unmatched_note_offs));
  return out;
}

namespace {

// Seeds the native defaults, then applies whichever the caller supplied.
SonareProjectTempoOptions tempoOptionsFrom(val options) {
  SonareProjectTempoOptions out;
  sonare_project_tempo_options_init(&out);
  if (options.isUndefined() || options.isNull()) return out;
  // Every field here is optional and keeps its seeded default when omitted; a written count is
  // read in its domain.
  out.adaptive_tempo = boolProperty(options, "adaptiveTempo", out.adaptive_tempo != 0) ? 1 : 0;
  out.tempo_update_interval_beats =
      optionalDomainValue<int>(options["tempoUpdateIntervalBeats"], "tempoUpdateIntervalBeats",
                               sonare::kMusicCountDomain)
          .value_or(out.tempo_update_interval_beats);
  out.ramp_threshold = floatProperty(options, "rampThreshold", out.ramp_threshold);
  out.include_octave_candidates =
      boolProperty(options, "includeOctaveCandidates", out.include_octave_candidates != 0) ? 1 : 0;
  return out;
}

}  // namespace

val ProjectWasm::analyzeTempo(val audio, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> samples = float32ArrayToVector(audio, "samples");
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  const SonareProjectTempoOptions resolved = tempoOptionsFrom(options);
  const SonareError err = sonare_project_analyze_tempo(project_.get(), elementData(samples),
                                                       samples.size(), sample_rate, &resolved,
                                                       candidates, std::size(candidates), &count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val output = val::array();
  const char* labels[] = {"primary", "half", "double"};
  for (size_t i = 0; i < count && i < std::size(candidates); ++i) {
    const SonareProjectTempoCandidate& candidate = candidates[i];
    val item = val::object();
    item.set("bpm", candidate.bpm);
    item.set("confidence", candidate.confidence);
    item.set("label", labels[candidate.kind <= SONARE_TEMPO_CANDIDATE_DOUBLE ? candidate.kind : 0]);
    item.set("timeSignatureCount", candidate.time_signature_count);
    val time_signature = val::object();
    time_signature.set("startPpq", candidate.first_time_signature.start_ppq);
    time_signature.set("numerator", candidate.first_time_signature.numerator);
    time_signature.set("denominator", candidate.first_time_signature.denominator);
    item.set("timeSignature", time_signature);
    output.call<void>("push", item);
  }
  return output;
}

val ProjectWasm::autoTempo(val audio, const val& sample_rate_val, const val& candidate_index_val,
                           bool apply_time_signatures, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const size_t candidate_index = checkedSizeTFromVal(candidate_index_val, "candidateIndex");
  std::vector<float> samples = float32ArrayToVector(audio, "samples");
  const SonareProjectTempoOptions resolved = tempoOptionsFrom(options);
  SonareProjectAutoTempoResult result{};
  const SonareError err =
      sonare_project_auto_tempo(project_.get(), elementData(samples), samples.size(), sample_rate,
                                &resolved, candidate_index, apply_time_signatures ? 1 : 0, &result);
  if (err != SONARE_OK) throwCodedCError(err);
  val signature = val::object();
  signature.set("numerator", result.time_signature.numerator);
  signature.set("denominator", result.time_signature.denominator);
  signature.set("confidence", result.time_signature.confidence);
  val out = val::object();
  out.set("bpm", result.bpm);
  out.set("bpmConfidence", result.bpm_confidence);
  out.set("timeSignature", signature);
  return out;
}

double ProjectWasm::snapToGrid(const val& ppq_val, const val& strength_val,
                               const val& division_val) {
  const double ppq = checkedDoubleFromVal(ppq_val, "ppq");
  const double strength = checkedDoubleFromVal(strength_val, "strength");
  const int division =
      domainNumberFromVal<int>(division_val, "division", sonare_c_detail::kSnapDivisionDomain);
  double out = 0.0;
  SonareProjectSnapToGridOptions options;
  sonare_project_snap_to_grid_options_init(&options);
  options.division = division;
  const SonareError err =
      sonare_project_snap_to_grid(project_.get(), ppq, strength, &options, &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return out;
}

uint32_t js_project_abi_version() { return sonare_project_abi_version(); }

val js_nullable_string(const char* value) {
  return value != nullptr ? val(std::string(value)) : val::null();
}

// The MIDI 1.0 <-> 2.0 value scalers: each argument is read at its C parameter's width, refusing a
// value the narrowing cast would wrap, and the C call owns the MIDI range.
namespace {

// Reads a scaler input at the width of @p In, refusing what the narrowing cast would wrap.
template <typename In>
In scaleInput(const val& value, const char* key) {
  const double number = numberFromVal(value, key);
  constexpr double kMax = static_cast<double>(std::numeric_limits<In>::max());
  if (!(number >= 0.0) || number > kMax || std::floor(number) != number) {
    throw WasmRangeError(std::string(key) + " must be an integer in [0, " +
                         std::to_string(static_cast<unsigned long long>(kMax)) + "]");
  }
  return static_cast<In>(number);
}

template <typename In, typename Out>
double midiScale(In input, SonareError (*scale)(In, Out*)) {
  Out out{};
  const SonareError err = scale(input, &out);
  if (err != SONARE_OK) throwCodedCError(err);
  return static_cast<double>(out);
}

}  // namespace

double js_midi_velocity_to_midi2(const val& v) {
  return midiScale(scaleInput<uint8_t>(v, "velocity"), sonare_midi_velocity_to_midi2);
}
double js_midi_note_on_velocity_to_midi2(const val& v) {
  return midiScale(scaleInput<uint8_t>(v, "velocity"), sonare_midi_note_on_velocity_to_midi2);
}
double js_midi2_velocity_to_midi1(const val& v) {
  return midiScale(scaleInput<uint16_t>(v, "velocity16"), sonare_midi2_velocity_to_midi1);
}
double js_midi2_note_on_velocity_to_midi1(const val& v) {
  return midiScale(scaleInput<uint16_t>(v, "velocity16"), sonare_midi2_note_on_velocity_to_midi1);
}
double js_midi_control_to_midi2(const val& v) {
  return midiScale(scaleInput<uint8_t>(v, "value"), sonare_midi_control_to_midi2);
}
double js_midi_control14_to_midi2(const val& v) {
  return midiScale(scaleInput<uint16_t>(v, "value14"), sonare_midi_control14_to_midi2);
}
double js_midi2_control_to_midi1(const val& v) {
  return midiScale(checkedUintFromVal(v, "value32"), sonare_midi2_control_to_midi1);
}
double js_midi2_control_to_midi1_control14(const val& v) {
  return midiScale(checkedUintFromVal(v, "value32"), sonare_midi2_control_to_midi1_control14);
}
double js_midi_pitch_bend_to_midi2(const val& v) {
  return midiScale(scaleInput<uint16_t>(v, "bend14"), sonare_midi_pitch_bend_to_midi2);
}
double js_midi2_pitch_bend_to_midi1(const val& v) {
  return midiScale(checkedUintFromVal(v, "bend32"), sonare_midi2_pitch_bend_to_midi1);
}

val js_midi_gm_instrument_name(const val& program) {
  return js_nullable_string(sonare_midi_gm_instrument_name(checkedIntFromVal(program, "program")));
}

int js_midi_gm_program_for_name(const std::string& name) {
  return sonare_midi_gm_program_for_name(wasmCString(name, "name"));
}

val js_midi_gm_family_name(const val& family) {
  return js_nullable_string(sonare_midi_gm_family_name(checkedIntFromVal(family, "family")));
}

int js_midi_gm_family_first_program(const val& family) {
  return sonare_midi_gm_family_first_program(checkedIntFromVal(family, "family"));
}

val js_midi_gm2_instrument_name(const val& bank_lsb, const val& program) {
  return js_nullable_string(sonare_midi_gm2_instrument_name(checkedIntFromVal(bank_lsb, "bankLsb"),
                                                            checkedIntFromVal(program, "program")));
}

val js_midi_gm_drum_name(const val& note) {
  return js_nullable_string(sonare_midi_gm_drum_name(checkedIntFromVal(note, "note")));
}

int js_midi_gm_drum_note_for_name(const std::string& name) {
  return sonare_midi_gm_drum_note_for_name(wasmCString(name, "name"));
}

val js_midi_gm2_drum_set_name(const val& bank_lsb) {
  return js_nullable_string(sonare_midi_gm2_drum_set_name(checkedIntFromVal(bank_lsb, "bankLsb")));
}

val js_midi_gm2_drum_name(const val& bank_lsb, const val& note) {
  return js_nullable_string(sonare_midi_gm2_drum_name(checkedIntFromVal(bank_lsb, "bankLsb"),
                                                      checkedIntFromVal(note, "note")));
}

val js_midi_cc_name(const val& controller) {
  return js_nullable_string(sonare_midi_cc_name(checkedIntFromVal(controller, "controller")));
}

int js_midi_cc_index_for_name(const std::string& name) {
  return sonare_midi_cc_index_for_name(wasmCString(name, "name"));
}

val js_midi_per_note_controller_name(const val& index) {
  return js_nullable_string(
      sonare_midi_per_note_controller_name(checkedIntFromVal(index, "index")));
}

val js_midi_bank_program(const val& ppq_val, const val& group_val, const val& channel_val,
                         const val& bank_msb_val, const val& bank_lsb_val, const val& program_val) {
  using namespace sonare_c_detail;
  const double ppq = domainNumberFromVal<double>(ppq_val, "ppq", kPublicPpqDomain);
  const auto group = domainNumberFromVal<uint8_t>(group_val, "group", kMidiUmpGroupDomain);
  const auto channel = domainNumberFromVal<uint8_t>(channel_val, "channel", kMidiChannelDomain);
  const int bank_msb = domainNumberFromVal<int>(bank_msb_val, "bank.msb", kMidiDataByteDomain);
  const int bank_lsb = domainNumberFromVal<int>(bank_lsb_val, "bank.lsb", kMidiDataByteDomain);
  const int program = domainNumberFromVal<int>(program_val, "program", kMidiDataByteDomain);
  SonareMidiEventPod events[3]{};
  size_t count = 0;
  const SonareError err =
      sonare_midi_bank_program(ppq, group, channel, bank_msb, bank_lsb, program, events, 3, &count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::array();
  for (size_t i = 0; i < count; ++i) {
    val event = val::object();
    event.set("ppq", events[i].ppq);
    event.set("word0", static_cast<double>(events[i].word0));
    event.set("word1", static_cast<double>(events[i].word1));
    out.set(static_cast<unsigned>(i), event);
  }
  return out;
}

SonareMidiEventPod js_midi_event_from_val(val event) {
  SonareMidiEventPod out{};
  out.ppq = numberFromVal(event["ppq"], "ppq");
  out.word0 = checkedWordFromVal(event["word0"], "word0");
  out.word1 = wordProperty(event, "word1", out.word1);
  return out;
}

val js_midi_event_to_val(const SonareMidiEventPod& event) {
  val out = val::object();
  out.set("ppq", event.ppq);
  out.set("word0", static_cast<double>(event.word0));
  out.set("word1", static_cast<double>(event.word1));
  return out;
}

SonareMidiCcBinding js_cc_binding_from_val(val object, const std::string& at) {
  SonareMidiCcBinding out{};
  // The 14-bit kind's narrower [0, 31] is refused by the C ABI, which sees the kind.
  out.cc_number =
      domainNumberFromVal<uint8_t>(object["ccNumber"], elementField(at, "ccNumber").c_str(),
                                   sonare_c_detail::kMidiDataByteDomain);
  out.channel = static_cast<uint8_t>(
      intInRangeOrAbsentProperty(object, "channel", 0, 15, SONARE_MIDI_CC_ANY_CHANNEL, at));
  out.kind = static_cast<uint8_t>(ccBindingKindProperty(object, at));
  out.cc_lsb_number = byteProperty(object, "ccLsbNumber", 0u, at);
  out.selector_msb = byteProperty(object, "selectorMsb", 0u, at);
  out.selector_lsb = byteProperty(object, "selectorLsb", 0u, at);
  out.param_id = checkedUintFromVal(object["paramId"], elementField(at, "paramId").c_str());
  out.min_value = floatProperty(object, "minValue", 0.0f, at);
  out.max_value = floatProperty(object, "maxValue", 1.0f, at);
  out.consume = boolProperty(object, "consume", false, at) ? 1 : 0;
  return out;
}

val js_cc_binding_to_val(const SonareMidiCcBinding& binding) {
  val out = val::object();
  out.set("consume", binding.consume != 0);
  out.set("ccNumber", static_cast<double>(binding.cc_number));
  if (binding.channel != SONARE_MIDI_CC_ANY_CHANNEL) {
    out.set("channel", static_cast<double>(binding.channel));
  }
  out.set("kind", val(ccBindingKindToken(binding.kind)));
  out.set("ccLsbNumber", static_cast<double>(binding.cc_lsb_number));
  out.set("selectorMsb", static_cast<double>(binding.selector_msb));
  out.set("selectorLsb", static_cast<double>(binding.selector_lsb));
  out.set("paramId", static_cast<double>(binding.param_id));
  out.set("minValue", binding.min_value);
  out.set("maxValue", binding.max_value);
  return out;
}

std::vector<SonareMidiCcBinding> js_cc_bindings_from_val(val bindings) {
  const size_t count = wasmArrayLength(bindings, "bindings");
  std::vector<SonareMidiCcBinding> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = js_cc_binding_from_val(bindings[i], "bindings[" + std::to_string(i) + "]");
  }
  return out;
}

val js_midi_cc_learn(val events, const val& param_id_val, const val& min_value_val,
                     const val& max_value_val, const val& min_movement_val) {
  const uint32_t param_id = domainNumberFromVal<uint32_t>(
      param_id_val, "paramId", sonare_c_detail::kMidiTargetParamIdDomain);
  const float min_value = checkedFloatFromVal(min_value_val, "minValue");
  const float max_value = checkedFloatFromVal(max_value_val, "maxValue");
  const uint8_t min_movement = domainNumberFromVal<uint8_t>(min_movement_val, "minMovement",
                                                            sonare_c_detail::kMidiDataByteDomain);
  const size_t count = wasmArrayLength(events, "events");
  std::vector<SonareMidiEventPod> pods(count);
  for (size_t i = 0; i < count; ++i) {
    pods[i] = js_midi_event_from_val(events[i]);
  }
  SonareMidiCcBinding learned{};
  const SonareError err =
      sonare_midi_cc_learn(pods.empty() ? nullptr : pods.data(), pods.size(), param_id, min_value,
                           max_value, min_movement, &learned);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return js_cc_binding_to_val(learned);
}

val js_midi_cc_to_breakpoint(val bindings, val event) {
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod pod = js_midi_event_from_val(event);
  SonareAutomationPoint point{};
  const SonareError err = sonare_midi_cc_to_breakpoint(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), &pod, &point);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::object();
  out.set("ppq", point.ppq);
  out.set("value", point.value);
  out.set("curveToNext", static_cast<double>(point.curve_to_next));
  return out;
}

// paramId arrives as a val rather than as a declared uint32_t because embind
// converts a positional integer by the JS ToUint32 rule, which WRAPS: 2^32 + 5
// selected the binding whose id is 5, and 5.5 selected it too. The object-field
// spelling of the same field reads through checkedUintFromVal, so both paths
// refuse what neither can represent.
val js_midi_param_to_cc(val bindings, val param_id, const val& unit_value_val, const val& group_val,
                        const val& ppq_val) {
  const double ppq = checkedDoubleFromVal(ppq_val, "ppq");
  const uint32_t requested_param_id =
      domainNumberFromVal<uint32_t>(param_id, "paramId", sonare_c_detail::kMidiTargetParamIdDomain);
  const float unit_value = checkedFloatFromVal(unit_value_val, "unitValue");
  const uint8_t group =
      domainNumberFromVal<uint8_t>(group_val, "group", sonare_c_detail::kMidiUmpGroupDomain);
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), requested_param_id,
      unit_value, group, ppq, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return js_midi_event_to_val(event);
}

val js_midi_route_events(val events, val config) {
  const size_t count = wasmArrayLength(events, "events");
  std::vector<SonareMidiEventPod> input(count);
  std::vector<std::vector<uint8_t>> sysex_storage(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    input[i].ppq = numberFromVal(entry["ppq"], "ppq");
    input[i].word0 = checkedWordFromVal(entry["word0"], "word0");
    input[i].word1 = wordProperty(entry, "word1", input[i].word1);
    val sysex = entry["sysex"];
    if (!sysex.isUndefined() && !sysex.isNull()) {
      const std::string subject = "events[" + std::to_string(i) + "].sysex";
      const bool is_uint8_array =
          val::global("Uint8Array")["prototype"].call<bool>("isPrototypeOf", sysex);
      if (!is_uint8_array) {
        throw WasmTypeError(subject + " must be a Uint8Array");
      }
      sysex_storage[i] = uint8ArrayToVector(sysex, subject.c_str());
      input[i].sysex = nonNullBytes(sysex_storage[i]);
      input[i].sysex_size = sysex_storage[i].size();
    }
  }

  SonareMidiRouteConfig route;
  sonare_midi_route_config_init(&route);
  if (!config.isUndefined() && !config.isNull()) {
    // The init values are the "any group / any channel / no remap" sentinels, so the range
    // check has to keep them while refusing the values a bare cast turned into a
    // real filter: NaN reads as channel 0 and a fractional value truncates onto
    // a neighbouring channel, both of them silently.
    route.filter_group =
        intInRangeOrAbsentProperty(config, "filterGroup", 0, 15, route.filter_group);
    route.filter_channel =
        intInRangeOrAbsentProperty(config, "filterChannel", 0, 15, route.filter_channel);
    route.remap_channel =
        intInRangeOrAbsentProperty(config, "remapChannel", 0, 15, route.remap_channel);
    if (hasProperty(config, "thru")) {
      route.thru = boolProperty(config, "thru", false) ? 1 : 0;
    }
  }

  std::vector<SonareMidiEventPod> output(input.size());
  size_t output_count = 0;
  int overflowed = 0;
  size_t overflow_count = 0;
  const SonareError err =
      sonare_midi_route_events(input.empty() ? nullptr : input.data(), input.size(), &route,
                               output.empty() ? nullptr : output.data(), output.size(),
                               &output_count, &overflowed, &overflow_count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }

  val out = val::object();
  val routed = val::array();
  for (size_t i = 0; i < output_count; ++i) {
    const bool has_sysex = output[i].sysex != nullptr;
    if (has_sysex != (output[i].sysex_size != 0)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "midiRouteEvents returned an invalid SysEx payload");
    }
    val event = val::object();
    event.set("ppq", output[i].ppq);
    event.set("word0", static_cast<double>(output[i].word0));
    event.set("word1", static_cast<double>(output[i].word1));
    if (has_sysex) {
      event.set("sysex", vectorToUint8Array(output[i].sysex, output[i].sysex_size));
    }
    routed.set(static_cast<unsigned>(i), event);
  }
  out.set("events", routed);
  out.set("overflowed", overflowed != 0);
  out.set("overflowCount", static_cast<double>(overflow_count));
  return out;
}

void registerProjectMidi(class_<ProjectWasm>& cls) {
  cls.function("setMidiEvents", &ProjectWasm::setMidiEvents)
      .function("midiEvents", &ProjectWasm::midiEvents)
      .function("setDestinationInstrument", &ProjectWasm::setDestinationInstrument)
      .function("destinationInstruments", &ProjectWasm::destinationInstruments)
      .function("importSmf", &ProjectWasm::importSmf)
      .function("exportSmf", &ProjectWasm::exportSmf)
      .function("importClipFile", &ProjectWasm::importClipFile)
      .function("exportClipFile", &ProjectWasm::exportClipFile)
      .function("setProgram", &ProjectWasm::setProgram)
      .function("bakeMidiFx", &ProjectWasm::bakeMidiFx)
      .function("previewMidiFxCount", &ProjectWasm::previewMidiFxCount)
      .function("validateMidiNotes", &ProjectWasm::validateMidiNotes)
      .function("analyzeTempo", &ProjectWasm::analyzeTempo)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
