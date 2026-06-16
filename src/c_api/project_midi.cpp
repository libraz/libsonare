#include "c_api/project_internal.h"

// ============================================================================
// MIDI
// ============================================================================

SonareError sonare_project_set_midi_events(SonareProject* project, uint32_t clip_id,
                                           const SonareMidiEventPod* events, size_t count) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || (count > 0 && !events) || count > kMaxBufferSize / 16) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_clip(clip_id)) return SONARE_ERROR_INVALID_PARAMETER;
  if (find_midi_clip(project, clip_id) == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::MidiClipEventList list;
  list.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (!valid_midi_event_pod(events[i])) return SONARE_ERROR_INVALID_PARAMETER;
    arr::MidiClipEvent event;
    event.ppq = events[i].ppq;
    event.data0 = events[i].data0;
    event.data1 = events[i].data1;
    list.push_back(event);
  }
  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(list));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, events, count);
#endif
}

SonareError sonare_midi_note_on(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                uint8_t velocity, SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(note) || !valid_u7(velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_note_on(group, channel, note, velocity));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, note, velocity, out);
#endif
}

SonareError sonare_midi_note_off(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                 uint8_t velocity, SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(note) || !valid_u7(velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_note_off(group, channel, note, velocity));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, note, velocity, out);
#endif
}

SonareError sonare_midi_cc(double ppq, uint8_t group, uint8_t channel, uint8_t controller,
                           uint8_t value, SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(controller) || !valid_u7(value)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out =
      pod_from_ump(ppq, sonare::midi::make_midi1_control_change(group, channel, controller, value));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, controller, value, out);
#endif
}

SonareError sonare_midi_poly_pressure(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                      uint8_t pressure, SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(note) || !valid_u7(pressure)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_poly_pressure(group, channel, note, pressure));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, note, pressure, out);
#endif
}

SonareError sonare_midi_program(double ppq, uint8_t group, uint8_t channel, uint8_t program,
                                SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(program)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_program_change(group, channel, program));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, program, out);
#endif
}

namespace {

const char* view_to_cstr(std::string_view value) { return value.empty() ? nullptr : value.data(); }

bool valid_cc_binding_kind(uint8_t kind) noexcept {
  return kind <= static_cast<uint8_t>(SONARE_MIDI_CC_NRPN);
}

sonare::midi::Ump ump_from_pod(const SonareMidiEventPod& pod) noexcept {
  sonare::midi::Ump out{};
  out.words[0] = pod.data0;
  out.words[1] = pod.data1;
  out.word_count = pod.data1 != 0 ? 2 : 1;
  out.group = static_cast<uint8_t>((pod.data0 >> 24u) & 0x0Fu);
  return out;
}

bool binding_from_c(const SonareMidiCcBinding& src, sonare::midi::CcBinding* out) noexcept {
  if (out == nullptr || src.cc_number > 127 || src.param_id == 0 ||
      !valid_cc_binding_kind(src.kind) ||
      (src.channel != sonare::midi::kCcAnyChannel && src.channel > 15) ||
      !std::isfinite(src.min_value) || !std::isfinite(src.max_value) ||
      src.max_value < src.min_value) {
    return false;
  }
  sonare::midi::CcBinding binding{};
  binding.cc_number = src.cc_number;
  binding.channel = src.channel;
  binding.param_id = src.param_id;
  binding.min_value = src.min_value;
  binding.max_value = src.max_value;
  binding.kind = static_cast<sonare::midi::CcBindingKind>(src.kind);
  binding.cc_lsb_number = src.cc_lsb_number;
  binding.selector_msb = src.selector_msb;
  binding.selector_lsb = src.selector_lsb;
  if (binding.kind == sonare::midi::CcBindingKind::kControlChange14 &&
      (binding.cc_number > 31 || binding.cc_lsb_number != binding.cc_number + 32u)) {
    return false;
  }
  *out = binding;
  return true;
}

SonareMidiCcBinding binding_to_c(const sonare::midi::CcBinding& binding) noexcept {
  SonareMidiCcBinding out{};
  out.cc_number = binding.cc_number;
  out.channel = binding.channel;
  out.kind = static_cast<uint8_t>(binding.kind);
  out.cc_lsb_number = binding.cc_lsb_number;
  out.selector_msb = binding.selector_msb;
  out.selector_lsb = binding.selector_lsb;
  out.param_id = binding.param_id;
  out.min_value = binding.min_value;
  out.max_value = binding.max_value;
  return out;
}

bool populate_cc_map(const SonareMidiCcBinding* bindings, size_t binding_count,
                     sonare::midi::CcMap* out) {
  if (out == nullptr || (binding_count > 0 && bindings == nullptr) ||
      binding_count > sonare::midi::CcMap::kMaxBindings) {
    return false;
  }
  for (size_t i = 0; i < binding_count; ++i) {
    sonare::midi::CcBinding binding{};
    if (!binding_from_c(bindings[i], &binding) || !out->bind(binding)) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* sonare_midi_gm_instrument_name(int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (program < 0 || program > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm_instrument_name(static_cast<uint8_t>(program)));
#else
  (void)program;
  return nullptr;
#endif
}

int sonare_midi_gm_program_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::gm_program_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_gm_family_name(int family) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (family < 0 || family > 15) return nullptr;
  return view_to_cstr(sonare::midi::gm_family_name(static_cast<uint8_t>(family)));
#else
  (void)family;
  return nullptr;
#endif
}

int sonare_midi_gm_family_first_program(int family) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (family < 0 || family > 15) return -1;
  return sonare::midi::gm_family_first_program(static_cast<uint8_t>(family));
#else
  (void)family;
  return -1;
#endif
}

const char* sonare_midi_gm2_instrument_name(int bank_lsb, int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127 || program < 0 || program > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm2_instrument_name(static_cast<uint8_t>(bank_lsb),
                                                        static_cast<uint8_t>(program)));
#else
  (void)bank_lsb;
  (void)program;
  return nullptr;
#endif
}

