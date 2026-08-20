#pragma once

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "analysis/beat_analyzer.h"
#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "c_api/midi_fx_json.h"
#include "core/audio.h"
#include "engine/realtime_engine.h"
#include "midi/cc_map.h"
#include "midi/midi_fx.h"
#include "midi/program_map.h"
#include "midi/routing.h"
#include "midi/smf.h"
#include "midi/smf2.h"
#include "midi/synth/sf2_file.h"
#include "midi/ump.h"
#include "mir/grid_snap.h"
#include "mir/tempo_estimator_bridge.h"
#include "rt/command.h"
#include "serialize/project_serializer.h"
#include "transport/tempo_map.h"
#include "util/constants.h"
#include "util/json.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"
#endif

using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)

namespace arr = sonare::arrangement;
namespace json = sonare::util::json;

// Pin the C track-kind ordinals to the C++ enum so a reorder is caught.
static_assert(static_cast<int>(arr::Track::Kind::kAudio) == SONARE_TRACK_AUDIO,
              "SonareProjectTrackKind audio ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kMidi) == SONARE_TRACK_MIDI,
              "SonareProjectTrackKind midi ordinal drift");
static_assert(static_cast<int>(arr::Track::Kind::kAux) == SONARE_TRACK_AUX,
              "SonareProjectTrackKind aux ordinal drift");

struct SonareProject {
  arr::EditHistory history;
  arr::AudioContentStore audio;
  std::vector<arr::Diagnostic> last_bounce_diagnostics;
  bool last_bounce_has_timeline = false;
  /// Loaded SoundFont (sonare_project_load_soundfont); shared read-only with
  /// the SF2 players a bounce creates. Null until a load succeeds.
  std::shared_ptr<const sonare::midi::synth::Sf2File> soundfont;
};

// Compound public removals keep decoded PCM outside the arrangement model.
// These helpers pair source-registry GC with AudioContentStore ownership so
// RemoveClip and RemoveTrack can use one undoable transaction.
arr::EditCommandPtr sonare_project_make_remove_audio_content_command(
    arr::AudioContentStore* store, std::vector<arr::SourceId> source_ids);
std::vector<arr::SourceId> sonare_project_collect_orphaned_sources_for_track(
    const arr::Project& project, arr::TrackId removed_track_id);

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

/// Deinterleaves an interleaved float buffer into per-channel vectors.
std::vector<std::vector<float>> deinterleave(const float* interleaved, int64_t frames,
                                             int channels) {
  std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                      std::vector<float>(static_cast<size_t>(frames), 0.0f));
  for (int64_t frame = 0; frame < frames; ++frame) {
    for (int ch = 0; ch < channels; ++ch) {
      out[static_cast<size_t>(ch)][static_cast<size_t>(frame)] = interleaved[frame * channels + ch];
    }
  }
  return out;
}

bool finite_non_negative(double value) noexcept { return std::isfinite(value) && value >= 0.0; }

bool finite_positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }

bool valid_u7(uint8_t value) noexcept { return value <= 127; }

bool valid_nibble(uint8_t value) noexcept { return value <= 15; }

