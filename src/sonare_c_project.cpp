/// @file sonare_c_project.cpp
/// @brief Implementation of the curated headless-project C ABI.
///
/// The opaque SonareProject wraps an arrangement::EditHistory (owning the
/// Project + MidiContentStore) plus an arrangement::AudioContentStore. ALL
/// model mutation routes through EditHistory commands so undo / redo / replay
/// stay uniform; the Project is never mutated directly.

#include "sonare_c_project.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sonare_c.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "analysis/beat_analyzer.h"
#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "core/audio.h"
#include "engine/realtime_engine.h"
#include "midi/midi_fx.h"
#include "midi/smf.h"
#include "midi/ump.h"
#include "mir/grid_snap.h"
#include "mir/tempo_estimator_bridge.h"
#include "rt/command.h"
#include "serialize/project_serializer.h"
#include "transport/tempo_map.h"
#include "util/json.h"
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
};

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
  if (!desc->audio_interleaved && desc->audio_frames == 0 && desc->audio_channels == 0 &&
      desc->audio_sample_rate == 0) {
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

bool valid_midi_event_pod(const SonareMidiEventPod& event) noexcept {
  if (!std::isfinite(event.ppq) || event.ppq < 0.0) return false;
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

uint8_t ump_word_count_from_word0(uint32_t word0) noexcept {
  const uint8_t mt = static_cast<uint8_t>((word0 >> 28u) & 0x0Fu);
  return mt == static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi2ChannelVoice) ? 2 : 1;
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
  ump.word_count = ump_word_count_from_word0(event.data0);
  ump.group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
  if (ump.group != group || ump.channel() != channel) return false;
  if (ump.message_type() != sonare::midi::UmpMessageType::kMidi1ChannelVoice) return false;
  const uint8_t status = ump.status_nibble();
  if (status == static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange)) return true;
  if (status != static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange)) return false;
  const uint8_t cc = ump.note_number();
  return cc == 0 || cc == 32;
}

double json_number_or(const json::Value& obj, const char* key, double fallback) {
  const json::Value* value = obj.find(key);
  return value != nullptr && value->is_number() ? value->as_number() : fallback;
}

bool json_has_number(const json::Value& obj, const char* key) {
  const json::Value* value = obj.find(key);
  return value != nullptr && value->is_number();
}

int json_int_or(const json::Value& obj, const char* key, int fallback) {
  const double value = json_number_or(obj, key, static_cast<double>(fallback));
  if (!std::isfinite(value)) return fallback;
  return static_cast<int>(std::lround(value));
}