const char* sonare_midi_gm_drum_name(int note) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (note < 0 || note > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm_drum_name(static_cast<uint8_t>(note)));
#else
  (void)note;
  return nullptr;
#endif
}

int sonare_midi_gm_drum_note_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::gm_drum_note_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_gm2_drum_set_name(int bank_lsb) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm2_drum_set_name(static_cast<uint8_t>(bank_lsb)));
#else
  (void)bank_lsb;
  return nullptr;
#endif
}

const char* sonare_midi_gm2_drum_name(int bank_lsb, int note) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127 || note < 0 || note > 127) return nullptr;
  return view_to_cstr(
      sonare::midi::gm2_drum_name(static_cast<uint8_t>(bank_lsb), static_cast<uint8_t>(note)));
#else
  (void)bank_lsb;
  (void)note;
  return nullptr;
#endif
}

const char* sonare_midi_cc_name(int controller) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (controller < 0 || controller > 127) return nullptr;
  return view_to_cstr(sonare::midi::cc_name(static_cast<uint8_t>(controller)));
#else
  (void)controller;
  return nullptr;
#endif
}

int sonare_midi_cc_index_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::cc_index_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_per_note_controller_name(int index) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (index < 0 || index > 127) return nullptr;
  return view_to_cstr(sonare::midi::per_note_controller_name(static_cast<uint8_t>(index)));
#else
  (void)index;
  return nullptr;
#endif
}

SonareError sonare_midi_bank_program(double ppq, uint8_t group, uint8_t channel, int bank_msb,
                                     int bank_lsb, int program, SonareMidiEventPod* out_events,
                                     size_t out_capacity, size_t* out_count) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = 0;
  if (!out_events || out_capacity < 3 || !finite_non_negative(ppq) || !valid_nibble(group) ||
      !valid_nibble(channel) || bank_msb < 0 || bank_msb > 127 || bank_lsb < 0 || bank_lsb > 127 ||
      program < 0 || program > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::midi::ProgramSelection selection{};
  selection.bank_msb = static_cast<uint8_t>(bank_msb);
  selection.bank_lsb = static_cast<uint8_t>(bank_lsb);
  selection.program = static_cast<uint8_t>(program);
  const sonare::midi::BankProgramMessages messages =
      sonare::midi::program_to_messages(group, channel, selection);
  for (uint8_t i = 0; i < messages.count; ++i) {
    out_events[i] = pod_from_ump(ppq, messages.messages[i]);
  }
  *out_count = messages.count;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, bank_msb, bank_lsb, program, out_events,
                              out_capacity, out_count);
