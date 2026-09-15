/// @file project_midi.cpp
/// @brief Embind project facade: MIDI content (events, SMF/clip-file IO,
/// program/bank, MIDI FX, note validation) plus the standalone MIDI helper /
/// GM-table free functions.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

void ProjectWasm::setMidiEvents(const val& clip_id_val, val events) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const size_t count =
      events.isUndefined() || events.isNull() ? 0 : wasmArrayLikeLength(events, "events");
  std::vector<SonareMidiEventPod> pods(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    if (val::global("Array").call<bool>("isArray", entry)) {
      pods[i].ppq = entry[0].as<double>();
      pods[i].data0 = checkedWordFromVal(entry[1], "data0");
      pods[i].data1 = checkedWordFromVal(entry[2], "data1");
    } else {
      pods[i].ppq = entry["ppq"].as<double>();
      pods[i].data0 = checkedWordFromVal(entry["data0"], "data0");
      pods[i].data1 = wordProperty(entry, "data1", 0);
    }
  }
  const SonareError err = sonare_project_set_midi_events(
      project_.get(), clip_id, pods.empty() ? nullptr : pods.data(), pods.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI events");
  }
}

uint32_t ProjectWasm::importSmf(val data) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  uint32_t first_clip = 0;
  const SonareError err = sonare_project_import_smf(
      project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size(), &first_clip);
  if (err != SONARE_OK) {
    throwCError(err, "failed to import SMF");
  }
  return first_clip;
}

val ProjectWasm::exportSmf() {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_export_smf(project_.get(), &bytes, &len);
  if (err != SONARE_OK) {
    sonare_free_bytes(bytes);
    throwCError(err, "failed to export SMF");
  }
  std::vector<uint8_t> out(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return vectorToUint8Array(out);
}

uint32_t ProjectWasm::importClipFile(val data) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  uint32_t first_clip = 0;
  const SonareError err = sonare_project_import_clip_file(
      project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size(), &first_clip);
  if (err != SONARE_OK) {
    throwCError(err, "failed to import MIDI Clip File");
  }
  return first_clip;
}

val ProjectWasm::exportClipFile() {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_export_clip_file(project_.get(), &bytes, &len);
  if (err != SONARE_OK) {
    sonare_free_bytes(bytes);
    throwCError(err, "failed to export MIDI Clip File");
  }
  std::vector<uint8_t> out(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return vectorToUint8Array(out);
}

void ProjectWasm::setProgram(const val& clip_id_val, const val& program_val, const val& bank_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const int program = checkedIntFromVal(program_val, "program");
  const int bank = checkedIntFromVal(bank_val, "bank");
  const SonareError err = sonare_project_set_program(project_.get(), clip_id, program, bank);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI program");
  }
}

void ProjectWasm::setProgramOnChannel(const val& clip_id_val, const val& group_val,
                                      const val& channel_val, const val& program_val,
                                      const val& bank_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t group = checkedUintFromVal(group_val, "group");
  const uint32_t channel = checkedUintFromVal(channel_val, "channel");
  const int program = checkedIntFromVal(program_val, "program");
  const int bank = checkedIntFromVal(bank_val, "bank");
  const SonareError err =
      sonare_project_set_program_on_channel(project_.get(), clip_id, static_cast<uint8_t>(group),
                                            static_cast<uint8_t>(channel), program, bank);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI program");
  }
}

void ProjectWasm::bakeMidiFx(const val& clip_id_val, const std::string& config_json) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const SonareError err = sonare_project_bake_midi_fx(project_.get(), clip_id, config_json.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI FX");
  }
}

val ProjectWasm::bakeMidiFxWithSourceIndex(const val& clip_id_val, const std::string& config_json) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  // Sized from the non-destructive preview so the bake is a single pass with an
  // exactly-fitting buffer, rather than a bake that has to be repeated when the
  // guess is short.
  size_t expected = 0;
  const SonareError count_err =
      sonare_project_preview_midi_fx_count(project_.get(), clip_id, config_json.c_str(), &expected);
  if (count_err != SONARE_OK) {
    throwCError(count_err, "failed to set MIDI FX");
  }
  static_assert(sizeof(int) == sizeof(int32_t), "vectorToInt32Array assumes a 32-bit int");
  std::vector<int> source_index(expected, -1);
  size_t written = 0;
  const SonareError err =
      sonare_project_bake_midi_fx_ex(project_.get(), clip_id, config_json.c_str(),
                                     source_index.data(), source_index.size(), &written);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI FX");
  }
  source_index.resize(std::min(written, source_index.size()));
  return vectorToInt32Array(source_index);
}

