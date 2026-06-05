#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "automation/parameter.h"
#include "core/audio.h"
#include "core/resample.h"
#include "engine/realtime_engine.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/midi_fx_json.h"
#include "c_api/synth_patch_common.h"
#include "midi/builtin_synth.h"
#include "midi/midi_fx.h"
#include "midi/synth/sf2_player.h"
#include "util/json.h"
#endif
#if defined(SONARE_WITH_MASTERING)
#include "mastering/final/dither.h"
#endif
#include "rt/command.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_GRAPH)
#include "graph/connection.h"
#include "graph/graph.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
namespace json = sonare::util::json;
#endif

#if defined(SONARE_WITH_GRAPH)
namespace {

std::unique_ptr<rt::ProcessorBase> make_graph_processor(const SonareEngineGraphNode& node) {
  switch (node.type) {
    case 0:
      return std::make_unique<rt::PassProcessor>();
    case 1:
      return std::make_unique<rt::GainProcessor>(node.gain_db);
    default:
      return nullptr;
  }
}

}  // namespace
#endif

namespace {

// Pin the automation curve ordinal mapping used by
// sonare_engine_set_automation_lane (via SonareAutomationPoint.curve_to_next).
// As of the AutomationCurve unification this scheme matches the sample-accurate
// mixer API (sonare_strip_schedule_*_automation), so a single canonical
// ordinal set covers both subsystems and all four bindings.
static_assert(static_cast<int>(automation::CurveType::Linear) == 0,
              "automation::CurveType::Linear must be ordinal 0 to keep "
              "SonareAutomationPoint.curve_to_next ABI stable");
static_assert(static_cast<int>(automation::CurveType::Exponential) == 1,
              "automation::CurveType::Exponential must be ordinal 1");
static_assert(static_cast<int>(automation::CurveType::Hold) == 2,
              "automation::CurveType::Hold must be ordinal 2");
static_assert(static_cast<int>(automation::CurveType::SCurve) == 3,
              "automation::CurveType::SCurve must be ordinal 3");

automation::CurveType curve_from_int(int curve) {
  switch (curve) {
    case 1:
      return automation::CurveType::Exponential;
    case 2:
      return automation::CurveType::Hold;
    case 3:
      return automation::CurveType::SCurve;
    case 0:
    default:
      return automation::CurveType::Linear;
  }
}

int curve_to_int(automation::CurveType curve) { return static_cast<int>(curve); }

void copy_text(char* dest, size_t capacity, const char* src) {
  if (!dest || capacity == 0) return;
  const char* text = src ? src : "";
  std::strncpy(dest, text, capacity - 1);
  dest[capacity - 1] = '\0';
}

std::string fixed_text(const char* src, size_t capacity) {
  const char* end = std::find(src, src + capacity, '\0');
  return std::string(src, end);
}

std::vector<float> interleave_channels(const std::vector<std::vector<float>>& channels) {
  if (channels.empty()) return {};
  const size_t frames = channels[0].size();
  std::vector<float> interleaved(frames * channels.size(), 0.0f);
  for (size_t frame = 0; frame < frames; ++frame) {
    for (size_t ch = 0; ch < channels.size(); ++ch) {
      interleaved[frame * channels.size() + ch] = channels[ch][frame];
    }
  }
  return interleaved;
}

std::vector<std::vector<float>> resample_channels(const std::vector<std::vector<float>>& channels,
                                                  int source_rate, int target_rate) {
  if (source_rate == target_rate) return channels;
  std::vector<std::vector<float>> out;
  out.reserve(channels.size());
  for (const auto& channel : channels) {
    out.push_back(resample(channel.data(), channel.size(), source_rate, target_rate));
  }
  return out;
}

#if defined(SONARE_WITH_MASTERING)
mastering::final::DitherType dither_type_from_int(int value) {
  switch (value) {
    case 1:
      return mastering::final::DitherType::Rpdf;
    case 2:
      return mastering::final::DitherType::Tpdf;
    case 3:
      return mastering::final::DitherType::NoiseShaped;
    case 0:
    default:
      return mastering::final::DitherType::None;
  }
}
#endif

void fill_c_parameter(const automation::ParameterInfo& info, SonareParameterInfo* out) {
  out->id = info.id;
  copy_text(out->name, sizeof(out->name), info.name);
  copy_text(out->unit, sizeof(out->unit), info.unit);
  out->min_value = info.min_value;
  out->max_value = info.max_value;
  out->default_value = info.default_value;
  out->rt_safe = info.rt_safe ? 1 : 0;
  out->default_curve = curve_to_int(info.default_curve);
}

std::vector<automation::ParameterInfo> parameter_metadata_snapshot(
    const automation::ParameterRegistry& registry) {
  std::vector<automation::ParameterInfo> parameters;
  parameters.reserve(registry.parameter_count());
  for (size_t i = 0; i < registry.parameter_count(); ++i) {
    automation::ParameterInfo info{};
    if (registry.parameter_info_by_index(i, &info)) {
      parameters.push_back(info);
    }
  }
  return parameters;
}

void publish_parameter_metadata(SonareRealtimeEngine* engine) {
  engine->engine.automation().set_parameter_metadata(
      parameter_metadata_snapshot(engine->parameters));
}

bool registered_parameter_rejects_realtime(const SonareRealtimeEngine* engine, uint32_t param_id) {
  automation::ParameterInfo info{};
  return engine->parameters.parameter_info(param_id, &info) && !info.rt_safe;
}

void fill_c_marker(const transport::Marker& marker, SonareEngineMarker* out) {
  out->id = marker.id;
  out->ppq = marker.ppq;
  copy_text(out->name, sizeof(out->name), marker.name);
}

SonareEngineMetronomeConfig metronome_to_c(const engine::MetronomeConfig& config) {
  return {config.enabled ? 1 : 0, config.beat_gain, config.accent_gain, config.click_samples,
          config.click_seconds};
}

engine::MetronomeConfig metronome_from_c(const SonareEngineMetronomeConfig& config) {
  engine::MetronomeConfig out;
  out.enabled = config.enabled != 0;
  out.beat_gain = config.beat_gain;
  out.accent_gain = config.accent_gain;
  // When click_samples is 0 the engine derives the length from click_seconds and
  // the prepared sample rate; a positive click_samples overrides that. Treat a
  // non-positive click_seconds as "use the default" so older callers that leave
  // the field zero-initialized keep the 2 ms behavior.
  out.click_samples = config.click_samples;
  if (config.click_seconds > 0.0) {
    out.click_seconds = config.click_seconds;
  }
  return out;
}

#if defined(SONARE_WITH_ARRANGEMENT)
sonare::midi::BuiltinSynthConfig engine_synth_config_from_c(
    const SonareEngineBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

// Binds (or replaces) an engine-owned instrument on a destination, keeping the
// ownership table and the engine's instrument rack in sync. Shared by the
// built-in synth and SF2 instrument entries.
SonareError bind_engine_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                   std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : engine->builtin_instruments) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine->engine.set_midi_instrument(destination_id, raw)) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      entry.second = std::move(instrument);
      return SONARE_OK;
    }
  }
  engine->builtin_instruments.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = engine->builtin_instruments.back().second.get();
  if (!engine->engine.set_midi_instrument(destination_id, raw)) {
    engine->builtin_instruments.pop_back();
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

uint64_t pack_midi_note(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
         (static_cast<uint64_t>(channel) << 16) | (static_cast<uint64_t>(group) << 24);
}

bool valid_midi_note_args(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return group <= 15 && channel <= 15 && note <= 127 && velocity <= 127;
}

#endif

}  // namespace