SonareError midi_fx_chain_from_json(const char* config_json, sonare::midi::MidiFxChain* chain) {
  if (config_json == nullptr || chain == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  json::Value root;
  try {
    root = json::parse_strict(config_json);
  } catch (const json::JsonError&) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (!root.is_object()) return SONARE_ERROR_INVALID_PARAMETER;

  sonare::midi::TransposeConfig transpose;
  if (json_has_number(root, "transpose_semitones")) {
    transpose.enabled = true;
    transpose.semitones = json_int_or(root, "transpose_semitones", 0);
    chain->set_transpose(transpose);
  }

  sonare::midi::VelocityCurveConfig velocity;
  const bool has_velocity = json_has_number(root, "velocity_scale") ||
                            json_has_number(root, "velocity_offset") ||
                            json_has_number(root, "velocity_gamma");
  if (has_velocity) {
    velocity.enabled = true;
    velocity.scale = static_cast<float>(json_number_or(root, "velocity_scale", 1.0));
    velocity.offset = static_cast<float>(json_number_or(root, "velocity_offset", 0.0));
    velocity.gamma = static_cast<float>(json_number_or(root, "velocity_gamma", 1.0));
    if (!std::isfinite(velocity.scale) || !std::isfinite(velocity.offset) ||
        !std::isfinite(velocity.gamma) || velocity.gamma <= 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    chain->set_velocity_curve(velocity);
  }

  if (json_has_number(root, "quantize_ppq")) {
    const double grid_ppq = json_number_or(root, "quantize_ppq", 0.0);
    const double strength = json_number_or(root, "quantize_strength", 1.0);
    if (!finite_positive(grid_ppq) || !std::isfinite(strength) || strength < 0.0 ||
        strength > 1.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    constexpr int64_t kPpqFxScale = 960000;
    quantize.grid_frames =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(grid_ppq * kPpqFxScale)));
    quantize.strength = static_cast<float>(strength);
    chain->set_quantize(quantize);
  }

  if (const json::Value* intervals = root.find("chord_intervals")) {
    if (!intervals->is_array()) return SONARE_ERROR_INVALID_PARAMETER;
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    const auto& values = intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ChordConfig::kMaxChordNotes) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    chord.count = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number()) return SONARE_ERROR_INVALID_PARAMETER;
      chord.intervals[i] = static_cast<int>(std::lround(values[i].as_number()));
    }
    chain->set_chord(chord);
  }

  const bool has_humanize = json_has_number(root, "humanize_ppq") ||
                            json_has_number(root, "humanize_velocity") ||
                            json_has_number(root, "seed");
  if (has_humanize) {
    const double timing_ppq = json_number_or(root, "humanize_ppq", 0.0);
    const int velocity_amount = json_int_or(root, "humanize_velocity", 0);
    const int seed = json_int_or(root, "seed", 0);
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || velocity_amount < 0 ||
        velocity_amount > 127 || seed < 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::HumanizeConfig humanize;
    humanize.enabled = true;
    humanize.seed = static_cast<uint32_t>(seed);
    constexpr int64_t kPpqFxScale = 960000;
    humanize.timing_frames = static_cast<int64_t>(std::llround(timing_ppq * kPpqFxScale));
    humanize.velocity_amount = velocity_amount;
    chain->set_humanize(humanize);
  }

  chain->prepare();
  return SONARE_OK;
}

arr::MidiClipEventList apply_midi_fx_to_events(const arr::MidiClipEventList& events,
                                               sonare::midi::MidiFxChain* chain) {
  constexpr int64_t kPpqFxScale = 960000;
  std::vector<sonare::midi::MidiEvent> in;
  in.reserve(events.size());
  for (const arr::MidiClipEvent& event : events) {
    sonare::midi::MidiEvent midi_event;
    midi_event.render_frame = static_cast<int64_t>(std::llround(event.ppq * kPpqFxScale));
    midi_event.ump.words[0] = event.data0;
    midi_event.ump.words[1] = event.data1;
    midi_event.ump.word_count = ump_word_count_from_word0(event.data0);
    midi_event.ump.group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
    midi_event.ump.sysex_handle = event.sysex_handle;
    in.push_back(midi_event);
  }
  sonare::midi::MidiFxBuffer out;
  chain->process(in.data(), in.size(), &out);

  arr::MidiClipEventList transformed;
  transformed.reserve(out.size);
  for (size_t i = 0; i < out.size; ++i) {
    arr::MidiClipEvent event;
    event.ppq = static_cast<double>(out.events[i].render_frame) / static_cast<double>(kPpqFxScale);
    event.data0 = out.events[i].ump.words[0];
    event.data1 = out.events[i].ump.words[1];
    event.sysex_handle = out.events[i].ump.sysex_handle;
    transformed.push_back(event);
  }
  std::stable_sort(transformed.begin(), transformed.end(),
                   [](const arr::MidiClipEvent& a, const arr::MidiClipEvent& b) {
                     if (a.ppq != b.ppq) return a.ppq < b.ppq;
                     return a.data0 < b.data0 || (a.data0 == b.data0 && a.data1 < b.data1);
                   });
  return transformed;
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
  out->word_count = ump_word_count_from_word0(event.data0);
  out->group = static_cast<uint8_t>((event.data0 >> 24u) & 0x0Fu);
  const sonare::midi::SysExHandle sysex =
      remap_sysex_handle(midi, event.sysex_handle, export_store, exported_handles);
  out->sysex_handle = sysex;
}