uint32_t ProjectWasm::previewMidiFxCount(const val& clip_id_val, const std::string& config_json) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  size_t count = 0;
  const SonareError err =
      sonare_project_preview_midi_fx_count(project_.get(), clip_id, config_json.c_str(), &count);
  if (err != SONARE_OK) {
    throwCError(err, "failed to preview MIDI FX");
  }
  return static_cast<uint32_t>(count);
}

void ProjectWasm::setMidiFx(const val& clip_id, const std::string& config_json) {
  bakeMidiFx(clip_id, config_json);
}

val ProjectWasm::validateMidiNotes(const val& clip_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  SonareNotePairValidation result{};
  const SonareError err = sonare_project_validate_midi_notes(project_.get(), clip_id, &result);
  if (err != SONARE_OK) {
    throwCError(err, "failed to validate MIDI notes");
  }
  val out = val::object();
  out.set("ok", result.ok != 0);
  out.set("unmatchedNoteOns", static_cast<double>(result.unmatched_note_ons));
  out.set("unmatchedNoteOffs", static_cast<double>(result.unmatched_note_offs));
  return out;
}

namespace {

// Seeds the native defaults, then applies whichever the caller supplied.
// Seeding rather than zeroing matters: a zeroed ramp_threshold folds the whole
// take into one tempo segment and a zeroed interval is rejected.
SonareProjectTempoOptions tempoOptionsFrom(val options) {
  SonareProjectTempoOptions out = sonare_project_tempo_options_default();
  if (options.isUndefined() || options.isNull()) return out;
  // The presence-checked *Property readers, not a bare object["key"] read: every
  // field here is optional and the seeded default is the value to keep when one
  // is omitted. Range rules stay with the C ABI, which rejects what it cannot
  // use rather than silently substituting a default.
  out.adaptive_tempo = boolProperty(options, "adaptiveTempo", out.adaptive_tempo != 0) ? 1 : 0;
  out.tempo_update_interval_beats =
      intProperty(options, "tempoUpdateIntervalBeats", out.tempo_update_interval_beats);
  out.ramp_threshold = floatProperty(options, "rampThreshold", out.ramp_threshold);
  out.include_octave_candidates =
      boolProperty(options, "includeOctaveCandidates", out.include_octave_candidates != 0) ? 1 : 0;
  return out;
}

}  // namespace