SonareError validate_audio_clip_payload(const SonareProjectClipDesc* desc, size_t* out_samples) {
  if (out_samples) *out_samples = 0;
  if (!desc->audio_interleaved && desc->audio_frames == 0) {
    return SONARE_OK;
  }
  if (!desc->audio_interleaved || desc->audio_frames <= 0 || desc->audio_channels <= 0 ||
      desc->audio_sample_rate < kMinSampleRate || desc->audio_sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto frames = static_cast<uint64_t>(desc->audio_frames);
  const auto channels = static_cast<uint64_t>(desc->audio_channels);
  if (channels == 0 || frames > std::numeric_limits<size_t>::max() / channels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t total = static_cast<size_t>(frames * channels);
  if (total == 0 || total > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < total; ++i) {
    if (!std::isfinite(desc->audio_interleaved[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (out_samples) *out_samples = total;
  return SONARE_OK;
}

void rollback_to_depth(SonareProject* project, size_t undo_depth) {
  if (project == nullptr) return;
  while (project->history.undo_depth() > undo_depth) {
    if (!project->history.undo()) break;
  }
}

SonareMidiEventPod pod_from_ump(double ppq, const sonare::midi::Ump& ump) {
  SonareMidiEventPod out{};
  out.ppq = ppq;
  out.data0 = ump.words[0];
  out.data1 = ump.words[1];
  return out;
}

void fill_compile_result_from_diagnostics(const std::vector<arr::Diagnostic>& diagnostics,
                                          bool has_timeline, SonareProjectCompileResult* out) {
  if (out == nullptr) return;
  *out = {};
  out->has_timeline = has_timeline ? 1 : 0;
  out->diagnostic_count = diagnostics.size();
  if (diagnostics.empty()) return;

  out->diagnostics = new SonareProjectDiagnostic[diagnostics.size()];
  std::ostringstream stream;
  for (size_t i = 0; i < diagnostics.size(); ++i) {
    const arr::Diagnostic& d = diagnostics[i];
    out->diagnostics[i].code = static_cast<uint32_t>(d.code);
    out->diagnostics[i].severity = static_cast<uint32_t>(d.severity);
    out->diagnostics[i].target_id = d.target_id;
    if (i > 0) stream << '\n';
    std::string message = d.message;
    std::replace(message.begin(), message.end(), '\n', ' ');
    std::replace(message.begin(), message.end(), '\r', ' ');
    stream << message;
  }
  out->messages = copy_string(stream.str());
}

bool valid_midi_event_pod(const SonareMidiEventPod& event) noexcept {
  if (!sonare::transport::valid_public_ppq(event.ppq)) return false;
  const uint8_t mt = static_cast<uint8_t>((event.data0 >> 28u) & 0x0Fu);
  if (mt != static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi1ChannelVoice) &&
      mt != static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi2ChannelVoice)) {
    return false;
  }
  return true;
}

const arr::EditClip* find_midi_clip(const SonareProject* project, uint32_t clip_id) {
  if (project == nullptr || clip_id == 0) return nullptr;
  const arr::EditClip* clip = project->history.project().find_clip(clip_id);
  if (clip == nullptr) return nullptr;
  const arr::ClipSource* source = project->history.project().find_source(clip->source_id);
  if (source == nullptr || arr::source_kind(*source) != arr::SourceKind::kMidi) return nullptr;
  return clip;
}

arr::MidiClipEvent event_from_ump(double ppq, const sonare::midi::Ump& ump) {
  arr::MidiClipEvent event;
  event.ppq = ppq;
  event.data0 = ump.words[0];
  event.data1 = ump.words[1];
  event.sysex_handle = ump.sysex_handle;
  return event;
}

bool is_bank_or_program_event_for(const arr::MidiClipEvent& event, uint8_t group,
                                  uint8_t channel) noexcept {
  if (event.ppq != 0.0) return false;
  sonare::midi::Ump ump;
  ump.words[0] = event.data0;
  ump.words[1] = event.data1;
  ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
  ump.group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
  if (ump.group != group || ump.channel() != channel) return false;
  if (ump.message_type() != sonare::midi::UmpMessageType::kMidi1ChannelVoice) return false;
  const uint8_t status = ump.status_nibble();
  if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange)) return true;
  if (status != static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange)) return false;
  const uint8_t cc = ump.note_number();
  return cc == 0 || cc == 32;
}

/// Applies @p chain to @p events and writes the canonically ordered result to
/// @p transformed. When @p source_index is non-null it receives one entry per
/// transformed event: the index of the input event that event derives from.
/// Every entry is non-negative -- the only writer fills each chunk with its own
/// input's ordinal and the permutation sort merely reorders, so there is no
/// path that produces an unattributed event. Building the map narrows the
/// drain to one input per chunk so each output can be attributed, which is
/// output-identical to the wide-chunk drain because the ordinal base is passed
/// explicitly either way.
bool apply_midi_fx_to_events(const arr::MidiClipEventList& events, sonare::midi::MidiFxChain* chain,
                             arr::MidiClipEventList* transformed,
                             std::vector<int32_t>* source_index = nullptr) {
  if (chain == nullptr || transformed == nullptr) return false;
  std::vector<sonare::midi::MidiEvent> in;
  in.reserve(events.size());
  for (const arr::MidiClipEvent& event : events) {
    sonare::midi::MidiEvent midi_event;
    if (!sonare::transport::valid_public_ppq(event.ppq) ||
        !sonare::numeric::checked_round_cast(
            event.ppq * static_cast<double>(sonare::midi::kMidiFxPpqScale),
            &midi_event.render_frame)) {
      return false;
    }
    midi_event.ump.words[0] = event.data0;
    midi_event.ump.words[1] = event.data1;
    midi_event.ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
    midi_event.ump.group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
    midi_event.ump.sysex_handle = event.sysex_handle;
    in.push_back(midi_event);
  }
  // A single input can expand to at most 8 chord notes * 16 arpeggiator
  // steps * an on/off pair. Drain at most two inputs per fixed RT buffer, then
  // append into a control-thread vector. The ordinal base preserves humanize
  // determinism as if the entire clip had been processed in one call.
  constexpr size_t kMaxOutputPerInput =
      sonare::midi::ChordConfig::kMaxChordNotes * sonare::midi::ArpeggiatorConfig::kMaxArpSteps * 2;
  constexpr size_t kInputsPerChunk = sonare::midi::MidiFxBuffer::kCapacity / kMaxOutputPerInput;
  static_assert(kInputsPerChunk > 0);

  // One input per chunk when the caller wants provenance: every event a chunk
  // emits is then causally the expansion of that single input, which is what
  // makes the map meaningful. Chord and arpeggiator fan-out therefore lands
  // several outputs on the same source index.
  const size_t inputs_per_chunk = source_index != nullptr ? size_t{1} : kInputsPerChunk;

  chain->reset();
  std::vector<sonare::midi::MidiEvent> baked;
  baked.reserve(in.size());
  std::vector<int32_t> baked_source;
  if (source_index != nullptr) baked_source.reserve(in.size());
  sonare::midi::MidiFxBuffer chunk;
  for (size_t offset = 0; offset < in.size(); offset += inputs_per_chunk) {
    const size_t count = std::min(inputs_per_chunk, in.size() - offset);
    chain->process_chunk(in.data() + offset, count, offset, &chunk);
    if (chain->overflow_count() != 0) return false;
    baked.insert(baked.end(), chunk.events.begin(), chunk.events.begin() + chunk.size);
    if (source_index != nullptr) {
      baked_source.insert(baked_source.end(), chunk.size, static_cast<int32_t>(offset));
    }
  }

  if (source_index == nullptr) {
    sonare::midi::sort_render_events_stable(baked);
  } else {
    // Permutation sort so the provenance array follows the events through the
    // same stable ordering the plain path applies in place.
    std::vector<size_t> order(baked.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&baked](size_t a, size_t b) {
      return sonare::midi::render_event_before(baked[a], baked[b]);
    });
    std::vector<sonare::midi::MidiEvent> sorted_baked;
    sorted_baked.reserve(baked.size());
    source_index->clear();
    source_index->reserve(baked.size());
    for (const size_t index : order) {
      sorted_baked.push_back(baked[index]);
      source_index->push_back(baked_source[index]);
    }
    baked.swap(sorted_baked);
  }

  transformed->clear();
  transformed->reserve(baked.size());
  for (const sonare::midi::MidiEvent& midi_event : baked) {
    arr::MidiClipEvent event;
    event.ppq = static_cast<double>(midi_event.render_frame) /
                static_cast<double>(sonare::midi::kMidiFxPpqScale);
    event.data0 = midi_event.ump.words[0];
    event.data1 = midi_event.ump.words[1];
    event.sysex_handle = midi_event.ump.sysex_handle;
    transformed->push_back(event);
  }
  return true;
}