sonare::midi::MidiClip make_smf_clip_from_events(
    const arr::EditClip& clip, const arr::MidiClipEventList& events,
    const arr::MidiContentStore& midi, sonare::midi::SysExStore* export_store,
    std::map<uint32_t, sonare::midi::SysExHandle>* exported_handles) {
  sonare::midi::MidiClip out;
  const double clip_end = clip.end_ppq();
  for (const arr::MidiClipEvent& event : events) {
    if (!std::isfinite(event.ppq) || event.ppq < clip.source_offset_ppq) continue;
    const double rebased = event.ppq - clip.source_offset_ppq;
    if (clip.loop_mode == arr::LoopMode::kLoop && clip.loop_length_ppq > 0.0) {
      if (rebased >= clip.loop_length_ppq) continue;
      for (double loop_start = clip.start_ppq; loop_start < clip_end;
           loop_start += clip.loop_length_ppq) {
        const double ppq = loop_start + rebased;
        if (ppq >= clip_end) continue;
        sonare::midi::MidiClipEvent out_event;
        out_event.ppq = ppq;
        fill_ump_from_arr_event(event, midi, export_store, exported_handles, &out_event.ump);
        out.add_event(out_event);
      }
    } else {
      if (rebased >= clip.length_ppq) continue;
      sonare::midi::MidiClipEvent out_event;
      out_event.ppq = clip.start_ppq + rebased;
      fill_ump_from_arr_event(event, midi, export_store, exported_handles, &out_event.ump);
      out.add_event(out_event);
    }
  }
  out.sort_stable();
  return out;
}

/// Builds a TempoMap from the project's tempo / time-signature segments (or a
/// 120 BPM / 4-4 default when the project carries none) for grid snapping.
void fill_project_tempo_map(const arr::Project& project, sonare::transport::TempoMap* map) {
  if (map == nullptr) return;
  map->prepare(project.sample_rate());
  std::vector<sonare::transport::TempoSegment> tempo = project.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, 120.0, 0.0});
  std::vector<sonare::transport::TimeSignatureSegment> sigs = project.time_signatures();
  if (sigs.empty()) sigs.push_back({0.0, {4, 4}});
  map->set_segments(std::move(tempo));
  map->set_time_signatures(std::move(sigs));
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// ABI version
// ============================================================================

uint32_t sonare_project_abi_version(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_PROJECT_ABI_VERSION;
#else
  return 0u;
#endif
}

// ============================================================================
// Lifecycle / IO / render
// ============================================================================

SonareError sonare_project_create(SonareProject** out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  SONARE_C_TRY
  *out = new SonareProject{};
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(out);
#endif
}

void sonare_project_destroy(SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  delete project;
#else
  (void)project;
#endif
}

SonareError sonare_project_serialize(const SonareProject* project, char** out_json,
                                     size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_TRY
  const std::string json = sonare::serialize::project_to_json(project->history.project(),
                                                              project->history.midi_content());
  *out_json = copy_string(json);
  if (out_len) *out_len = json.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_json, out_len);
#endif
}

SonareError sonare_project_deserialize(const char* json, size_t len, SonareProject** out,
                                       char** out_diag) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!json || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (out_diag) *out_diag = nullptr;
  SONARE_C_TRY
  sonare::serialize::DeserializeResult result =
      sonare::serialize::project_from_json(std::string(json, len));
  if (!result.ok()) {
    if (out_diag) {
      std::ostringstream stream;
      for (size_t i = 0; i < result.diagnostics.size(); ++i) {
        if (i > 0) stream << '\n';
        stream << result.diagnostics[i].code << ": " << result.diagnostics[i].message;
      }
      *out_diag = copy_string(stream.str());
    }
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto handle = std::make_unique<SonareProject>();
  handle->history = arr::EditHistory(std::move(*result.project));
  handle->history.midi_content() = std::move(result.midi);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(json, len, out, out_diag);
#endif
}

SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Global property (not part of the command-driven invertible state); set
  // directly on the owned Project.
  project->history.project().set_sample_rate(sample_rate);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, sample_rate);
#endif
}

SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::CompileResult result =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio);
  out->has_timeline = result.timeline.has_value() ? 1 : 0;
  out->diagnostic_count = result.diagnostics.size();
  if (!result.diagnostics.empty()) {
    out->diagnostics = new SonareProjectDiagnostic[result.diagnostics.size()];
    std::ostringstream stream;
    for (size_t i = 0; i < result.diagnostics.size(); ++i) {
      const arr::Diagnostic& d = result.diagnostics[i];
      out->diagnostics[i].code = static_cast<uint32_t>(d.code);
      out->diagnostics[i].severity = static_cast<uint32_t>(d.severity);
      out->diagnostics[i].target_id = d.target_id;
      if (i > 0) stream << '\n';
      stream << d.message;
    }
    out->messages = copy_string(stream.str());
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out);
#endif
}

void sonare_project_free_compile_result(SonareProjectCompileResult* result) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!result) return;
  delete[] result->diagnostics;
  delete[] result->messages;
  *result = {};
#else
  (void)result;
#endif
}

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;

  SonareProjectBounceOptions opts{};
  if (options) opts = *options;
  if (opts.total_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  const int block_size = opts.block_size > 0 ? opts.block_size : 128;
  const int num_channels = opts.num_channels > 0 ? opts.num_channels : 2;
  if (block_size <= 0 || num_channels <= 0 ||
      static_cast<uint64_t>(opts.total_frames) >
          std::numeric_limits<size_t>::max() / static_cast<uint64_t>(num_channels) ||
      static_cast<uint64_t>(opts.total_frames) * static_cast<uint64_t>(num_channels) >
          kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const double project_sr = project->history.project().sample_rate();
  const double sample_rate =
      opts.sample_rate > 0 ? static_cast<double>(opts.sample_rate) : project_sr;
  if (!finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.instrument_latency_samples < 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  arr::CompileConfig config;
  config.instrument_latency_samples = opts.instrument_latency_samples;
  arr::CompileResult compiled = arr::compile(
      project->history.project(), project->history.midi_content(), project->audio, config);
  if (!compiled.timeline.has_value()) return SONARE_ERROR_INVALID_STATE;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(*compiled.timeline, engine);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  const int64_t frames = opts.total_frames;
  std::vector<std::vector<float>> channels(static_cast<size_t>(num_channels),
                                           std::vector<float>(static_cast<size_t>(frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels.size());
  for (auto& channel : channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, frames, block_size);

  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(num_channels);
  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t frame = 0; frame < frames; ++frame) {
    for (int ch = 0; ch < num_channels; ++ch) {
      interleaved[static_cast<size_t>(frame) * num_channels + ch] =
          channels[static_cast<size_t>(ch)][static_cast<size_t>(frame)];
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, out_interleaved, out_len);
#endif
}

// ============================================================================
// Edit
// ============================================================================

SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_track_id) *out_track_id = 0;
  if (!project || !desc || !out_track_id) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->kind < SONARE_TRACK_AUDIO || desc->kind > SONARE_TRACK_AUX) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::Track track;
  track.kind = static_cast<arr::Track::Kind>(desc->kind);
  if (desc->name) track.name = desc->name;
  auto command = std::make_unique<arr::AddTrack>(std::move(track));
  arr::AddTrack* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  *out_track_id = raw->allocated_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_track_id);
#endif
}

SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !desc || !out_clip_id) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->track_id == 0 || !finite_positive(desc->length_ppq) ||
      !finite_non_negative(desc->start_ppq) || !finite_non_negative(desc->source_offset_ppq) ||
      !std::isfinite(desc->gain) || desc->gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_track(desc->track_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const SonareError audio_err =
      desc->is_midi == 0 ? validate_audio_clip_payload(desc, nullptr) : SONARE_OK;
  if (audio_err != SONARE_OK) return audio_err;
  SONARE_C_TRY
  const size_t rollback_depth = project->history.undo_depth();
  // Register the clip source (audio or MIDI) through a command, then add the
  // clip. Both mutations go through EditHistory so they participate in undo.
  arr::SourceId source_id = 0;
  if (desc->is_midi != 0) {
    arr::MidiSourceRef ref;
    auto attach = std::make_unique<arr::AttachMidiSource>(ref);
    arr::AttachMidiSource* raw = attach.get();
    if (!project->history.apply(std::move(attach))) return SONARE_ERROR_INVALID_STATE;
    source_id = raw->allocated_id();
  } else {
    arr::AudioSourceRef ref;
    if (desc->source_uri) ref.uri = desc->source_uri;
    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      ref.channel_count = static_cast<uint32_t>(desc->audio_channels);
      ref.sample_rate_hint = static_cast<double>(desc->audio_sample_rate);
    }
    auto attach = std::make_unique<arr::AttachAudioSource>(ref);
    arr::AttachAudioSource* raw = attach.get();
    if (!project->history.apply(std::move(attach))) return SONARE_ERROR_INVALID_STATE;
    source_id = raw->allocated_id();

    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      arr::AudioSourceSamples samples;
      samples.sample_rate = static_cast<double>(desc->audio_sample_rate);
      samples.channels =
          deinterleave(desc->audio_interleaved, desc->audio_frames, desc->audio_channels);
      project->audio.sources[source_id] = std::move(samples);
    }
  }

  arr::EditClip clip;
  clip.track_id = desc->track_id;
  clip.source_id = source_id;
  clip.start_ppq = desc->start_ppq;
  clip.length_ppq = desc->length_ppq;
  clip.source_offset_ppq = desc->source_offset_ppq;
  clip.gain = desc->gain > 0.0f ? desc->gain : 1.0f;
  auto command = std::make_unique<arr::AddClip>(clip);
  arr::AddClip* raw = command.get();
  if (!project->history.apply(std::move(command)) || raw->allocated_id() == 0) {
    project->audio.sources.erase(source_id);
    rollback_to_depth(project, rollback_depth);
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = raw->allocated_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_clip_id);
#endif
}

SonareError sonare_project_add_midi_clip(SonareProject* project, double start_ppq,
                                         double length_ppq, uint32_t* out_track_id,
                                         uint32_t* out_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_track_id) *out_track_id = 0;
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !out_track_id || !out_clip_id || !finite_non_negative(start_ppq) ||
      !finite_positive(length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t rollback_depth = project->history.undo_depth();

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_MIDI;
  track_desc.name = "midi";
  SonareError err = sonare_project_add_track(project, &track_desc, out_track_id);
  if (err != SONARE_OK) {
    rollback_to_depth(project, rollback_depth);
    return err;
  }

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = *out_track_id;
  clip_desc.is_midi = 1;
  clip_desc.start_ppq = start_ppq;
  clip_desc.length_ppq = length_ppq;
  err = sonare_project_add_clip(project, &clip_desc, out_clip_id);
  if (err != SONARE_OK) {
    rollback_to_depth(project, rollback_depth);
  }
  return err;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, start_ppq, length_ppq, out_track_id, out_clip_id);
#endif
}

SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project || clip_id == 0 || !std::isfinite(split_ppq)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SplitClip>(clip_id, split_ppq);
  arr::SplitClip* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_new_clip_id) *out_new_clip_id = raw->new_clip_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, split_ppq, out_new_clip_id);
#endif
}

SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq) ||
      !finite_positive(new_length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::TrimClip>(clip_id, new_start_ppq, new_length_ppq);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_length_ppq);
#endif
}

SonareError sonare_project_move_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     uint32_t new_track_id) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::MoveClip>(clip_id, new_start_ppq, new_track_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_track_id);
#endif
}

