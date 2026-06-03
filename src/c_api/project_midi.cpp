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

  std::vector<std::pair<double, std::string>> markers;
  markers.reserve(result.markers.size());
  for (const auto& marker : result.markers) markers.emplace_back(marker.ppq, marker.text);

  // The per-event PPQ already comes from the importer in quarter-note units, so
  // the shared installer derives each clip length from the largest event
  // position (or the imported end-of-track length).
  return install_imported_midi(project, result.tempo_segments, result.time_signatures, markers,
                               result.clips, result.clip_names, result.clip_lengths_ppq,
                               result.sysex_store, out_first_clip_id);
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
  const std::vector<std::pair<double, std::string>> markers;
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

SonareError sonare_project_set_midi_fx(SonareProject* project, uint32_t clip_id,
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
