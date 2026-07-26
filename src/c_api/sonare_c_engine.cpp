#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "automation/parameter.h"
#include "engine/realtime_engine.h"
#include "rt/command.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_engine_detail;

namespace {

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
  out->kind = marker.kind;
  out->key_fifths = marker.key_fifths;
  out->key_minor = marker.key_minor ? 1 : 0;
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

}  // namespace

SonareError sonare_engine_create(SonareRealtimeEngine** out) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  if (!engine || sample_rate <= 0.0 || max_block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_play(SonareRealtimeEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_stop(SonareRealtimeEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_sample(SonareRealtimeEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_ppq(SonareRealtimeEngine* engine, double ppq, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine || !std::isfinite(ppq) || !transport::valid_public_ppq(ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  command.arg.d = ppq;  // full double precision; engine applies without truncation
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_settle_parameters(SonareRealtimeEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.settle_parameters();
  return SONARE_OK;
}

SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm) {
  SONARE_C_API_ENTRY;
  if (!engine || !transport::valid_public_tempo(bpm)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_tempo(bpm);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator) {
  SONARE_C_API_ENTRY;
  if (!engine || numerator <= 0 || denominator <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_time_signature(numerator, denominator);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_tempo_segments(SonareRealtimeEngine* engine,
                                             const SonareProjectTempoSegment* segments,
                                             size_t segment_count) {
  SONARE_C_API_ENTRY;
  if (!engine || (segment_count > 0 && !segments) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<transport::TempoSegment> out;
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTempoSegment& in = segments[i];
    const transport::TempoSegment candidate{in.start_ppq, in.bpm, 0.0, in.end_bpm};
    if (!transport::valid_public_tempo_segment(candidate)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    transport::TempoSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.bpm = in.bpm;
    seg.start_sample = 0.0;
    seg.end_bpm = in.end_bpm;
    out.push_back(seg);
  }
  SONARE_C_TRY
  engine->engine.set_tempo_segments(std::move(out));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature_segments(
    SonareRealtimeEngine* engine, const SonareProjectTimeSignatureSegment* segments,
    size_t segment_count) {
  SONARE_C_API_ENTRY;
  if (!engine || (segment_count > 0 && !segments) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<transport::TimeSignatureSegment> out;
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTimeSignatureSegment& in = segments[i];
    const transport::TimeSignatureSegment candidate{in.start_ppq, {in.numerator, in.denominator}};
    if (!transport::valid_public_time_signature_segment(candidate)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    transport::TimeSignatureSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.time_sig.numerator = in.numerator;
    seg.time_sig.denominator = in.denominator;
    out.push_back(seg);
  }
  SONARE_C_TRY
  engine->engine.set_time_signature_segments(std::move(out));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_sample_at_ppq(SonareRealtimeEngine* engine, double ppq,
                                        int64_t* out_sample) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_sample || !std::isfinite(ppq) || !transport::valid_public_ppq(ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_sample = engine->engine.sample_at_ppq(ppq);
  return SONARE_OK;
}

SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled) {
  SONARE_C_API_ENTRY;
  if (!engine || !std::isfinite(start_ppq) || !std::isfinite(end_ppq) ||
      !transport::valid_public_ppq(start_ppq) || !transport::valid_public_ppq(end_ppq) ||
      (enabled && end_ppq <= start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_loop(start_ppq, end_ppq, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info) {
  SONARE_C_API_ENTRY;
  if (!engine || !info || info->max_value < info->min_value) return SONARE_ERROR_INVALID_PARAMETER;
  // Reject an out-of-range default curve ordinal instead of silently clamping,
  // matching the automation-lane path and the other surfaces.
  if (info->default_curve < 0 || info->default_curve > 3) return SONARE_ERROR_INVALID_PARAMETER;
  if (sonare::engine::RealtimeEngine::parameter_target_reserved(info->id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  // Clear the registry first (it holds raw pointers into parameter_strings),
  // then release the backing strings so no dangling pointer ever exists.
  engine->parameters.clear();
  engine->parameter_strings.clear();
  publish_parameter_metadata(engine);
  return SONARE_OK;
}

SonareError sonare_engine_parameter_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->parameters.parameter_count();
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info_by_index(SonareRealtimeEngine* engine, size_t index,
                                                  SonareParameterInfo* out) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
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
    // lane drives. An out-of-range curve ordinal is rejected (not clamped) so
    // every surface reports the same error for the same invalid input.
    if (!automation::valid_public_breakpoint(points[i].ppq, points[i].value,
                                             points[i].curve_to_next)) {
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
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.automation().lane_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_markers(SonareRealtimeEngine* engine,
                                      const SonareEngineMarker* markers, size_t marker_count) {
  SONARE_C_API_ENTRY;
  if (!engine || (marker_count > 0 && !markers)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  // All-or-nothing: stage the names and markers in local storage so a mid-list
  // validation failure leaves the engine's existing marker store (and the
  // non-owning Marker::name pointers into engine->marker_strings) untouched.
  // Clearing engine->marker_strings before validation would dangle the live
  // MarkerMap on early return -> use-after-free in later marker queries.
  // marker_strings is a std::deque, so element addresses stay stable across
  // push_back and across the move below.
  std::deque<std::string> staged_strings;
  std::vector<transport::Marker> prepared;
  prepared.reserve(marker_count);
  std::vector<uint32_t> staged_ids;
  staged_ids.reserve(marker_count);
  for (size_t i = 0; i < marker_count; ++i) {
    if (markers[i].id == 0 ||
        std::find(staged_ids.begin(), staged_ids.end(), markers[i].id) != staged_ids.end() ||
        !std::isfinite(markers[i].ppq) || markers[i].ppq < 0.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    staged_ids.push_back(markers[i].id);
    staged_strings.push_back(fixed_text(markers[i].name, sizeof(markers[i].name)));
    transport::Marker prepared_marker;
    prepared_marker.ppq = markers[i].ppq;
    prepared_marker.id = markers[i].id;
    prepared_marker.name = staged_strings.back().c_str();
    prepared_marker.kind = markers[i].kind;
    prepared_marker.key_fifths = markers[i].key_fifths;
    prepared_marker.key_minor = markers[i].key_minor != 0;
    prepared.push_back(prepared_marker);
  }
  // Publish the new marker snapshot BEFORE replacing the backing string storage.
  // The prepared markers' `name` pointers reference nodes in `staged_strings`
  // (a deque, so the move preserves element addresses). Replacing
  // `engine->marker_strings` first would free the old storage while its snapshot
  // is still the published one, dangling any audio-thread reader's `.name`.
  engine->engine.set_markers(std::move(prepared));
  engine->marker_strings = std::move(staged_strings);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_marker_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.marker_count();
  return SONARE_OK;
}

SonareError sonare_engine_marker_by_index(SonareRealtimeEngine* engine, size_t index,
                                          SonareEngineMarker* out) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kSeekMarker;
  command.target_id = marker_id;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_loop_from_markers(SonareRealtimeEngine* engine,
                                                uint32_t start_marker_id, uint32_t end_marker_id) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  return engine->engine.set_loop_from_markers(start_marker_id, end_marker_id)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
}

SonareError sonare_engine_set_metronome(SonareRealtimeEngine* engine,
                                        const SonareEngineMetronomeConfig* config) {
  SONARE_C_API_ENTRY;
  // Zero keeps the documented default sentinel. Reject hostile values at the
  // public boundary; the core also clamps direct/internal configurations.
  if (!engine || !config || !std::isfinite(config->beat_gain) || config->beat_gain < 0.0f ||
      !std::isfinite(config->accent_gain) || config->accent_gain < 0.0f ||
      config->click_samples < 0 || config->click_samples > engine::kMaxMetronomeClickSamples ||
      !std::isfinite(config->click_seconds) || config->click_seconds < 0.0 ||
      config->click_seconds > engine::kMaxMetronomeClickSeconds) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_metronome_config(metronome_from_c(*config));
  return SONARE_OK;
}

SonareError sonare_engine_metronome(SonareRealtimeEngine* engine,
                                    SonareEngineMetronomeConfig* out) {
  SONARE_C_API_ENTRY;
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = metronome_to_c(engine->engine.metronome_config());
  return SONARE_OK;
}

SonareError sonare_engine_count_in_end_sample(SonareRealtimeEngine* engine, int64_t start_sample,
                                              int bars, int64_t* out_sample) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_sample || start_sample < 0 || bars <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out_sample = engine->engine.count_in_end_sample(start_sample, bars);
  return SONARE_OK;
}

SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
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

SonareError sonare_engine_set_param_smoothing_ms(SonareRealtimeEngine* engine, float smoothing_ms) {
  SONARE_C_API_ENTRY;
  if (!engine || !std::isfinite(smoothing_ms) || smoothing_ms < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_param_smoothing_ms(smoothing_ms);
  return SONARE_OK;
}

SonareError sonare_engine_set_solo_mute(SonareRealtimeEngine* engine, uint32_t lane_index, int solo,
                                        int mute, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)lane_index;
  (void)solo;
  (void)mute;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kSetSoloMute;
  command.target_id = lane_index;
  command.sample_time = render_frame;
  command.arg.i = (mute ? 0x1 : 0x0) | (solo ? 0x2 : 0x0);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out) {
  SONARE_C_API_ENTRY;
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
  out->beat = state.beat;
  out->beat_fraction = state.beat_fraction;
  return SONARE_OK;
}