SonareError sonare_project_undo(SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return project->history.undo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

SonareError sonare_project_redo(SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return project->history.redo() ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

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
  const size_t rollback_depth = project->history.undo_depth();
  sonare::midi::SmfImportResult result = sonare::midi::import_smf(bytes, len);
  if (!result.ok()) return SONARE_ERROR_INVALID_FORMAT;

  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(2 + result.markers.size() + result.clips.size() * 4);

  // Install the imported tempo / time-signature map and markers (undoable commands).
  if (!result.tempo_segments.empty()) {
    commands.push_back(std::make_unique<arr::SetTempoSegment>(result.tempo_segments));
  }
  if (!result.time_signatures.empty()) {
    commands.push_back(std::make_unique<arr::SetTimeSignatureSegment>(result.time_signatures));
  }
  for (const auto& marker : result.markers) {
    commands.push_back(std::make_unique<arr::SetMarker>(0, marker.ppq, marker.text));
  }

  // Convert ticks-per-quarter to PPQ (quarter notes). When the SMF used a
  // non-480 division the per-event PPQ already comes from the importer in
  // quarter-note units, so the clip length is the largest event position.
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
  for (size_t clip_index = 0; clip_index < result.clips.size(); ++clip_index) {
    const sonare::midi::MidiClip& src = result.clips[clip_index];

    const arr::SourceId source_id = next_source_id++;
    const arr::TrackId track_id = next_track_id++;
    const arr::ClipId clip_id = next_clip_id++;

    arr::MidiSourceRef source_ref;
    auto attach = std::make_unique<arr::AttachMidiSource>(source_ref);
    attach->reseed_id(source_id);
    commands.push_back(std::move(attach));

    arr::Track track;
    track.kind = arr::Track::Kind::kMidi;
    track.name = clip_index < result.clip_names.size() && !result.clip_names[clip_index].empty()
                     ? result.clip_names[clip_index]
                     : "midi";
    auto add_track = std::make_unique<arr::AddTrack>(std::move(track));
    add_track->reseed_id(track_id);
    commands.push_back(std::move(add_track));

    double max_ppq = 0.0;
    for (const auto& event : src.events()) {
      if (event.ppq > max_ppq) max_ppq = event.ppq;
    }
    const double length_ppq = max_ppq > 0.0 ? max_ppq : 1.0;

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

    // Bridge the UMP events to the arrangement event POD: data0 = word[0],
    // data1 = word[1].
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
        const std::vector<uint8_t>* payload = result.sysex_store.lookup(event.ump.sysex_handle);
        if (payload != nullptr) {
          clip_sysex_payloads[out_event.sysex_handle] = *payload;
        }
      }
      list.push_back(out_event);
    }
    commands.push_back(std::make_unique<arr::ReplaceMidiClipEvents>(
        clip_id, std::move(list), std::move(clip_sysex_payloads)));
  }
  if (!project->history.apply_transaction(std::move(commands))) {
    rollback_to_depth(project, rollback_depth);
    return SONARE_ERROR_INVALID_STATE;
  }
  if (out_first_clip_id) *out_first_clip_id = first_clip;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, bytes, len, out_first_clip_id);
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

// ============================================================================
// MIR
// ============================================================================

SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, float* out_bpm) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bpm) *out_bpm = 0.0f;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  sonare::Audio wrapped = sonare::Audio::from_buffer(audio, len, sample_rate);
  sonare::BeatAnalyzer analyzer(wrapped);
  sonare::mir::BeatAnalysisInput input = sonare::mir::make_input_from_analyzer(analyzer);
  std::vector<sonare::mir::TempoEstimate> estimates = sonare::mir::estimate_tempo(input);
  if (estimates.empty() || estimates.front().segments.empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  const sonare::mir::TempoEstimate& primary = estimates.front();
  auto command = std::make_unique<arr::SetTempoSegment>(primary.segments);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_bpm) *out_bpm = static_cast<float>(primary.segments.front().bpm);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, out_bpm);
#endif
}

SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        double* out_ppq) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_ppq) *out_ppq = ppq;
  if (!project || !out_ppq || !finite_non_negative(ppq) || !std::isfinite(strength) ||
      strength < 0.0 || strength > 1.0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const sonare::mir::SnapGrid grid = sonare::mir::make_grid(map, ppq);
  *out_ppq = sonare::mir::snap_to_beat(grid, ppq, strength);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, ppq, strength, out_ppq);
#endif
}

// ============================================================================
// Memory management
// ============================================================================

void sonare_free_bytes(uint8_t* ptr) { delete[] ptr; }
