#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include "automation/parameter.h"
#include "engine/realtime_engine.h"
#include "rt/command.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

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

// A reserved-namespace description carries no id of its own (the id is the
// query, not a stored field), so it is echoed back from @p id rather than
// read off @p description.
#if defined(SONARE_WITH_MIXING) || defined(SONARE_WITH_ARRANGEMENT)
void fill_c_parameter_description(uint32_t id, const automation::ParameterDescription& description,
                                  SonareParameterInfo* out) {
  out->id = id;
  copy_text(out->name, sizeof(out->name), description.name.c_str());
  copy_text(out->unit, sizeof(out->unit), description.unit.c_str());
  out->min_value = description.min_value;
  out->max_value = description.max_value;
  out->default_value = description.default_value;
  out->rt_safe = description.rt_safe ? 1 : 0;
  out->default_curve = curve_to_int(description.default_curve);
}
#endif

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

void publish_parameter_metadata(SonareEngine* engine) {
  engine->engine.automation().set_parameter_metadata(
      parameter_metadata_snapshot(engine->parameters));
}

bool registered_parameter_rejects_realtime(const SonareEngine* engine, uint32_t param_id) {
  automation::ParameterInfo info{};
  return engine->parameters.parameter_info(param_id, &info) && !info.rt_safe;
}

// A reserved mixer id that names no strip (the retired positional track/bus
// encoding) is refused at the control-thread entry.
SonareError check_automatable_parameter(const SonareEngine* engine, uint32_t param_id) {
  return run_validation([&] {
    if (engine::RealtimeEngine::mixer_parameter_names_no_strip(param_id)) {
      util::check::refuse_unknown_id("param_id", "a mixer strip's parameter", "engine");
    }
    if (registered_parameter_rejects_realtime(engine, param_id)) {
      util::check::refuse("param_id", "must name a realtime-safe parameter");
    }
  });
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

// The public bounds and the core's clamps are two spellings of one limit; keep
// them pinned so the header cannot promise a ceiling the engine does not honor.
static_assert(SONARE_ENGINE_MAX_COMMAND_CAPACITY == engine::RealtimeEngine::kMaxCommandCapacity,
              "SONARE_ENGINE_MAX_COMMAND_CAPACITY drift");
static_assert(SONARE_ENGINE_MAX_TELEMETRY_CAPACITY == engine::RealtimeEngine::kMaxTelemetryCapacity,
              "SONARE_ENGINE_MAX_TELEMETRY_CAPACITY drift");
static_assert(SONARE_ENGINE_MAX_WARP_VOICES == engine::RealtimeEngine::kMaxWarpVoices,
              "SONARE_ENGINE_MAX_WARP_VOICES drift");
// sonare_engine_push_midi_sysex's doc promises a 1..512 byte payload and the
// guard spells that ceiling as the engine constant; this pins the prose to it.
static_assert(engine::RealtimeEngine::kMaxSysExPayloadBytes == 512,
              "sonare_engine_push_midi_sysex documents a 1..512 byte SysEx payload");

// The engine multiplies telemetry_capacity by the metered lane count before
// reserving, so an unchecked capacity is an unbounded allocation (and, far
// enough out, an overflowing multiply). The core clamps; the C ABI reports the
// out-of-range request instead of silently preparing a smaller engine than the
// host asked for.
SonareError check_prepare_limits(double sample_rate, int max_block_size, size_t command_capacity,
                                 size_t telemetry_capacity) {
  return run_validation([&] {
    if (!is_supported_sample_rate(sample_rate)) {
      util::check::refuse("sample_rate", util::refusal::integer_range_predicate(
                                             kMinAudioSampleRate, kMaxAudioSampleRate));
    }
    util::check::integer_in_range(max_block_size, 1, kMaxRealtimeBlockSize, "max_block_size");
    util::check::integer_in_range(static_cast<long long>(command_capacity), 0,
                                  SONARE_ENGINE_MAX_COMMAND_CAPACITY, "command_capacity");
    util::check::integer_in_range(static_cast<long long>(telemetry_capacity), 0,
                                  SONARE_ENGINE_MAX_TELEMETRY_CAPACITY, "telemetry_capacity");
  });
}

engine::MetronomeConfig metronome_from_c(const SonareEngineMetronomeConfig& config) {
  engine::MetronomeConfig out;
  out.enabled = config.enabled != 0;
  out.beat_gain = config.beat_gain;
  out.accent_gain = config.accent_gain;
  // When click_samples is 0 the engine derives the length from click_sec and
  // the prepared sample rate; a positive click_samples overrides that. Treat a
  // non-positive click_sec as "use the default" so older callers that leave
  // the field zero-initialized keep the 2 ms behavior.
  out.click_samples = config.click_samples;
  if (config.click_sec > 0.0) {
    out.click_seconds = config.click_sec;
  }
  return out;
}

}  // namespace