val ProjectWasm::analyzeTempo(val audio, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> samples = float32ArrayToVector(audio);
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  const SonareProjectTempoOptions resolved = tempoOptionsFrom(options);
  const SonareError err = sonare_project_analyze_tempo_with_options(
      project_.get(), samples.data(), samples.size(), sample_rate, &resolved, candidates,
      std::size(candidates), &count);
  if (err != SONARE_OK) {
    throwCError(err, "failed to analyze project tempo");
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

float ProjectWasm::autoTempo(val audio, const val& sample_rate_val, const val& candidate_index_val,
                             bool apply_time_signatures, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int candidate_index = checkedIntFromVal(candidate_index_val, "candidateIndex");
  std::vector<float> samples = float32ArrayToVector(audio);
  float bpm = 0.0f;
  const SonareProjectTempoOptions resolved = tempoOptionsFrom(options);
  const SonareError err = sonare_project_auto_tempo_with_options(
      project_.get(), samples.data(), samples.size(), sample_rate, &resolved,
      static_cast<size_t>(std::max(candidate_index, 0)), apply_time_signatures ? 1 : 0, &bpm);
  if (err != SONARE_OK) throwCError(err, "failed to detect project tempo");
  return bpm;
}

double ProjectWasm::snapToGrid(double ppq, double strength, const val& division_val) {
  const int division = checkedIntFromVal(division_val, "division");
  double out = 0.0;
  const SonareError err =
      sonare_project_snap_to_grid_ex(project_.get(), ppq, strength, division, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to snap to grid");
  }
  return out;
}

uint32_t js_project_abi_version() { return sonare_project_abi_version(); }

val js_nullable_string(const char* value) {
  return value != nullptr ? val(std::string(value)) : val::null();
}

val js_midi_gm_instrument_name(const val& program) {
  return js_nullable_string(sonare_midi_gm_instrument_name(checkedIntFromVal(program, "program")));
}

int js_midi_gm_program_for_name(const std::string& name) {
  return sonare_midi_gm_program_for_name(name.c_str());
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
  return sonare_midi_gm_drum_note_for_name(name.c_str());
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
  return sonare_midi_cc_index_for_name(name.c_str());
}

val js_midi_per_note_controller_name(const val& index) {
  return js_nullable_string(
      sonare_midi_per_note_controller_name(checkedIntFromVal(index, "index")));
}

val js_midi_bank_program(double ppq, const val& group_val, const val& channel_val,
                         const val& bank_msb_val, const val& bank_lsb_val, const val& program_val) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int bank_msb = checkedIntFromVal(bank_msb_val, "bankMsb");
  const int bank_lsb = checkedIntFromVal(bank_lsb_val, "bankLsb");
  const int program = checkedIntFromVal(program_val, "program");
  SonareMidiEventPod events[3]{};
  size_t count = 0;
  const SonareError err =
      sonare_midi_bank_program(ppq, static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                               bank_msb, bank_lsb, program, events, 3, &count);
  if (err != SONARE_OK) {
    throwCError(err, "invalid MIDI bank/program arguments");
  }
  val out = val::array();
  for (size_t i = 0; i < count; ++i) {
    val event = val::object();
    event.set("ppq", events[i].ppq);
    event.set("data0", static_cast<double>(events[i].data0));
    event.set("data1", static_cast<double>(events[i].data1));
    out.set(static_cast<unsigned>(i), event);
  }
  return out;
}

SonareMidiEventPod js_midi_event_from_val(val event) {
  SonareMidiEventPod out{};
  out.ppq = event["ppq"].as<double>();
  out.data0 = checkedWordFromVal(event["data0"], "data0");
  out.data1 = wordProperty(event, "data1", 0);
  return out;
}

val js_midi_event_to_val(const SonareMidiEventPod& event) {
  val out = val::object();
  out.set("ppq", event.ppq);
  out.set("data0", static_cast<double>(event.data0));
  out.set("data1", static_cast<double>(event.data1));
  return out;
}

SonareMidiCcBinding js_cc_binding_from_val(val object) {
  SonareMidiCcBinding out{};
  out.cc_number = checkedByteFromVal(object["ccNumber"], "ccNumber");
  out.channel = byteProperty(object, "channel", 0xffu);
  out.kind = byteProperty(object, "kind", 0u);
  out.cc_lsb_number = byteProperty(object, "ccLsbNumber", 0u);
  out.selector_msb = byteProperty(object, "selectorMsb", 0u);
  out.selector_lsb = byteProperty(object, "selectorLsb", 0u);
  out.param_id = checkedUintFromVal(object["paramId"], "paramId");
  out.min_value = hasProperty(object, "minValue") ? object["minValue"].as<float>() : 0.0f;
  out.max_value = hasProperty(object, "maxValue") ? object["maxValue"].as<float>() : 1.0f;
  return out;
}

val js_cc_binding_to_val(const SonareMidiCcBinding& binding) {
  val out = val::object();
  out.set("ccNumber", static_cast<double>(binding.cc_number));
  out.set("channel", static_cast<double>(binding.channel));
  out.set("kind", static_cast<double>(binding.kind));
  out.set("ccLsbNumber", static_cast<double>(binding.cc_lsb_number));
  out.set("selectorMsb", static_cast<double>(binding.selector_msb));
  out.set("selectorLsb", static_cast<double>(binding.selector_lsb));
  out.set("paramId", static_cast<double>(binding.param_id));
  out.set("minValue", binding.min_value);
  out.set("maxValue", binding.max_value);
  return out;
}

std::vector<SonareMidiCcBinding> js_cc_bindings_from_val(val bindings) {
  const size_t count =
      bindings.isUndefined() || bindings.isNull() ? 0 : wasmArrayLikeLength(bindings, "bindings");
  std::vector<SonareMidiCcBinding> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = js_cc_binding_from_val(bindings[i]);
  }
  return out;
}