SonareError sonare_engine_create(SonareRealtimeEngine** out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out = new SonareRealtimeEngine{};
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_engine_destroy(SonareRealtimeEngine* engine) { delete engine; }

SonareError sonare_engine_prepare(SonareRealtimeEngine* engine, double sample_rate,
                                  int max_block_size, size_t command_capacity,
                                  size_t telemetry_capacity) {
  if (!engine || sample_rate <= 0.0 || max_block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_play(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_stop(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_sample(SonareRealtimeEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_ppq(SonareRealtimeEngine* engine, double ppq, int64_t render_frame) {
  if (!engine || !std::isfinite(ppq) || ppq < 0.0) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  command.arg.d = ppq;  // full double precision; engine applies without truncation
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm) {
  if (!engine || !std::isfinite(bpm) || bpm <= 0.0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_tempo(bpm);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator) {
  if (!engine || numerator <= 0 || denominator <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_time_signature(numerator, denominator);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled) {
  if (!engine || !std::isfinite(start_ppq) || !std::isfinite(end_ppq) || start_ppq < 0.0 ||
      end_ppq < 0.0 || (enabled && end_ppq <= start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_loop(start_ppq, end_ppq, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info) {
  if (!engine || !info || info->max_value < info->min_value) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  // Stage the name/unit strings in the deque. std::deque keeps pointers to its
  // existing elements valid across push_back/pop_back at the ends, so the raw
  // const char* the registry holds for previously-added parameters are not
  // invalidated, and we can pop_back our two stagings if the registry rejects
  // the entry (duplicate id) — avoiding the previous unbounded leak on re-add.
  engine->parameter_strings.push_back(fixed_text(info->name, sizeof(info->name)));
  const char* name = engine->parameter_strings.back().c_str();
  engine->parameter_strings.push_back(fixed_text(info->unit, sizeof(info->unit)));
  const char* unit = engine->parameter_strings.back().c_str();
  const bool added = engine->parameters.add({info->id, name, unit, info->min_value, info->max_value,
                                             info->default_value, info->rt_safe != 0,
                                             curve_from_int(info->default_curve)});
  if (!added) {
    // Reclaim the two strings we just staged; the registry kept no reference to
    // them. The order matters: pop the most recent (unit) first.
    engine->parameter_strings.pop_back();
    engine->parameter_strings.pop_back();
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  publish_parameter_metadata(engine);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clear_parameters(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  // Clear the registry first (it holds raw pointers into parameter_strings),
  // then release the backing strings so no dangling pointer ever exists.
  engine->parameters.clear();
  engine->parameter_strings.clear();
  publish_parameter_metadata(engine);
  return SONARE_OK;
}

SonareError sonare_engine_parameter_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->parameters.parameter_count();
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info_by_index(SonareRealtimeEngine* engine, size_t index,
                                                  SonareParameterInfo* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  automation::ParameterInfo info{};
  if (!engine->parameters.parameter_info_by_index(index, &info)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_parameter(info, out);
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info(SonareRealtimeEngine* engine, uint32_t id,
                                         SonareParameterInfo* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  automation::ParameterInfo info{};
  if (!engine->parameters.parameter_info(id, &info)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_parameter(info, out);
  return SONARE_OK;
}

SonareError sonare_engine_set_automation_lane(SonareRealtimeEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count) {
  if (!engine || (point_count > 0 && !points)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    // Reject non-finite breakpoints (both axes): a NaN/Inf ppq or value would
    // poison the lane's interpolation and propagate through every parameter the
    // lane drives.
    if (!std::isfinite(points[i].ppq) || !std::isfinite(points[i].value)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    breakpoints.push_back(
        {points[i].ppq, points[i].value, curve_from_int(points[i].curve_to_next)});
  }

  automation::AutomationLane lane(param_id);
  lane.set_points(std::move(breakpoints));
  auto found = std::find_if(
      engine->automation_lanes.begin(), engine->automation_lanes.end(),
      [&](const automation::AutomationLane& item) { return item.target_param_id() == param_id; });
  if (found == engine->automation_lanes.end()) {
    engine->automation_lanes.push_back(std::move(lane));
  } else {
    *found = std::move(lane);
  }
  engine->engine.automation().set_lanes(engine->automation_lanes);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_automation_lane_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.automation().lane_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_markers(SonareRealtimeEngine* engine,
                                      const SonareEngineMarker* markers, size_t marker_count) {
  if (!engine || (marker_count > 0 && !markers)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->marker_strings.clear();
  std::vector<transport::Marker> prepared;
  prepared.reserve(marker_count);
  for (size_t i = 0; i < marker_count; ++i) {
    if (!std::isfinite(markers[i].ppq) || markers[i].ppq < 0.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    engine->marker_strings.push_back(fixed_text(markers[i].name, sizeof(markers[i].name)));
    prepared.push_back({markers[i].ppq, markers[i].id, engine->marker_strings.back().c_str()});
  }
  engine->engine.set_markers(std::move(prepared));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_marker_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.marker_count();
  return SONARE_OK;
}

SonareError sonare_engine_marker_by_index(SonareRealtimeEngine* engine, size_t index,
                                          SonareEngineMarker* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  transport::Marker marker{};
  if (!engine->engine.marker_by_index(index, &marker)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
}

SonareError sonare_engine_marker(SonareRealtimeEngine* engine, uint32_t id,
                                 SonareEngineMarker* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  transport::Marker marker{};
  if (!engine->engine.marker_by_id(id, &marker)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
}

SonareError sonare_engine_seek_marker(SonareRealtimeEngine* engine, uint32_t marker_id,
                                      int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kSeekMarker;
  command.target_id = marker_id;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_loop_from_markers(SonareRealtimeEngine* engine,
                                                uint32_t start_marker_id, uint32_t end_marker_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  return engine->engine.set_loop_from_markers(start_marker_id, end_marker_id)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
}

SonareError sonare_engine_set_metronome(SonareRealtimeEngine* engine,
                                        const SonareEngineMetronomeConfig* config) {
  // click_samples == 0 is the documented "use the sample-rate-derived default"
  // sentinel (the engine derives it from click_seconds and the sample rate).
  // Only a negative explicit length is invalid.
  if (!engine || !config || config->beat_gain < 0.0f || config->accent_gain < 0.0f ||
      config->click_samples < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_metronome_config(metronome_from_c(*config));
  return SONARE_OK;
}

SonareError sonare_engine_metronome(SonareRealtimeEngine* engine,
                                    SonareEngineMetronomeConfig* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = metronome_to_c(engine->engine.metronome_config());
  return SONARE_OK;
}

SonareError sonare_engine_count_in_end_sample(SonareRealtimeEngine* engine, int64_t start_sample,
                                              int bars, int64_t* out_sample) {
  if (!engine || !out_sample || start_sample < 0 || bars <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out_sample = engine->engine.count_in_end_sample(start_sample, bars);
  return SONARE_OK;
}

SonareError sonare_engine_set_clips(SonareRealtimeEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count) {
  if (!engine || (clip_count > 0 && !clips)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<std::shared_ptr<engine::ClipAudioStorage>> clip_storage;
  clip_storage.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    if (!clip.channels || clip.num_channels <= 0 || clip.num_samples <= 0 ||
        !std::isfinite(clip.start_ppq) || clip.start_ppq < 0.0 || clip.clip_offset_samples < 0 ||
        clip.clip_offset_samples >= clip.num_samples || clip.length_samples <= 0 ||
        !(std::isfinite(clip.gain) && clip.gain >= 0.0f) || clip.fade_in_samples < 0 ||
        clip.fade_out_samples < 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    auto owned = std::make_shared<engine::ClipAudioStorage>();
    owned->channels.reserve(static_cast<size_t>(clip.num_channels));
    owned->channel_ptrs.reserve(static_cast<size_t>(clip.num_channels));
    for (int ch = 0; ch < clip.num_channels; ++ch) {
      if (!clip.channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
      owned->channels.emplace_back(clip.channels[ch], clip.channels[ch] + clip.num_samples);
    }
    clip_storage.push_back(std::move(owned));
  }

  std::vector<engine::ClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    auto& owned = clip_storage[i];
    owned->channel_ptrs.clear();
    for (const auto& channel : owned->channels) {
      owned->channel_ptrs.push_back(channel.data());
    }
    engine::ClipSchedule schedule{};
    schedule.id = clip.id;
    schedule.buffer = {owned->channel_ptrs.data(), clip.num_channels, clip.num_samples};
    schedule.storage = owned;
    schedule.start_ppq = clip.start_ppq;
    schedule.clip_offset_samples = clip.clip_offset_samples;
    schedule.length_samples = clip.length_samples;
    schedule.loop = clip.loop != 0;
    schedule.gain = clip.gain;
    schedule.fade_in_samples = clip.fade_in_samples;
    schedule.fade_out_samples = clip.fade_out_samples;
    schedules.push_back(schedule);
  }
  engine->engine.set_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clip_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.clip_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer) {
  if (!engine || !buffer || !buffer->channels || buffer->num_channels <= 0 ||
      buffer->capacity_frames <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (int ch = 0; ch < buffer->num_channels; ++ch) {
    if (!buffer->channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_segment(
      {buffer->channels, buffer->num_channels, buffer->capacity_frames});
  return SONARE_OK;
}

SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_capture_armed(armed != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled) {
  if (!engine || start_sample < 0 || end_sample < start_sample) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_punch(start_sample, end_sample, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_reset_capture(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.reset_capture();
  return SONARE_OK;
}

SonareError sonare_engine_capture_status(SonareRealtimeEngine* engine,
                                         SonareEngineCaptureStatus* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  out->captured_frames = engine->engine.captured_frames();
  out->overflow_count = engine->engine.capture_overflow_count();
  out->armed = engine->engine.capture_armed() ? 1 : 0;
  out->punch_enabled = engine->engine.capture_punch_enabled() ? 1 : 0;
  return SONARE_OK;
}

SonareError sonare_engine_set_graph(SonareRealtimeEngine* engine,
                                    const SonareEngineGraphSpec* spec) {
  if (!engine || !spec || !spec->nodes || spec->node_count == 0 || spec->num_channels <= 0 ||
      (spec->connection_count > 0 && !spec->connections) ||
      (spec->parameter_binding_count > 0 && !spec->parameter_bindings)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  SONARE_C_TRY
  auto graph = std::make_unique<graph::Graph>();
  for (size_t i = 0; i < spec->node_count; ++i) {
    const SonareEngineGraphNode& node = spec->nodes[i];
    const int ports = node.num_ports > 0 ? node.num_ports : spec->num_channels;
    auto processor = make_graph_processor(node);
    if (!processor ||
        !graph->add_node(fixed_text(node.id, sizeof(node.id)), std::move(processor), ports)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  for (size_t i = 0; i < spec->connection_count; ++i) {
    const SonareEngineGraphConnection& connection = spec->connections[i];
    graph::Connection graph_connection{};
    graph_connection.source_node =
        fixed_text(connection.source_node, sizeof(connection.source_node));
    graph_connection.source_port = connection.source_port;
    graph_connection.dest_node = fixed_text(connection.dest_node, sizeof(connection.dest_node));
    graph_connection.dest_port = connection.dest_port;
    graph_connection.mix =
        connection.mix == 0 ? graph::Connection::Mix::Replace : graph::Connection::Mix::Add;
    if (!graph->connect(std::move(graph_connection))) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  if (!graph->compile()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto state = engine->engine.transport().snapshot_control();
  graph->prepare(state.sample_rate, engine->engine.max_block_size());
  const std::string input_node = fixed_text(spec->input_node, sizeof(spec->input_node));
  const std::string output_node = fixed_text(spec->output_node, sizeof(spec->output_node));
  if (!engine->engine.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(),
                                 spec->num_channels)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < spec->parameter_binding_count; ++i) {
    const SonareEngineGraphParameterBinding& binding = spec->parameter_bindings[i];
    const std::string node_id = fixed_text(binding.node_id, sizeof(binding.node_id));
    if (!engine->engine.bind_graph_parameter(binding.param_id, node_id.c_str())) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_node_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_node_count();
#else
  *out_count = 0;
#endif
  return SONARE_OK;
}

SonareError sonare_engine_graph_connection_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_connection_count();
#else
  *out_count = 0;
#endif
  return SONARE_OK;
}

SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames) {
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process(channels, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames) {
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process_with_monitor(channels, monitor_out, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size) {
  if (!engine || !out || num_channels <= 0 || total_frames < 0 || block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.render_offline(out, num_channels, total_frames, block_size);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_bounce_options_default(SonareEngineBounceOptions* options) {
  if (!options) return SONARE_ERROR_INVALID_PARAMETER;
  *options = SonareEngineBounceOptions{};
  options->block_size = 128;
  options->num_channels = 2;
  options->target_sample_rate = 48000;
  options->source_sample_rate = 48000;
  options->normalize_lufs = 0;
  options->target_lufs = SONARE_DEFAULT_BOUNCE_TARGET_LUFS;
  options->dither = 0;
  options->dither_bits = 16;
  options->dither_seed = 0;
  return SONARE_OK;
}

SonareError sonare_engine_bounce_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineBounceOptions* options,
                                         SonareEngineBounceResult* out) {
  // Zero the owned out-pointer/lengths BEFORE any validation early-return so a
  // failed validation always leaves a NULL owned pointer (matching the analysis
  // wrappers in sonare_c.cpp). Otherwise the standard
  // sonare_free_bounce_result(&r) idiom would delete[] an uninitialised pointer.
  if (out) {
    *out = {};
  }
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || options->target_sample_rate <= 0 ||
      options->source_sample_rate <= 0 || options->dither_bits < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<std::vector<float>> channels(
      static_cast<size_t>(options->num_channels),
      std::vector<float>(static_cast<size_t>(options->total_frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels.size());
  for (auto& channel : channels) {
    ptrs.push_back(channel.data());
  }
  engine->engine.render_offline(ptrs.data(), options->num_channels, options->total_frames,
                                options->block_size);

  channels = resample_channels(channels, options->source_sample_rate, options->target_sample_rate);
  std::vector<float> interleaved = interleave_channels(channels);
  if (options->normalize_lufs) {
    // target_lufs == 0.0f is the documented "use default" sentinel; promote it
    // to the canonical SONARE_DEFAULT_BOUNCE_TARGET_LUFS so a zero-initialised
    // SonareEngineBounceOptions normalises to the same target regardless of
    // which binding (C, Node, Python, WASM) populated the struct. The
    // static_assert below pins the macro to the value the WASM wrapper used
    // to hardcode at the embind layer (see src/wasm/bindings.cpp::bounceOffline).
    static_assert(SONARE_DEFAULT_BOUNCE_TARGET_LUFS == -14.0f,
                  "SONARE_DEFAULT_BOUNCE_TARGET_LUFS must match the WASM/Node "
                  "facade default to keep cross-binding bounce behaviour identical");
    // Non-finite targets (NaN/Inf) also fall back to the default so a
    // garbage float can never propagate into the normalisation gain. Note the
    // 0.0f sentinel makes an exact 0 LUFS target unrepresentable; that is an
    // accepted trade-off documented on SonareEngineBounceOptions::target_lufs.
    const float effective_target_lufs =
        (options->target_lufs == 0.0f || !std::isfinite(options->target_lufs))
            ? SONARE_DEFAULT_BOUNCE_TARGET_LUFS
            : options->target_lufs;
    metering::normalize_interleaved_to_lufs(interleaved, channels[0].size(), options->num_channels,
                                            options->target_sample_rate, effective_target_lufs);
  }
  if (options->dither != 0) {
#if defined(SONARE_WITH_MASTERING)
    mastering::final::DitherConfig config{};
    config.type = dither_type_from_int(options->dither);
    config.target_bits = options->dither_bits > 0 ? options->dither_bits : 16;
    config.seed = options->dither_seed == 0 ? config.seed : options->dither_seed;
    Audio dithered = mastering::final::dither(
        Audio::from_buffer(interleaved.data(), interleaved.size(), options->target_sample_rate),
        config);
    interleaved.assign(dithered.data(), dithered.data() + dithered.size());
#else
    return SONARE_ERROR_NOT_SUPPORTED;
#endif
  }
  const auto loudness = metering::lufs_interleaved(
      interleaved.data(), channels[0].size(), options->num_channels, options->target_sample_rate);
  out->sample_count = interleaved.size();
  out->frames = static_cast<int64_t>(channels[0].size());
  out->num_channels = options->num_channels;
  out->sample_rate = options->target_sample_rate;
  out->integrated_lufs = loudness.integrated_lufs;
  out->interleaved = new float[interleaved.size()];
  std::memcpy(out->interleaved, interleaved.data(), interleaved.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_freeze_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out) {
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || !std::isfinite(options->start_ppq) ||
      options->start_ppq < 0.0 || !(std::isfinite(options->gain) && options->gain >= 0.0f)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *out = {};
  auto owned = std::make_shared<engine::ClipAudioStorage>();
  owned->channels.assign(static_cast<size_t>(options->num_channels),
                         std::vector<float>(static_cast<size_t>(options->total_frames), 0.0f));
  std::vector<float*> render_ptrs;
  render_ptrs.reserve(owned->channels.size());
  for (auto& channel : owned->channels) {
    render_ptrs.push_back(channel.data());
  }
  engine->engine.render_offline(render_ptrs.data(), options->num_channels, options->total_frames,
                                options->block_size);

  owned->channel_ptrs.clear();
  owned->channel_ptrs.reserve(owned->channels.size());
  for (const auto& channel : owned->channels) {
    owned->channel_ptrs.push_back(channel.data());
  }
  engine::ClipSchedule schedule{};
  schedule.id = options->clip_id == 0 ? 1 : options->clip_id;
  schedule.buffer = {owned->channel_ptrs.data(), options->num_channels, options->total_frames};
  schedule.storage = owned;
  schedule.start_ppq = options->start_ppq;
  schedule.clip_offset_samples = 0;
  schedule.length_samples = options->total_frames;
  schedule.loop = false;
  schedule.gain = options->gain;
  schedule.fade_in_samples = 0;
  schedule.fade_out_samples = 0;

  engine->engine.set_clips({schedule});
  out->clip_id = schedule.id;
  out->frames = options->total_frames;
  out->num_channels = options->num_channels;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_drain_telemetry(SonareRealtimeEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written) {
  if (!engine || !written || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  size_t count = 0;
  engine::Telemetry telemetry{};
  while (count < max_records && engine->engine.pop_telemetry(telemetry)) {
    out[count] = {static_cast<int>(telemetry.type),
                  static_cast<int>(telemetry.error),
                  telemetry.render_frame,
                  telemetry.timeline_sample,
                  telemetry.audible_timeline_sample,
                  telemetry.graph_latency_samples_q8,
                  telemetry.value};
    ++count;
  }
  *written = count;
  return SONARE_OK;
}

SonareError sonare_engine_drain_meter_telemetry(SonareRealtimeEngine* engine,
                                                SonareMeterTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::MeterTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_meter_telemetry(record)) {
    out[count].target_id = record.target_id;
    out[count].render_frame = record.render_frame;
    out[count].seq = record.seq;
    out[count].peak_db_l = record.peak_db[0];
    out[count].peak_db_r = record.peak_db[1];
    out[count].rms_db_l = record.rms_db[0];
    out[count].rms_db_r = record.rms_db[1];
    out[count].true_peak_db_l = record.true_peak_db[0];
    out[count].true_peak_db_r = record.true_peak_db[1];
    out[count].max_true_peak_db = record.max_true_peak_db;
    out[count].correlation = record.correlation;
    out[count].mono_compat_width = record.mono_compat_width;
    out[count].momentary_lufs = record.momentary_lufs;
    out[count].short_term_lufs = record.short_term_lufs;
    out[count].integrated_lufs = record.integrated_lufs;
    out[count].gain_reduction_db = record.gain_reduction_db;
    out[count].dropped_records = record.dropped_records;
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame) {
  if (!engine || !std::isfinite(value)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParam;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_parameter_smoothed(SonareRealtimeEngine* engine, uint32_t param_id,
                                                 float value, int64_t render_frame) {
  if (!engine || !std::isfinite(value)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParamSmoothed;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config) {
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(engine_synth_config_from_c(*config));
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_synth_instrument(SonareRealtimeEngine* engine,
                                               uint32_t destination_id,
                                               const SonareSynthPatch* patch) {
  if (!engine || !patch) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(*patch, &cfg, &error)) {
    set_last_error(error != nullptr ? error : "invalid synth patch");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size) {
  if (!engine || !data || size == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->soundfont = std::move(soundfont);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config) {
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (config->struct_version > 1) return SONARE_ERROR_INVALID_PARAMETER;
  // A missing SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor, so live MIDI stays audible with zero data.
  SONARE_C_TRY
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (config->gain > 0.0f) cfg.gain = config->gain;
  if (config->polyphony > 0) cfg.polyphony = config->polyphony;
  auto player = std::make_unique<sonare::midi::synth::Sf2Player>(cfg);
  player->set_soundfont(engine->soundfont);
  return bind_engine_instrument(engine, destination_id, std::move(player));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_instrument(destination_id, nullptr);
  engine->builtin_instruments.erase(
      std::remove_if(engine->builtin_instruments.begin(), engine->builtin_instruments.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      engine->builtin_instruments.end());
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_instrument_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value) {
  if (!engine || channel > 15 || controller > 127 || param_id == 0 || !std::isfinite(min_value) ||
      !std::isfinite(max_value) || max_value < min_value) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.bind_midi_cc(controller, channel, param_id, min_value, max_value)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_clear_midi_cc_bindings(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_cc_bindings();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_cc_binding_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_cc_binding_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id,
                                      const char* config_json) {
  if (!engine || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  const SonareError parse_err = midi_fx_chain_from_json(config_json, &chain);
  if (parse_err != SONARE_OK) return parse_err;
  return engine->engine.set_midi_fx(destination_id, chain) ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_fx(destination_id);
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_input_source(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(&engine->midi_input_source, destination_id);
  engine->midi_input_source_enabled = true;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_clear_midi_input_source(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(nullptr, 0);
  engine->midi_input_source_enabled = false;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_input_pending_count(SonareRealtimeEngine* engine,
                                                   size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->midi_input_source.pending_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_push_midi_input_note_on(SonareRealtimeEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_on(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_note_off(SonareRealtimeEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_off(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_cc(SonareRealtimeEngine* engine, uint8_t group,
                                             uint8_t channel, uint8_t controller, uint8_t value,
                                             int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)controller;
  (void)value;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || group > 15 || channel > 15 || controller > 127 ||
      value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_control_change(group, channel, controller, value), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_on(SonareRealtimeEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOnImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_off(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOffImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_cc(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t group, uint8_t channel, uint8_t controller,
                                       uint8_t value, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  if (group > 15 || channel > 15 || controller > 127 || value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  // Pack the scalar MIDI fields into arg.i using the encoding documented in
  // rt/command.h and decoded by RealtimeEngine::apply_command.
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  rt::Command command{};
  command.type = rt::CommandType::kMidiCcImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_panic(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const transport::TransportState state = engine->engine.transport_state_control();
  out->playing = state.playing ? 1 : 0;
  out->looping = state.looping ? 1 : 0;
  out->render_frame = state.render_frame;
  out->sample_position = state.sample_position;
  out->ppq_position = state.ppq_position;
  out->bpm = state.bpm;
  out->loop_start_ppq = state.loop_start_ppq;
  out->loop_end_ppq = state.loop_end_ppq;
  out->sample_rate = state.sample_rate;
  out->bar_start_ppq = state.bar_start_ppq;
  out->bar_count = state.bar_count;
  out->time_signature.numerator = state.time_sig.numerator;
  out->time_signature.denominator = state.time_sig.denominator;
  out->time_signature.confidence = 1.0f;
  return SONARE_OK;
}