SonareError sonare_engine_create(SonareEngine** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  *out = new SonareEngine{};
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_engine_destroy(SonareEngine* engine) { delete engine; }

SonareError sonare_engine_prepare(SonareEngine* engine, double sample_rate, int max_block_size,
                                  size_t command_capacity, size_t telemetry_capacity) {
  SONARE_C_API_ENTRY;
  if (max_block_size == 0) max_block_size = engine::RealtimeEngine::kDefaultMaxBlock;
  if (command_capacity == 0) command_capacity = engine::RealtimeEngine::kDefaultCommandCapacity;
  if (telemetry_capacity == 0) {
    telemetry_capacity = engine::RealtimeEngine::kDefaultTelemetryCapacity;
  }
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          check_prepare_limits(sample_rate, max_block_size, command_capacity, telemetry_capacity);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  engine->engine.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_prepare_with_channels(SonareEngine* engine, double sample_rate,
                                                int max_block_size, size_t command_capacity,
                                                size_t telemetry_capacity, int max_channels) {
  SONARE_C_API_ENTRY;
  if (max_block_size == 0) max_block_size = engine::RealtimeEngine::kDefaultMaxBlock;
  if (command_capacity == 0) command_capacity = engine::RealtimeEngine::kDefaultCommandCapacity;
  if (telemetry_capacity == 0) {
    telemetry_capacity = engine::RealtimeEngine::kDefaultTelemetryCapacity;
  }
  if (max_channels == 0) max_channels = engine::RealtimeEngine::kDefaultMaxChannels;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          check_prepare_limits(sample_rate, max_block_size, command_capacity, telemetry_capacity);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        util::check::integer_in_range(
            max_channels, 1, static_cast<long long>(engine::RealtimeEngine::kMaxAudioChannels),
            "max_channels");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  engine->engine.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity,
                         max_channels);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_play(SonareEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  rt::Command command{};
  command.type = rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_stop(SonareEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  rt::Command command{};
  command.type = rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_sample(SonareEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  // A negative position is refused, not clamped onto the timeline start.
  if (const SonareError err = run_validation(
          [&] { util::check::integer_at_least<int64_t>(timeline_sample, 0, "timeline_sample"); });
      err != SONARE_OK) {
    return err;
  }
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_ppq(SonareEngine* engine, double ppq, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          run_validation([&] { util::check::in_range(ppq, 0.0, transport::kMaxPublicPpq, "ppq"); });
      err != SONARE_OK) {
    return err;
  }
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  command.arg.d = ppq;  // full double precision; engine applies without truncation
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_reset_master_loudness_meter(SonareEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if defined(SONARE_WITH_MIXING)
  return engine->engine.reset_master_meter_integrated(render_frame) ? SONARE_OK
                                                                    : SONARE_ERROR_OUT_OF_MEMORY;
#else
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_settle_parameters(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.settle_parameters();
  return SONARE_OK;
}

SonareError sonare_engine_flush_control_commands(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.flush_control_commands();
  return SONARE_OK;
}

SonareError sonare_engine_settle_insert_parameters(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.settle_insert_parameters();
  return SONARE_OK;
}

SonareError sonare_engine_apply_commands_due_now_preserving_future(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.apply_commands_due_now_preserving_future();
  return SONARE_OK;
}

SonareError sonare_engine_set_tempo(SonareEngine* engine, double bpm) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] {
        util::check::in_interval(bpm, 0.0, false, transport::kMaxPublicTempoBpm, true, "bpm");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  engine->engine.set_tempo(bpm);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature(SonareEngine* engine, int numerator, int denominator) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] {
        util::check::integer_at_least(numerator, 1, "numerator");
        util::check::integer_at_least(denominator, 1, "denominator");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  engine->engine.set_time_signature(numerator, denominator);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_tempo_segments(SonareEngine* engine,
                                             const SonareProjectTempoSegment* segments,
                                             size_t segment_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(segments, segment_count);
  util::check::integer_in_range(static_cast<long long>(segment_count), 0,
                                static_cast<long long>(kMaxBufferSize), "segment_count");
  std::vector<transport::TempoSegment> out;
  // The staging vector is sized from a caller-supplied count, so its allocation
  // has to sit inside the try or a bad_alloc crosses the extern "C" boundary.
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTempoSegment& in = segments[i];
    const std::string prefix = "segments[" + std::to_string(i) + "].";
    util::check::in_range(in.start_ppq, 0.0, transport::kMaxPublicPpq,
                          (prefix + "start_ppq").c_str());
    util::check::in_interval(in.bpm, 0.0, false, transport::kMaxPublicTempoBpm, true,
                             (prefix + "bpm").c_str());
    util::check::in_range(in.end_bpm, 0.0, transport::kMaxPublicTempoBpm,
                          (prefix + "end_bpm").c_str());
    transport::TempoSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.bpm = in.bpm;
    seg.start_sample = 0.0;
    seg.end_bpm = in.end_bpm;
    out.push_back(seg);
  }
  engine->engine.set_tempo_segments(std::move(out));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature_segments(
    SonareEngine* engine, const SonareProjectTimeSignatureSegment* segments, size_t segment_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(segments, segment_count);
  util::check::integer_in_range(static_cast<long long>(segment_count), 0,
                                static_cast<long long>(kMaxBufferSize), "segment_count");
  std::vector<transport::TimeSignatureSegment> out;
  // Same reason as sonare_engine_set_tempo_segments: a caller-sized allocation
  // must not throw outside the try.
  out.reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectTimeSignatureSegment& in = segments[i];
    const std::string prefix = "segments[" + std::to_string(i) + "].";
    util::check::in_range(in.start_ppq, 0.0, transport::kMaxPublicPpq,
                          (prefix + "start_ppq").c_str());
    util::check::integer_at_least(in.numerator, 1, (prefix + "numerator").c_str());
    util::check::integer_at_least(in.denominator, 1, (prefix + "denominator").c_str());
    transport::TimeSignatureSegment seg;
    seg.start_ppq = in.start_ppq;
    seg.time_sig.numerator = in.numerator;
    seg.time_sig.denominator = in.denominator;
    out.push_back(seg);
  }
  engine->engine.set_time_signature_segments(std::move(out));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_sample_at_ppq(SonareEngine* engine, double ppq, int64_t* out_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_sample);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          run_validation([&] { util::check::in_range(ppq, 0.0, transport::kMaxPublicPpq, "ppq"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_sample);
  *out_sample = engine->engine.sample_at_ppq(ppq);
  return SONARE_OK;
}

SonareError sonare_engine_set_loop(SonareEngine* engine, double start_ppq, double end_ppq,
                                   int enabled) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          run_validation([&] { transport::check_loop_range(start_ppq, end_ppq, enabled != 0); });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_loop(start_ppq, end_ppq, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_add_parameter(SonareEngine* engine, const SonareParameterInfo* info) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(info);
  if (const SonareError err = run_validation([&] {
        // default_value needs finiteness only: nothing clamps it to [min_value, max_value].
        util::check::finite_float(info->min_value, "info.min_value");
        util::check::finite_float(info->max_value, "info.max_value");
        util::check::finite_float(info->default_value, "info.default_value");
        if (info->max_value < info->min_value) {
          util::check::refuse_relation({"info.max_value", "info.min_value"},
                                       "{0} must be at least {1}");
        }
        // Reject an out-of-range default curve ordinal instead of silently clamping,
        // matching the automation-lane path and the other surfaces.
        util::check::one_of(info->default_curve, std::array<int, 4>{0, 1, 2, 3},
                            "info.default_curve");
        util::check::integer_in_range(info->id, 1, std::numeric_limits<uint32_t>::max(), "info.id");
        if (sonare::engine::RealtimeEngine::parameter_target_reserved(info->id)) {
          util::check::refuse("info.id", "must not be a reserved engine parameter id");
        }
      });
      err != SONARE_OK) {
    return err;
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
    return run_validation(
        [&] { util::check::refuse("info.id", "must be unique among registered parameters"); });
  }
  publish_parameter_metadata(engine);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clear_parameters(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  // Clear the registry first (it holds raw pointers into parameter_strings),
  // then release the backing strings so no dangling pointer ever exists.
  engine->parameters.clear();
  engine->parameter_strings.clear();
  publish_parameter_metadata(engine);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_parameter_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->parameters.parameter_count();
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info_by_index(SonareEngine* engine, size_t index,
                                                  SonareParameterInfo* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  automation::ParameterInfo info{};
  if (!engine->parameters.parameter_info_by_index(index, &info)) {
    return run_validation([&] {
      util::check::refuse("index", "must be less than the number of registered parameters");
    });
  }
  fill_c_parameter(info, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_parameter_info(SonareEngine* engine, uint32_t id,
                                         SonareParameterInfo* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  automation::ParameterInfo info{};
  if (engine->parameters.parameter_info(id, &info)) {
    fill_c_parameter(info, out);
    return SONARE_OK;
  }
#if defined(SONARE_WITH_MIXING) || defined(SONARE_WITH_ARRANGEMENT)
  automation::ParameterDescription description{};
  if (engine->engine.describe_reserved_parameter(id, &description)) {
    fill_c_parameter_description(id, description, out);
    return SONARE_OK;
  }
#endif
  return run_validation([&] { util::check::refuse_unknown_id("id", "a parameter", "engine"); });
  SONARE_C_CATCH
}

SonareError sonare_engine_set_automation_lane(SonareEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(points, point_count);
  if (const SonareError err = check_automatable_parameter(engine, param_id); err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  std::vector<automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    // Reject non-finite breakpoints (both axes): a NaN/Inf ppq or value would
    // poison the lane's interpolation and propagate through every parameter the
    // lane drives. An out-of-range curve ordinal is rejected (not clamped) so
    // every surface reports the same error for the same invalid input.
    const std::string prefix = "points[" + std::to_string(i) + "].";
    util::check::finite(points[i].ppq, (prefix + "ppq").c_str());
    util::check::finite_float(points[i].value, (prefix + "value").c_str());
    util::check::one_of(points[i].curve_to_next, std::array<int, 4>{0, 1, 2, 3},
                        (prefix + "curve_to_next").c_str());
    breakpoints.push_back(
        {points[i].ppq, points[i].value, curve_from_int(points[i].curve_to_next)});
  }

  if (breakpoints.empty()) {
    // An empty lane drives nothing, so drop it rather than let cleared lanes accumulate.
    engine->automation_lanes.erase(
        std::remove_if(engine->automation_lanes.begin(), engine->automation_lanes.end(),
                       [&](const automation::AutomationLane& item) {
                         return item.target_param_id() == param_id;
                       }),
        engine->automation_lanes.end());
    engine->engine.automation().set_lanes(engine->automation_lanes);
    return SONARE_OK;
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

SonareError sonare_engine_automation_lane_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->engine.automation().lane_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_markers(SonareEngine* engine, const SonareEngineMarker* markers,
                                      size_t marker_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(markers, marker_count);
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
    const std::string prefix = "markers[" + std::to_string(i) + "].";
    util::check::integer_at_least(markers[i].id, 1, (prefix + "id").c_str());
    if (std::find(staged_ids.begin(), staged_ids.end(), markers[i].id) != staged_ids.end()) {
      util::check::refuse((prefix + "id").c_str(), "must be unique across markers");
    }
    util::check::at_least(markers[i].ppq, 0.0, (prefix + "ppq").c_str());
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

SonareError sonare_engine_marker_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->engine.marker_count();
  return SONARE_OK;
}

SonareError sonare_engine_marker_by_index(SonareEngine* engine, size_t index,
                                          SonareEngineMarker* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  transport::Marker marker{};
  if (!engine->engine.marker_by_index(index, &marker)) {
    return run_validation(
        [&] { util::check::refuse("index", "must be less than the number of markers"); });
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_marker(SonareEngine* engine, uint32_t id, SonareEngineMarker* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  transport::Marker marker{};
  if (!engine->engine.marker_by_id(id, &marker)) {
    return run_validation([&] { util::check::refuse_unknown_id("id", "a marker", "engine"); });
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_seek_marker(SonareEngine* engine, uint32_t marker_id,
                                      int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  rt::Command command{};
  command.type = rt::CommandType::kSeekMarker;
  command.target_id = marker_id;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_loop_from_markers(SonareEngine* engine, uint32_t start_marker_id,
                                                uint32_t end_marker_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (engine->engine.set_loop_from_markers(start_marker_id, end_marker_id)) return SONARE_OK;
  return run_validation([&] {
    transport::Marker start{};
    transport::Marker end{};
    if (!engine->engine.marker_by_id(start_marker_id, &start)) {
      util::check::refuse_unknown_id("start_marker_id", "a marker", "engine");
    }
    if (!engine->engine.marker_by_id(end_marker_id, &end)) {
      util::check::refuse_unknown_id("end_marker_id", "a marker", "engine");
    }
    util::check::refuse_relation({"end_marker_id", "start_marker_id"},
                                 "the marker {0} names must be after the marker {1} names");
  });
}

SonareError sonare_engine_set_metronome(SonareEngine* engine,
                                        const SonareEngineMetronomeConfig* config) {
  SONARE_C_API_ENTRY;
  // Zero keeps the documented default sentinel. Reject hostile values at the
  // public boundary; the core also clamps direct/internal configurations.
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(config);
  if (const SonareError err = run_validation([&] {
        util::check::at_least(config->beat_gain, 0.0, "config.beat_gain");
        util::check::at_least(config->accent_gain, 0.0, "config.accent_gain");
        util::check::integer_in_range(config->click_samples, 0, engine::kMaxMetronomeClickSamples,
                                      "config.click_samples");
        util::check::in_range(config->click_sec, 0.0, engine::kMaxMetronomeClickSeconds,
                              "config.click_sec");
      });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_metronome_config(metronome_from_c(*config));
  return SONARE_OK;
}

SonareError sonare_engine_metronome(SonareEngine* engine, SonareEngineMetronomeConfig* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  *out = metronome_to_c(engine->engine.metronome_config());
  return SONARE_OK;
}

SonareError sonare_engine_count_in_end_sample(SonareEngine* engine, int64_t start_sample, int bars,
                                              int64_t* out_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_sample);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation(
          [&] { util::check::integer_at_least<int64_t>(start_sample, 0, "start_sample"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_sample);
  // bars <= 0 counts nothing and answers start_sample, as the core does.
  *out_sample = engine->engine.count_in_end_sample(start_sample, bars);
  return SONARE_OK;
}

SonareError sonare_engine_set_parameter(SonareEngine* engine, uint32_t param_id, float value,
                                        int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_automatable_parameter(engine, param_id); err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { util::check::finite_float(value, "value"); });
      err != SONARE_OK) {
    return err;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParam;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_parameter_smoothed(SonareEngine* engine, uint32_t param_id,
                                                 float value, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_automatable_parameter(engine, param_id); err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { util::check::finite_float(value, "value"); });
      err != SONARE_OK) {
    return err;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParamSmoothed;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_param_smoothing_ms(SonareEngine* engine, float smoothing_ms) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          run_validation([&] { util::check::at_least(smoothing_ms, 0.0, "smoothing_ms"); });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_param_smoothing_ms(smoothing_ms);
  return SONARE_OK;
}

SonareError sonare_engine_set_solo_mute(SonareEngine* engine, uint32_t lane_index, int solo,
                                        int mute, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
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

SonareError sonare_engine_set_track_monitor_mode(SonareEngine* engine, uint32_t lane_index,
                                                 SonareEngineTrackMonitorMode mode,
                                                 int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] {
        util::check::one_of(mode,
                            std::array<int, 3>{SONARE_ENGINE_TRACK_MONITOR_MODE_OFF,
                                               SONARE_ENGINE_TRACK_MONITOR_MODE_PFL,
                                               SONARE_ENGINE_TRACK_MONITOR_MODE_AFL},
                            "mode");
      });
      err != SONARE_OK) {
    return err;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)lane_index;
  (void)mode;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kSetTrackMonitorMode;
  command.target_id = lane_index;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(mode);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_get_transport_state(SonareEngine* engine, SonareTransportState* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
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

SonareError sonare_engine_reset_processor_state(SonareEngine* engine, int64_t render_frame) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  return engine->engine.reset_processor_state(render_frame) ? SONARE_OK
                                                            : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_prime_offline_parameters(SonareEngine* engine, int num_channels,
                                                   int block_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] {
        util::check::integer_at_least(num_channels, 1, "num_channels");
        util::check::integer_at_least(block_size, 0, "block_size");
      });
      err != SONARE_OK) {
    return err;
  }
  if (engine->engine.max_block_size() <= 0) return SONARE_ERROR_INVALID_STATE;
  block_size = resolve_offline_block(engine->engine, block_size);
  if (const SonareError err = run_validation([&] {
        util::check::integer_in_range(num_channels, 1, engine->engine.prepared_channels(),
                                      "num_channels");
        util::check::integer_in_range(block_size, 1, engine->engine.max_block_size(), "block_size");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  engine->engine.prime_offline_parameters(num_channels, block_size);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_graph_latency_samples_q8(SonareEngine* engine, int* out_latency_q8) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_latency_q8);
  *out_latency_q8 = engine->engine.graph_latency_samples_q8();
  return SONARE_OK;
}

SonareError sonare_engine_max_block_size(const SonareEngine* engine, int* out_max_block_size) {
  SONARE_C_API_ENTRY;
  if (out_max_block_size) *out_max_block_size = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_max_block_size);
  *out_max_block_size = engine->engine.max_block_size();
  return SONARE_OK;
}

SonareError sonare_engine_tail_samples(SonareEngine* engine, int* out_tail_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_tail_samples);
  *out_tail_samples = engine->engine.tail_samples();
  return SONARE_OK;
  SONARE_C_CATCH
}