sonare::midi::SysExHandle remap_sysex_handle(
    const arr::MidiContentStore& midi, uint32_t source_handle,
    sonare::midi::SysExStore* export_store,
    std::map<uint32_t, sonare::midi::SysExHandle>* exported_handles) {
  if (source_handle == 0 || export_store == nullptr || exported_handles == nullptr) return 0;
  const auto existing = exported_handles->find(source_handle);
  if (existing != exported_handles->end()) return existing->second;
  const auto payload = midi.sysex_payloads.find(source_handle);
  if (payload == midi.sysex_payloads.end()) return 0;
  const sonare::midi::SysExHandle exported = export_store->add(payload->second);
  if (exported != 0) (*exported_handles)[source_handle] = exported;
  return exported;
}

void fill_ump_from_arr_event(const arr::MidiClipEvent& event, const arr::MidiContentStore& midi,
                             sonare::midi::SysExStore* export_store,
                             std::map<uint32_t, sonare::midi::SysExHandle>* exported_handles,
                             sonare::midi::Ump* out) {
  out->words[0] = event.data0;
  out->words[1] = event.data1;
  out->word_count = sonare::midi::ump_word_count_for_word0(event.data0);
  out->group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
  const sonare::midi::SysExHandle sysex =
      remap_sysex_handle(midi, event.sysex_handle, export_store, exported_handles);
  out->sysex_handle = sysex;
}