#endif
}

SonareError sonare_midi_route_events(const SonareMidiEventPod* events, size_t count,
                                     const SonareMidiRouteConfig* config,
                                     SonareMidiEventPod* out_events, size_t out_capacity,
                                     size_t* out_count, int* out_overflowed,
                                     uint32_t* out_overflow_count) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = 0;
  if (out_overflowed) *out_overflowed = 0;
  if (out_overflow_count) *out_overflow_count = 0;
  if ((count > 0 && !events) || (out_capacity > 0 && !out_events) || count > kMaxBufferSize / 16) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareMidiRouteConfig default_config{-1, -1, -1, 1};
  const SonareMidiRouteConfig& c = config != nullptr ? *config : default_config;
  auto valid_route_field = [](int value) noexcept {
    return value == -1 || (value >= 0 && value <= 15);
  };
  if (!valid_route_field(c.filter_group) || !valid_route_field(c.filter_channel) ||
      !valid_route_field(c.remap_channel)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  std::vector<sonare::midi::MidiEvent> input;
  input.reserve(count);
  std::vector<double> ppq_by_index;
  ppq_by_index.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (!valid_midi_event_pod(events[i])) return SONARE_ERROR_INVALID_PARAMETER;
    sonare::midi::MidiEvent event{};
    event.render_frame = static_cast<int64_t>(i);
    event.ump.words[0] = events[i].data0;
    event.ump.words[1] = events[i].data1;
    event.ump.word_count = events[i].data1 != 0 ? 2 : 1;
    input.push_back(event);
    ppq_by_index.push_back(events[i].ppq);
  }

  sonare::midi::MidiRouteConfig route{};
  route.filter_group =
      c.filter_group == -1 ? sonare::midi::kRouteAnyGroup : static_cast<uint8_t>(c.filter_group);
  route.filter_channel = c.filter_channel == -1 ? sonare::midi::kRouteAnyChannel
                                                : static_cast<uint8_t>(c.filter_channel);
  route.remap_channel =
      c.remap_channel == -1 ? sonare::midi::kRouteNoRemap : static_cast<uint8_t>(c.remap_channel);
  route.thru = c.thru != 0;

  sonare::midi::MidiRouter router;
  router.set_config(route);
  sonare::midi::MidiRouteOutput routed;
  router.process(input.empty() ? nullptr : input.data(), input.size(), &routed);

  const size_t copied = std::min(out_capacity, routed.size);
  for (size_t i = 0; i < copied; ++i) {
    const sonare::midi::MidiEvent& event = routed.events[i];
    const size_t source_index = static_cast<size_t>(event.render_frame);
    out_events[i] = pod_from_ump(
        source_index < ppq_by_index.size() ? ppq_by_index[source_index] : 0.0, event.ump);
  }
  const uint32_t capacity_overflow =
      routed.size > out_capacity ? static_cast<uint32_t>(routed.size - out_capacity) : 0u;
  *out_count = copied;
  if (out_overflowed) *out_overflowed = (routed.overflowed || capacity_overflow > 0u) ? 1 : 0;
  if (out_overflow_count) *out_overflow_count = router.overflow_count() + capacity_overflow;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(events, count, config, out_events, out_capacity, out_count,
                              out_overflowed, out_overflow_count);
#endif
}

SonareError sonare_midi_cc_learn(const SonareMidiEventPod* events, size_t count, uint32_t param_id,
                                 float min_value, float max_value, uint8_t min_movement,
                                 SonareMidiCcBinding* out_binding) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out_binding || param_id == 0 || (count > 0 && !events) || count > kMaxBufferSize / 16 ||
      !std::isfinite(min_value) || !std::isfinite(max_value) || max_value < min_value) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::midi::CcMap map;
  map.begin_learn(param_id, min_value, max_value, min_movement);
  sonare::midi::CcBinding learned{};
  for (size_t i = 0; i < count; ++i) {
    if (!valid_midi_event_pod(events[i])) return SONARE_ERROR_INVALID_PARAMETER;
    if (map.observe_for_learn(ump_from_pod(events[i]), &learned)) {
      *out_binding = binding_to_c(learned);
      return SONARE_OK;
    }
  }
  return SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(events, count, param_id, min_value, max_value, min_movement,
                              out_binding);
