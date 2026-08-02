/// @file project_midi.cpp
/// @brief Embind project facade: MIDI content (events, SMF/clip-file IO,
/// program/bank, MIDI FX, note validation) plus the standalone MIDI helper /
/// GM-table free functions.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

void ProjectWasm::setMidiEvents(uint32_t clip_id, val events) {
  const size_t count = events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
  std::vector<SonareMidiEventPod> pods(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    if (val::global("Array").call<bool>("isArray", entry)) {
      pods[i].ppq = entry[0].as<double>();
      pods[i].data0 = entry[1].as<uint32_t>();
      pods[i].data1 = entry[2].as<uint32_t>();
    } else {
      pods[i].ppq = entry["ppq"].as<double>();
      pods[i].data0 = entry["data0"].as<uint32_t>();
      pods[i].data1 = hasProperty(entry, "data1") ? entry["data1"].as<uint32_t>() : 0;
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

void ProjectWasm::setProgram(uint32_t clip_id, int program, int bank) {
  const SonareError err = sonare_project_set_program(project_.get(), clip_id, program, bank);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI program");
  }
}

void ProjectWasm::setProgramOnChannel(uint32_t clip_id, uint32_t group, uint32_t channel,
                                      int program, int bank) {
  const SonareError err =
      sonare_project_set_program_on_channel(project_.get(), clip_id, static_cast<uint8_t>(group),
                                            static_cast<uint8_t>(channel), program, bank);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI program");
  }
}

void ProjectWasm::bakeMidiFx(uint32_t clip_id, const std::string& config_json) {
  const SonareError err = sonare_project_bake_midi_fx(project_.get(), clip_id, config_json.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set MIDI FX");
  }
}

void ProjectWasm::setMidiFx(uint32_t clip_id, const std::string& config_json) {
  bakeMidiFx(clip_id, config_json);
}

val ProjectWasm::validateMidiNotes(uint32_t clip_id) {
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

val ProjectWasm::analyzeTempo(val audio, int sample_rate) {
  std::vector<float> samples = float32ArrayToVector(audio);
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  const SonareError err =
      sonare_project_analyze_tempo(project_.get(), samples.data(), samples.size(), sample_rate,
                                   candidates, std::size(candidates), &count);
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

float ProjectWasm::autoTempo(val audio, int sample_rate, int candidate_index,
                             bool apply_time_signatures) {
  std::vector<float> samples = float32ArrayToVector(audio);
  float bpm = 0.0f;
  const SonareError err = sonare_project_auto_tempo_ex(
      project_.get(), samples.data(), samples.size(), sample_rate,
      static_cast<size_t>(std::max(candidate_index, 0)), apply_time_signatures ? 1 : 0, &bpm);
  if (err != SONARE_OK) throwCError(err, "failed to detect project tempo");
  return bpm;
}

double ProjectWasm::snapToGrid(double ppq, double strength, int division) {
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

val js_midi_gm_instrument_name(int program) {
  return js_nullable_string(sonare_midi_gm_instrument_name(program));
}

int js_midi_gm_program_for_name(const std::string& name) {
  return sonare_midi_gm_program_for_name(name.c_str());
}

val js_midi_gm_family_name(int family) {
  return js_nullable_string(sonare_midi_gm_family_name(family));
}

int js_midi_gm_family_first_program(int family) {
  return sonare_midi_gm_family_first_program(family);
}

val js_midi_gm2_instrument_name(int bank_lsb, int program) {
  return js_nullable_string(sonare_midi_gm2_instrument_name(bank_lsb, program));
}

val js_midi_gm_drum_name(int note) { return js_nullable_string(sonare_midi_gm_drum_name(note)); }

int js_midi_gm_drum_note_for_name(const std::string& name) {
  return sonare_midi_gm_drum_note_for_name(name.c_str());
}

val js_midi_gm2_drum_set_name(int bank_lsb) {
  return js_nullable_string(sonare_midi_gm2_drum_set_name(bank_lsb));
}

val js_midi_gm2_drum_name(int bank_lsb, int note) {
  return js_nullable_string(sonare_midi_gm2_drum_name(bank_lsb, note));
}

val js_midi_cc_name(int controller) { return js_nullable_string(sonare_midi_cc_name(controller)); }

int js_midi_cc_index_for_name(const std::string& name) {
  return sonare_midi_cc_index_for_name(name.c_str());
}

val js_midi_per_note_controller_name(int index) {
  return js_nullable_string(sonare_midi_per_note_controller_name(index));
}

val js_midi_bank_program(double ppq, int group, int channel, int bank_msb, int bank_lsb,
                         int program) {
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
  out.data0 = event["data0"].as<uint32_t>();
  out.data1 = hasProperty(event, "data1") ? event["data1"].as<uint32_t>() : 0;
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
  out.cc_number = object["ccNumber"].as<uint8_t>();
  out.channel = hasProperty(object, "channel") && !object["channel"].isNull()
                    ? object["channel"].as<uint8_t>()
                    : 0xffu;
  out.kind = hasProperty(object, "kind") ? object["kind"].as<uint8_t>() : 0u;
  out.cc_lsb_number = hasProperty(object, "ccLsbNumber") ? object["ccLsbNumber"].as<uint8_t>() : 0u;
  out.selector_msb = hasProperty(object, "selectorMsb") ? object["selectorMsb"].as<uint8_t>() : 0u;
  out.selector_lsb = hasProperty(object, "selectorLsb") ? object["selectorLsb"].as<uint8_t>() : 0u;
  out.param_id = object["paramId"].as<uint32_t>();
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
      bindings.isUndefined() || bindings.isNull() ? 0 : bindings["length"].as<size_t>();
  std::vector<SonareMidiCcBinding> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = js_cc_binding_from_val(bindings[i]);
  }
  return out;
}

val js_midi_cc_learn(val events, uint32_t param_id, float min_value, float max_value,
                     int min_movement) {
  const size_t count = events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
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

val js_midi_param_to_cc(val bindings, uint32_t param_id, float unit_value, int group, double ppq) {
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), param_id, unit_value,
      static_cast<uint8_t>(group), ppq, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throwCError(err, "invalid MIDI param-to-CC arguments");
  }
  return js_midi_event_to_val(event);
}

val js_midi_route_events(val events, val config) {
  const size_t count = events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
  std::vector<SonareMidiEventPod> input(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    input[i].ppq = entry["ppq"].as<double>();
    input[i].data0 = entry["data0"].as<uint32_t>();
    input[i].data1 = hasProperty(entry, "data1") ? entry["data1"].as<uint32_t>() : 0;
  }

  SonareMidiRouteConfig route{-1, -1, -1, 1};
  if (!config.isUndefined() && !config.isNull()) {
    if (hasProperty(config, "filterGroup") && !config["filterGroup"].isNull()) {
      route.filter_group = config["filterGroup"].as<int>();
    }
    if (hasProperty(config, "filterChannel") && !config["filterChannel"].isNull()) {
      route.filter_channel = config["filterChannel"].as<int>();
    }
    if (hasProperty(config, "remapChannel") && !config["remapChannel"].isNull()) {
      route.remap_channel = config["remapChannel"].as<int>();
    }
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
      .function("setMidiFx", &ProjectWasm::setMidiFx)
      .function("validateMidiNotes", &ProjectWasm::validateMidiNotes)
      .function("analyzeTempo", &ProjectWasm::analyzeTempo)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