// Bound offline loop expansion independently of the audio-sample buffer cap.
// One million UMP events is already tens of MiB in the intermediate vectors and
// is large enough for practical project export while preventing a tiny loop
// length from turning one accepted event into an unbounded allocation.
inline constexpr size_t kMaxProjectMidiExportEvents = sonare::resource::kMaxProjectMidiExportEvents;
inline constexpr size_t kMaxMidiExportLoopIterations =
    sonare::resource::kMaxMidiExportLoopIterations;

SonareError make_smf_clip_from_events(
    const arr::EditClip& clip, const arr::MidiClipEventList& events,
    const arr::MidiContentStore& midi, sonare::midi::SysExStore* export_store,
    std::map<uint32_t, sonare::midi::SysExHandle>* exported_handles, size_t max_output_events,
    sonare::midi::MidiClip* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  if (!std::isfinite(clip.start_ppq) || !std::isfinite(clip.length_ppq) ||
      !std::isfinite(clip.source_offset_ppq) || clip.start_ppq < 0.0 || clip.length_ppq <= 0.0 ||
      clip.source_offset_ppq < 0.0) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (clip.loop_mode == arr::LoopMode::kLoop &&
      (!std::isfinite(clip.loop_length_ppq) || clip.loop_length_ppq < 0.0)) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  const double clip_end = clip.end_ppq();
  if (!std::isfinite(clip_end) || !(clip_end > clip.start_ppq)) {
    return SONARE_ERROR_INVALID_FORMAT;
  }

  const bool looping = clip.loop_mode == arr::LoopMode::kLoop && clip.loop_length_ppq > 0.0;
  size_t loop_iterations = 1;
  if (looping) {
    if (!sonare::numeric::checked_ceil_ratio(clip.length_ppq, clip.loop_length_ppq,
                                             kMaxMidiExportLoopIterations, &loop_iterations) ||
        loop_iterations == 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  size_t eligible_events = 0;
  for (const arr::MidiClipEvent& event : events) {
    if (!std::isfinite(event.ppq) || event.ppq < clip.source_offset_ppq) continue;
    const double rebased = event.ppq - clip.source_offset_ppq;
    if (!std::isfinite(rebased)) return SONARE_ERROR_INVALID_FORMAT;
    if ((looping && rebased < clip.loop_length_ppq) || (!looping && rebased < clip.length_ppq)) {
      ++eligible_events;
    }
  }
  size_t projected_events = 0;
  if (!sonare::numeric::checked_size_product(eligible_events, loop_iterations, max_output_events,
                                             &projected_events)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  std::vector<sonare::midi::MidiClipEvent> expanded;
  expanded.reserve(projected_events);
  for (const arr::MidiClipEvent& event : events) {
    if (!std::isfinite(event.ppq) || event.ppq < clip.source_offset_ppq) continue;
    const double rebased = event.ppq - clip.source_offset_ppq;
    if (looping) {
      if (rebased >= clip.loop_length_ppq) continue;
      double previous_loop_start = 0.0;
      double previous_event_ppq = 0.0;
      bool have_previous = false;
      for (size_t iteration = 0; iteration < loop_iterations; ++iteration) {
        const long double loop_start_wide =
            static_cast<long double>(clip.start_ppq) +
            static_cast<long double>(iteration) * static_cast<long double>(clip.loop_length_ppq);
        const double loop_start = static_cast<double>(loop_start_wide);
        if (!std::isfinite(loop_start) || (have_previous && !(loop_start > previous_loop_start))) {
          return SONARE_ERROR_INVALID_FORMAT;
        }
        if (loop_start >= clip_end) break;
        const double ppq = loop_start + rebased;
        if (!std::isfinite(ppq) || (have_previous && !(ppq > previous_event_ppq))) {
          return SONARE_ERROR_INVALID_FORMAT;
        }
        previous_loop_start = loop_start;
        previous_event_ppq = ppq;
        have_previous = true;
        if (ppq >= clip_end) continue;
        sonare::midi::MidiClipEvent out_event;
        out_event.ppq = ppq;
        fill_ump_from_arr_event(event, midi, export_store, exported_handles, &out_event.ump);
        expanded.push_back(out_event);
      }
    } else {
      if (rebased >= clip.length_ppq) continue;
      const double ppq = clip.start_ppq + rebased;
      if (!std::isfinite(ppq) || ppq < clip.start_ppq) return SONARE_ERROR_INVALID_FORMAT;
      sonare::midi::MidiClipEvent out_event;
      out_event.ppq = ppq;
      fill_ump_from_arr_event(event, midi, export_store, exported_handles, &out_event.ump);
      expanded.push_back(out_event);
    }
  }
  out->set_events(std::move(expanded));
  out->sort_stable();
  return SONARE_OK;
}

/// Builds a TempoMap from the project's tempo / time-signature segments (or a
/// 120 BPM / 4-4 default when the project carries none) for grid snapping.
void fill_project_tempo_map(const arr::Project& project, sonare::transport::TempoMap* map) {
  if (map == nullptr) return;
  map->prepare(project.sample_rate());
  std::vector<sonare::transport::TempoSegment> tempo = project.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, sonare::constants::kDefaultBpm, 0.0});
  std::vector<sonare::transport::TimeSignatureSegment> sigs = project.time_signatures();
  if (sigs.empty()) sigs.push_back({0.0, {4, 4}});
  map->set_segments(std::move(tempo));
  map->set_time_signatures(std::move(sigs));
}

// Replays an import's already-built commands against value copies of the live
// project and MIDI store, then exercises the complete persistence boundary.
// Commands are intentionally applied by reference: Add commands and SetMarker
// retain the ids allocated during this first replay, so the exact same command
// objects can be moved into the live history transaction after the preflight.
// project_from_json is the aggregate resource gate for parser nodes, array
// entities, decoded strings, and decoded sidecar/SysEx payload bytes; the
// non-throwing encoder form keeps the encoder side bounded as well, and reports
// an over-budget document as a rejected import instead of letting it surface as
// a project-state error.
bool imported_midi_persistence_preflight(const arr::Project& project,
                                         const arr::MidiContentStore& midi,
                                         const std::vector<arr::EditCommandPtr>& commands) {
  arr::Project candidate_project = project;
  arr::MidiContentStore candidate_midi = midi;
  for (const auto& command : commands) {
    if (command == nullptr || !command->apply(candidate_project, candidate_midi)) return false;
  }

  std::string serialized;
  if (!sonare::serialize::try_project_to_json(candidate_project, candidate_midi, &serialized)) {
    return false;
  }
  const sonare::serialize::DeserializeResult decoded =
      sonare::serialize::project_from_json(serialized);
  if (!decoded.ok() || !decoded.project.has_value()) return false;

  std::string reserialized;
  if (!sonare::serialize::try_project_to_json(*decoded.project, decoded.midi, &reserialized)) {
    return false;
  }
  return serialized == reserialized;
}

// Installs a normalized imported MIDI file (tempo / time-signature / markers +
// one track+clip per imported clip) into the project as a single undoable
// transaction. Shared by the SMF (MIDI 1.0) and MIDI Clip File (SMF2) importers;
// `markers` may be empty (SMF2 carries none). On failure the history is rolled
// back to its prior depth. `out_first_clip_id` (optional) receives the first
// added clip id.
SonareError install_imported_midi(
    SonareProject* project, const std::vector<sonare::transport::TempoSegment>& tempos,
    const std::vector<sonare::transport::TimeSignatureSegment>& sigs,
    const std::vector<sonare::midi::SmfMarker>& markers,
    const std::vector<sonare::midi::MidiClip>& clips, const std::vector<std::string>& clip_names,
    const std::vector<double>& clip_lengths_ppq, const sonare::midi::SysExStore& sysex_store,
    uint32_t* out_first_clip_id, const std::string& sequence_name = "") {
  const size_t rollback_depth = project->history.undo_depth();
  // The conductor-track song title (if any) becomes the name of the first track
  // that has no name of its own, so it is not lost when flattening to clips.
  bool sequence_name_used = sequence_name.empty();

  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(2 + markers.size() + clips.size() * 4);

  if (!tempos.empty()) commands.push_back(std::make_unique<arr::SetTempoSegment>(tempos));
  if (!sigs.empty()) commands.push_back(std::make_unique<arr::SetTimeSignatureSegment>(sigs));
  uint32_t next_marker_id = project->history.project().next_marker_id();
  for (const auto& marker : markers) {
    if (next_marker_id == std::numeric_limits<uint32_t>::max()) {
      return SONARE_ERROR_INVALID_STATE;
    }
    commands.push_back(std::make_unique<arr::SetMarker>(next_marker_id, marker.ppq, marker.text,
                                                        static_cast<uint8_t>(marker.kind),
                                                        marker.key_fifths, marker.key_minor));
    ++next_marker_id;
  }

  uint32_t first_clip = 0;
  arr::SourceId next_source_id = project->history.project().next_source_id();
  arr::TrackId next_track_id = project->history.project().next_track_id();
  arr::ClipId next_clip_id = project->history.project().next_clip_id();
  uint32_t next_sysex_handle = 1;
  for (const auto& [handle, payload] : project->history.midi_content().sysex_payloads) {
    (void)payload;
    if (handle == std::numeric_limits<uint32_t>::max()) return SONARE_ERROR_INVALID_STATE;
    next_sysex_handle = std::max(next_sysex_handle, handle + 1);
  }
  std::map<uint32_t, uint32_t> imported_sysex_handles;
  for (size_t clip_index = 0; clip_index < clips.size(); ++clip_index) {
    const sonare::midi::MidiClip& src = clips[clip_index];

    const arr::SourceId source_id = next_source_id++;
    const arr::TrackId track_id = next_track_id++;
    const arr::ClipId clip_id = next_clip_id++;

    arr::MidiSourceRef source_ref;
    auto attach = std::make_unique<arr::AttachMidiSource>(source_ref);
    attach->reseed_id(source_id);
    commands.push_back(std::move(attach));

    arr::Track track;
    track.kind = arr::Track::Kind::kMidi;
    if (clip_index < clip_names.size() && !clip_names[clip_index].empty()) {
      track.name = clip_names[clip_index];
    } else if (!sequence_name_used) {
      track.name = sequence_name;
      sequence_name_used = true;
    } else {
      track.name = "midi";
    }
    auto add_track = std::make_unique<arr::AddTrack>(std::move(track));
    add_track->reseed_id(track_id);
    commands.push_back(std::move(add_track));

    double length_ppq = clip_index < clip_lengths_ppq.size() ? clip_lengths_ppq[clip_index] : 0.0;
    double max_event_ppq = 0.0;
    bool has_events = false;
    for (const auto& event : src.events()) {
      has_events = true;
      if (event.ppq > max_event_ppq) max_event_ppq = event.ppq;
    }
    // The edit compiler keeps clip events on the half-open window [0, length_ppq)
    // and drops anything at or past the end tick. A standard SMF places the final
    // note-off exactly on the EndOfTrack tick, which equals the imported clip
    // length, so without this nudge that closing note-off would be discarded and
    // the note left hanging when the clip is bounced through an instrument. Extend
    // the clip just past the last event so a boundary event survives; nextafter
    // keeps the timing numerically indistinguishable from the original tick.
    // Normalize an empty/tick-0 clip before boundary handling. Otherwise an
    // EOT at tick 0 would be nudged to the smallest subnormal and serialize as
    // an effectively zero-length clip. Positive boundary endpoints retain the
    // closure nudge, directed explicitly toward +infinity.
    if (length_ppq <= 0.0) length_ppq = 1.0;
    if (has_events && max_event_ppq > 0.0 && max_event_ppq >= length_ppq) {
      length_ppq = std::nextafter(max_event_ppq, std::numeric_limits<double>::infinity());
    }

    arr::EditClip clip;
    clip.track_id = track_id;
    clip.source_id = source_id;
    clip.start_ppq = 0.0;
    clip.length_ppq = length_ppq;
    clip.gain = 1.0f;
    auto add_clip = std::make_unique<arr::AddClip>(clip);
    add_clip->reseed_id(clip_id);
    commands.push_back(std::move(add_clip));
    if (first_clip == 0) first_clip = clip_id;

    arr::MidiClipEventList list;
    std::map<uint32_t, std::vector<uint8_t>> clip_sysex_payloads;
    list.reserve(src.events().size());
    for (const auto& event : src.events()) {
      arr::MidiClipEvent out_event;
      out_event.ppq = event.ppq;
      out_event.data0 = event.ump.words[0];
      out_event.data1 = event.ump.words[1];
      if (event.ump.sysex_handle != 0) {
        if (next_sysex_handle == std::numeric_limits<uint32_t>::max() &&
            imported_sysex_handles.find(event.ump.sysex_handle) == imported_sysex_handles.end()) {
          return SONARE_ERROR_INVALID_STATE;
        }
        auto [it, inserted] =
            imported_sysex_handles.emplace(event.ump.sysex_handle, next_sysex_handle);
        if (inserted) ++next_sysex_handle;
        out_event.sysex_handle = it->second;
        const std::vector<uint8_t>* payload = sysex_store.lookup(event.ump.sysex_handle);
        if (payload != nullptr) {
          clip_sysex_payloads[out_event.sysex_handle] = *payload;
        }
      }
      list.push_back(out_event);
    }
    commands.push_back(std::make_unique<arr::ReplaceMidiClipEvents>(
        clip_id, std::move(list), std::move(clip_sysex_payloads)));
  }
  if (!imported_midi_persistence_preflight(project->history.project(),
                                           project->history.midi_content(), commands)) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (!project->history.apply_transaction(std::move(commands))) {
    rollback_to_depth(project, rollback_depth);
    return SONARE_ERROR_INVALID_STATE;
  }
  if (out_first_clip_id) *out_first_clip_id = first_clip;
  return SONARE_OK;
}

}  // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif  // SONARE_WITH_ARRANGEMENT