#endif
}

SonareError sonare_midi_cc_to_breakpoint(const SonareMidiCcBinding* bindings, size_t binding_count,
                                         const SonareMidiEventPod* event,
                                         SonareAutomationPoint* out_point) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!event || !out_point || !valid_midi_event_pod(*event)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::midi::CcMap map;
  if (!populate_cc_map(bindings, binding_count, &map)) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<sonare::automation::Breakpoint> points;
  if (!map.cc_to_breakpoint(ump_from_pod(*event), event->ppq, &points) || points.empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  out_point->ppq = points[0].ppq;
  out_point->value = points[0].value;
  out_point->curve_to_next = 0;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(bindings, binding_count, event, out_point);
#endif
}

SonareError sonare_midi_param_to_cc(const SonareMidiCcBinding* bindings, size_t binding_count,
                                    uint32_t param_id, float unit_value, uint8_t group, double ppq,
                                    SonareMidiEventPod* out_event) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out_event || param_id == 0 || group > 15 || !finite_non_negative(ppq) ||
      !std::isfinite(unit_value)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::midi::CcMap map;
  if (!populate_cc_map(bindings, binding_count, &map)) return SONARE_ERROR_INVALID_PARAMETER;
  sonare::midi::Ump ump{};
  if (!map.param_to_cc(param_id, unit_value, group, &ump)) return SONARE_ERROR_INVALID_STATE;
  *out_event = pod_from_ump(ppq, ump);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(bindings, binding_count, param_id, unit_value, group, ppq, out_event);
#endif
}

SonareError sonare_midi_channel_pressure(double ppq, uint8_t group, uint8_t channel,
                                         uint8_t pressure, SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      !valid_u7(pressure)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_channel_pressure(group, channel, pressure));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, pressure, out);
#endif
}

SonareError sonare_midi_pitch_bend(double ppq, uint8_t group, uint8_t channel, uint16_t bend,
                                   SonareMidiEventPod* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out || !finite_non_negative(ppq) || !valid_nibble(group) || !valid_nibble(channel) ||
      bend > 16383u) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = pod_from_ump(ppq, sonare::midi::make_midi1_pitch_bend(group, channel, bend));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(ppq, group, channel, bend, out);
#endif
}

SonareError sonare_project_import_smf(SonareProject* project, const uint8_t* bytes, size_t len,
                                      uint32_t* out_first_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_first_clip_id) *out_first_clip_id = 0;
  if (!project || len == 0 || !bytes || len > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::midi::SmfImportResult result = sonare::midi::import_smf(bytes, len);
  if (!result.ok()) return SONARE_ERROR_INVALID_FORMAT;

  // The per-event PPQ already comes from the importer in quarter-note units, so
  // the shared installer derives each clip length from the largest event
  // position (or the imported end-of-track length). Marker kind / key-signature
  // fields are carried through verbatim via SmfMarker.
  return install_imported_midi(project, result.tempo_segments, result.time_signatures,
                               result.markers, result.clips, result.clip_names,
                               result.clip_lengths_ppq, result.sysex_store, out_first_clip_id,
                               result.sequence_name);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, bytes, len, out_first_clip_id);
#endif
}

SonareError sonare_project_import_clip_file(SonareProject* project, const uint8_t* bytes,
                                            size_t len, uint32_t* out_first_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_first_clip_id) *out_first_clip_id = 0;
  if (!project || len == 0 || !bytes || len > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::midi::Smf2ImportResult result = sonare::midi::import_clip_file(bytes, len);
  if (!result.ok()) return SONARE_ERROR_INVALID_FORMAT;

  // MIDI Clip Files carry no SMF "Marker" equivalent.
  const std::vector<sonare::midi::SmfMarker> markers;
  return install_imported_midi(project, result.tempo_segments, result.time_signatures, markers,
                               result.clips, result.clip_names, result.clip_lengths_ppq,
                               result.sysex_store, out_first_clip_id);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, bytes, len, out_first_clip_id);