val js_midi_cc_learn(val events, const val& param_id_val, float min_value, float max_value,
                     const val& min_movement_val) {
  const uint32_t param_id = checkedUintFromVal(param_id_val, "paramId");
  const int min_movement = checkedIntFromVal(min_movement_val, "minMovement");
  const size_t count =
      events.isUndefined() || events.isNull() ? 0 : wasmArrayLikeLength(events, "events");
  std::vector<SonareMidiEventPod> pods(count);
  for (size_t i = 0; i < count; ++i) {
    pods[i] = js_midi_event_from_val(events[i]);
  }
  SonareMidiCcBinding learned{};
  const SonareError err =
      sonare_midi_cc_learn(pods.empty() ? nullptr : pods.data(), pods.size(), param_id, min_value,
                           max_value, static_cast<uint8_t>(min_movement), &learned);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCError(err, "invalid MIDI CC learn arguments");
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
    throwCError(err, "invalid MIDI CC breakpoint arguments");
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
val js_midi_param_to_cc(val bindings, val param_id, float unit_value, const val& group_val,
                        double ppq) {
  const uint32_t requested_param_id = checkedUintFromVal(param_id, "paramId");
  const int group = checkedIntFromVal(group_val, "group");
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), requested_param_id,
      unit_value, static_cast<uint8_t>(group), ppq, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCError(err, "invalid MIDI param-to-CC arguments");
  }
  return js_midi_event_to_val(event);
}

val js_midi_route_events(val events, val config) {
  const size_t count =
      events.isUndefined() || events.isNull() ? 0 : wasmArrayLikeLength(events, "events");
  std::vector<SonareMidiEventPod> input(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    input[i].ppq = entry["ppq"].as<double>();
    input[i].data0 = checkedWordFromVal(entry["data0"], "data0");
    input[i].data1 = wordProperty(entry, "data1", 0);
  }

  SonareMidiRouteConfig route{-1, -1, -1, 1};
  if (!config.isUndefined() && !config.isNull()) {
    // -1 is the "any group / any channel / no remap" sentinel, so the range
    // check has to keep it while refusing the values a bare cast turned into a
    // real filter: NaN reads as channel 0 and a fractional value truncates onto
    // a neighbouring channel, both of them silently.
    route.filter_group = intProperty(config, "filterGroup", -1);
    route.filter_channel = intProperty(config, "filterChannel", -1);
    route.remap_channel = intProperty(config, "remapChannel", -1);
    if (hasProperty(config, "thru")) {
      route.thru = config["thru"].as<bool>() ? 1 : 0;
    }
  }

  std::vector<SonareMidiEventPod> output(input.size());
  size_t output_count = 0;
  int overflowed = 0;
  uint32_t overflow_count = 0;
  const SonareError err =
      sonare_midi_route_events(input.empty() ? nullptr : input.data(), input.size(), &route,
                               output.empty() ? nullptr : output.data(), output.size(),
                               &output_count, &overflowed, &overflow_count);
  if (err != SONARE_OK) {
    throwCError(err, "invalid MIDI route arguments");
  }

  val out = val::object();
  val routed = val::array();
  for (size_t i = 0; i < output_count; ++i) {
    val event = val::object();
    event.set("ppq", output[i].ppq);
    event.set("data0", static_cast<double>(output[i].data0));
    event.set("data1", static_cast<double>(output[i].data1));
    routed.set(static_cast<unsigned>(i), event);
  }
  out.set("events", routed);
  out.set("overflowed", overflowed != 0);
  out.set("overflowCount", static_cast<double>(overflow_count));
  return out;
}

void registerProjectMidi(class_<ProjectWasm>& cls) {
  cls.function("setMidiEvents", &ProjectWasm::setMidiEvents)
      .function("importSmf", &ProjectWasm::importSmf)
      .function("exportSmf", &ProjectWasm::exportSmf)
      .function("importClipFile", &ProjectWasm::importClipFile)
      .function("exportClipFile", &ProjectWasm::exportClipFile)
      .function("setProgram", &ProjectWasm::setProgram)
      .function("setProgramOnChannel", &ProjectWasm::setProgramOnChannel)
      .function("bakeMidiFx", &ProjectWasm::bakeMidiFx)
      .function("bakeMidiFxWithSourceIndex", &ProjectWasm::bakeMidiFxWithSourceIndex)
      .function("previewMidiFxCount", &ProjectWasm::previewMidiFxCount)
      .function("setMidiFx", &ProjectWasm::setMidiFx)
      .function("validateMidiNotes", &ProjectWasm::validateMidiNotes)
      .function("analyzeTempo", &ProjectWasm::analyzeTempo)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