#endif
}

SonareError sonare_project_export_clip_file(const SonareProject* project, uint8_t** out_bytes,
                                            size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bytes) *out_bytes = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_bytes || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const arr::Project& model = project->history.project();
  std::vector<sonare::transport::TempoSegment> tempo = model.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, 120.0, 0.0});
  std::vector<sonare::transport::TimeSignatureSegment> sigs = model.time_signatures();
  if (sigs.empty()) sigs.push_back({0.0, {4, 4}});

  // The MIDI Clip File is a single-clip container: merge every MIDI clip's
  // events into one clip, positioned at each clip's project start.
  sonare::midi::SysExStore sysex_store;
  std::map<uint32_t, sonare::midi::SysExHandle> exported_sysex_handles;
  sonare::midi::MidiClip merged;
  std::string name;
  const arr::MidiContentStore& midi_content = project->history.midi_content();
  for (const arr::EditClip& clip : model.clips()) {
    const arr::ClipSource* source = model.find_source(clip.source_id);
    if (source == nullptr || arr::source_kind(*source) != arr::SourceKind::kMidi) continue;
    const auto it = midi_content.events.find(clip.id);
    if (it == midi_content.events.end()) continue;
    sonare::midi::MidiClip piece = make_smf_clip_from_events(clip, it->second, midi_content,
                                                             &sysex_store, &exported_sysex_handles);
    for (const sonare::midi::MidiClipEvent& ev : piece.events()) merged.add_event(ev);
    if (name.empty()) {
      const arr::Track* track = model.find_track(clip.track_id);
      if (track != nullptr) name = track->name;
    }
  }
  merged.sort_stable();

  sonare::midi::Smf2ExportOptions options;
  options.sysex_store = &sysex_store;
  options.name = name;
  sonare::midi::Smf2ExportResult result =
      sonare::midi::export_clip_file(merged, tempo, sigs, options);
  if (!result.ok()) return SONARE_ERROR_INVALID_STATE;

  *out_len = result.bytes.size();
  *out_bytes = new uint8_t[result.bytes.size()];
  std::memcpy(*out_bytes, result.bytes.data(), result.bytes.size());
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_bytes, out_len);
#endif
}

SonareError sonare_project_export_smf(const SonareProject* project, uint8_t** out_bytes,
                                      size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bytes) *out_bytes = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_bytes || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const arr::Project& model = project->history.project();
  std::vector<sonare::transport::TempoSegment> tempo = model.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, 120.0, 0.0});
  std::vector<sonare::transport::TimeSignatureSegment> sigs = model.time_signatures();
  if (sigs.empty()) sigs.push_back({0.0, {4, 4}});

  std::vector<sonare::midi::MidiClip> clips;
  std::vector<std::string> names;
  sonare::midi::SysExStore sysex_store;
  std::map<uint32_t, sonare::midi::SysExHandle> exported_sysex_handles;
  const arr::MidiContentStore& midi_content = project->history.midi_content();
  for (const arr::EditClip& clip : model.clips()) {
    const arr::ClipSource* source = model.find_source(clip.source_id);
    if (source == nullptr || arr::source_kind(*source) != arr::SourceKind::kMidi) continue;
    const auto it = midi_content.events.find(clip.id);
    if (it == midi_content.events.end()) continue;
    sonare::midi::MidiClip smf_clip = make_smf_clip_from_events(
        clip, it->second, midi_content, &sysex_store, &exported_sysex_handles);
    if (smf_clip.events().empty()) continue;
    clips.push_back(std::move(smf_clip));
    const arr::Track* track = model.find_track(clip.track_id);
    names.push_back(track != nullptr ? track->name : std::string{});
  }

  sonare::midi::SmfExportOptions options;
  options.sysex_store = &sysex_store;
  // Carry project markers (with their kind / key-signature) onto SMF track 0 so
  // marker / text / lyric / cue / key-signature meta round-trip through export.
  options.markers.reserve(model.markers().size());
  for (const arr::ProjectMarker& m : model.markers()) {
    sonare::midi::SmfMarker out;
    out.ppq = m.ppq;
    out.text = m.name;
    out.kind = static_cast<sonare::midi::SmfMarkerKind>(m.kind);
    out.key_fifths = m.key_fifths;
    out.key_minor = m.key_minor;
    options.markers.push_back(std::move(out));
  }
  sonare::midi::SmfExportResult result =
      sonare::midi::export_smf(clips, tempo, sigs, names, options);
  if (!result.ok()) return SONARE_ERROR_INVALID_STATE;

  *out_len = result.bytes.size();
  *out_bytes = new uint8_t[result.bytes.size()];
  std::memcpy(*out_bytes, result.bytes.data(), result.bytes.size());
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_bytes, out_len);
#endif
}

SonareError sonare_project_set_program(SonareProject* project, uint32_t clip_id, int program,
                                       int bank) {
  return sonare_project_set_program_on_channel(project, clip_id, 0, 0, program, bank);
}

SonareError sonare_project_set_program_on_channel(SonareProject* project, uint32_t clip_id,
                                                  uint8_t group, uint8_t channel, int program,
                                                  int bank) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !valid_nibble(group) || !valid_nibble(channel) || program < 0 ||
      program > 127 || bank < -1 || bank > 16383) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (find_midi_clip(project, clip_id) == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::MidiClipEventList next;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) {
    next.reserve(it->second.size() + 3);
    for (const arr::MidiClipEvent& event : it->second) {
      if (!is_bank_or_program_event_for(event, group, channel)) next.push_back(event);
    }
  }

  if (bank >= 0) {
    const uint8_t msb = static_cast<uint8_t>((bank >> 7) & 0x7F);
    const uint8_t lsb = static_cast<uint8_t>(bank & 0x7F);
    next.push_back(
        event_from_ump(0.0, sonare::midi::make_midi1_control_change(group, channel, 0, msb)));
    next.push_back(
        event_from_ump(0.0, sonare::midi::make_midi1_control_change(group, channel, 32, lsb)));
  }
  next.push_back(event_from_ump(
      0.0, sonare::midi::make_midi1_program_change(group, channel, static_cast<uint8_t>(program))));

  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(next));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, group, channel, program, bank);
#endif
}

SonareError sonare_project_validate_midi_notes(const SonareProject* project, uint32_t clip_id,
                                               SonareNotePairValidation* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  out->ok = 1;
  out->unmatched_note_ons = 0;
  out->unmatched_note_offs = 0;
  if (find_midi_clip(project, clip_id) == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::midi::MidiClip clip;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) {
    for (const arr::MidiClipEvent& event : it->second) {
      sonare::midi::MidiClipEvent ev;
      ev.ppq = event.ppq;
      ev.ump.words[0] = event.data0;
      ev.ump.words[1] = event.data1;
      ev.ump.word_count = ump_word_count_from_word0(event.data0);
      ev.ump.group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
      clip.add_event(ev);
    }
  }
  clip.sort_stable();
  const sonare::midi::NotePairValidation v = clip.validate_note_pairs();
  out->ok = v.ok ? 1 : 0;
  out->unmatched_note_ons = v.unmatched_note_ons;
  out->unmatched_note_offs = v.unmatched_note_offs;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, out);
#endif
}

SonareError sonare_project_bake_midi_fx(SonareProject* project, uint32_t clip_id,
                                        const char* config_json) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
  if (find_midi_clip(project, clip_id) == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  const SonareError parse_err = midi_fx_chain_from_json(config_json, &chain);
  if (parse_err != SONARE_OK) return parse_err;

  arr::MidiClipEventList current;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) current = it->second;
  arr::MidiClipEventList transformed = apply_midi_fx_to_events(current, &chain);
  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(transformed));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, config_json);
#endif
}

SonareError sonare_project_set_midi_fx(SonareProject* project, uint32_t clip_id,
                                       const char* config_json) {
  return sonare_project_bake_midi_fx(project, clip_id, config_json);
}
