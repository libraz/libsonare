#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "metering/lufs.h"
#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#endif
#if defined(SONARE_WITH_VOICE_CHANGER)
#include "editing/voice_changer/realtime_voice_changer.h"
#include "editing/voice_changer/voice_changer.h"
#endif
#include "automation/parameter.h"
#include "core/audio.h"
#include "core/resample.h"
#include "engine/realtime_engine.h"
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

namespace {

#if defined(SONARE_WITH_PITCH_EDITOR) || defined(SONARE_WITH_VOICE_CHANGER)
SonareError copy_audio_result(const Audio& result, float** out, size_t* out_length) {
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
}
#endif

#if defined(SONARE_WITH_VOICE_CHANGER)
char* copy_c_string(const std::string& value) {
  char* out = new char[value.size() + 1];
  std::memcpy(out, value.c_str(), value.size() + 1);
  return out;
}
#endif

}  // namespace

#if defined(SONARE_WITH_VOICE_CHANGER)
struct SonareRealtimeVoiceChanger {
  editing::voice_changer::RealtimeVoiceChanger changer;
  int max_block_size = 0;
  int num_channels = 1;
  // Persistent scratch buffers for interleaved -> planar conversion so that
  // realtime callers don't trigger per-block heap allocations.
  std::vector<float> planar_scratch;        // num_channels * max_block_size
  std::vector<float*> planar_channel_ptrs;  // num_channels pointers into planar_scratch
};
#endif

namespace {
// Variadic no-op used by SONARE_VC_STUB to swallow unused parameters in the
// out-of-feature build configuration without per-parameter (void) casts.
template <typename... T>
inline void ignore_args(const T&...) noexcept {}
}  // namespace

// Helper macro for stubbing out C API functions when the optional voice
// changer module is not compiled in. Suppresses unused-parameter warnings and
// returns SONARE_ERROR_NOT_SUPPORTED.
#define SONARE_VC_STUB(...)            \
  do {                                 \
    ignore_args(__VA_ARGS__);          \
    return SONARE_ERROR_NOT_SUPPORTED; \
  } while (false)

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

void fill_c_marker(const transport::Marker& marker, SonareEngineMarker* out) {
  out->id = marker.id;
  out->ppq = marker.ppq;
  copy_text(out->name, sizeof(out->name), marker.name);
}

SonareEngineMetronomeConfig metronome_to_c(const engine::MetronomeConfig& config) {
  return {config.enabled ? 1 : 0, config.beat_gain, config.accent_gain, config.click_samples};
}

engine::MetronomeConfig metronome_from_c(const SonareEngineMetronomeConfig& config) {
  engine::MetronomeConfig out;
  out.enabled = config.enabled != 0;
  out.beat_gain = config.beat_gain;
  out.accent_gain = config.accent_gain;
  // The C ABI exposes only an explicit sample count; a positive value overrides
  // the sample-rate-derived click_seconds default, while 0 selects that default.
  out.click_samples = config.click_samples;
  return out;
}

}  // namespace

SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = 512;
  track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};
  Audio result = corrector.correct_to_midi(audio, track, target_midi);
  return copy_audio_result(result, out, out_length);
  SONARE_C_CATCH
#else
  (void)samples;
  (void)length;
  (void)sample_rate;
  (void)current_midi;
  (void)target_midi;
  (void)out;
  (void)out_length;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

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
  engine->parameter_strings.push_back(fixed_text(info->name, sizeof(info->name)));
  const char* name = engine->parameter_strings.back().c_str();
  engine->parameter_strings.push_back(fixed_text(info->unit, sizeof(info->unit)));
  const char* unit = engine->parameter_strings.back().c_str();
  engine->parameters.add({info->id, name, unit, info->min_value, info->max_value,
                          info->default_value, info->rt_safe != 0,
                          curve_from_int(info->default_curve)});
  return SONARE_OK;
  SONARE_C_CATCH
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
  SONARE_C_TRY
  std::vector<automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    if (!std::isfinite(points[i].ppq)) return SONARE_ERROR_INVALID_PARAMETER;
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
        clip.gain < 0.0f || clip.fade_in_samples < 0 || clip.fade_out_samples < 0) {
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
  const auto state = engine->engine.transport().snapshot();
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
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || options->target_sample_rate <= 0 ||
      options->source_sample_rate <= 0 || options->dither_bits < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *out = {};
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
    const float effective_target_lufs =
        options->target_lufs == 0.0f ? SONARE_DEFAULT_BOUNCE_TARGET_LUFS : options->target_lufs;
    const auto loudness = metering::lufs_interleaved(
        interleaved.data(), channels[0].size(), options->num_channels, options->target_sample_rate);
    if (std::isfinite(loudness.integrated_lufs)) {
      const float gain =
          std::pow(10.0f, (effective_target_lufs - loudness.integrated_lufs) / 20.0f);
      for (auto& sample : interleaved) {
        sample *= gain;
      }
    }
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
      options->start_ppq < 0.0 || options->gain < 0.0f) {
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
  schedule.gain = options->gain == 0.0f ? 1.0f : options->gain;
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
  rt::Command command{};
  command.type = rt::CommandType::kSetParamSmoothed;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const transport::TransportState state = engine->engine.transport().snapshot();
  out->playing = state.playing ? 1 : 0;
  out->looping = state.looping ? 1 : 0;
  out->render_frame = state.render_frame;
  out->sample_position = state.sample_position;
  out->ppq_position = state.ppq_position;
  out->bpm = state.bpm;
  out->loop_start_ppq = state.loop_start_ppq;
  out->loop_end_ppq = state.loop_end_ppq;
  out->sample_rate = state.sample_rate;
  return SONARE_OK;
}

SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, stretch_ratio);
  return copy_audio_result(result, out, out_length);
  SONARE_C_CATCH
#else
  (void)samples;
  (void)length;
  (void)sample_rate;
  (void)onset_sample;
  (void)offset_sample;
  (void)stretch_ratio;
  (void)out;
  (void)out_length;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  editing::voice_changer::VoiceChanger changer(config);
  Audio result = changer.process(audio);
  return copy_audio_result(result, out, out_length);
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(samples, length, sample_rate, pitch_semitones, formant_factor, out, out_length);
#endif
}

#if defined(SONARE_WITH_VOICE_CHANGER)
namespace {

// Single source of truth for the POD ↔ C++ voice changer config bridge.
// X(cpp_path, pod_field) — cpp_path is a dotted C++ member path on the
// RealtimeVoiceChangerConfig struct; pod_field is the flat POD member on the
// SonareRealtimeVoiceChangerConfig struct. Both conversion directions are
// generated by re-expanding the same list, so asymmetric edits (e.g. only
// updating from_pod but forgetting to_pod) are no longer possible.
#define SONARE_VC_FIELDS(X)                               \
  X(input_gain_db, input_gain_db)                         \
  X(output_gain_db, output_gain_db)                       \
  X(wet_mix, wet_mix)                                     \
  X(retune.semitones, retune_semitones)                   \
  X(retune.mix, retune_mix)                               \
  X(retune.grain_size, retune_grain_size)                 \
  X(formant.factor, formant_factor)                       \
  X(formant.amount, formant_amount)                       \
  X(formant.body, formant_body)                           \
  X(formant.brightness, formant_brightness)               \
  X(formant.nasal, formant_nasal)                         \
  X(eq.highpass_hz, eq_highpass_hz)                       \
  X(eq.body_db, eq_body_db)                               \
  X(eq.presence_db, eq_presence_db)                       \
  X(eq.air_db, eq_air_db)                                 \
  X(gate.threshold_db, gate_threshold_db)                 \
  X(gate.attack_ms, gate_attack_ms)                       \
  X(gate.release_ms, gate_release_ms)                     \
  X(gate.range_db, gate_range_db)                         \
  X(compressor.threshold_db, compressor_threshold_db)     \
  X(compressor.ratio, compressor_ratio)                   \
  X(compressor.attack_ms, compressor_attack_ms)           \
  X(compressor.release_ms, compressor_release_ms)         \
  X(compressor.makeup_gain_db, compressor_makeup_gain_db) \
  X(deesser.frequency_hz, deesser_frequency_hz)           \
  X(deesser.threshold_db, deesser_threshold_db)           \
  X(deesser.ratio, deesser_ratio)                         \
  X(deesser.range_db, deesser_range_db)                   \
  X(reverb.mix, reverb_mix)                               \
  X(reverb.time_ms, reverb_time_ms)                       \
  X(reverb.damping, reverb_damping)                       \
  X(reverb.seed, reverb_seed)                             \
  X(limiter.ceiling_db, limiter_ceiling_db)               \
  X(limiter.release_ms, limiter_release_ms)

editing::voice_changer::RealtimeVoiceChangerConfig vc_config_from_pod(
    const SonareRealtimeVoiceChangerConfig& pod) {
  editing::voice_changer::RealtimeVoiceChangerConfig c;
#define X(cpp_path, pod_field) c.cpp_path = pod.pod_field;
  SONARE_VC_FIELDS(X)
#undef X
  return c;
}

void vc_config_to_pod(const editing::voice_changer::RealtimeVoiceChangerConfig& c,
                      SonareRealtimeVoiceChangerConfig* pod) {
#define X(cpp_path, pod_field) pod->pod_field = c.cpp_path;
  SONARE_VC_FIELDS(X)
#undef X
}

/// Single source of truth for the C ↔ C++ preset enum bridge. Adding a new
/// preset is a one-row table edit instead of two switch updates.
struct VcPresetMapping {
  SonareVoiceCharacterPreset c_enum;
  editing::voice_changer::VoiceCharacterPreset cpp_enum;
};

constexpr std::array<VcPresetMapping, 6> kVcPresetMapping = {{
    {SONARE_VC_PRESET_NEUTRAL_MONITOR,
     editing::voice_changer::VoiceCharacterPreset::NeutralMonitor},
    {SONARE_VC_PRESET_BRIGHT_IDOL, editing::voice_changer::VoiceCharacterPreset::BrightIdol},
    {SONARE_VC_PRESET_SOFT_WHISPER, editing::voice_changer::VoiceCharacterPreset::SoftWhisper},
    {SONARE_VC_PRESET_DEEP_NARRATOR, editing::voice_changer::VoiceCharacterPreset::DeepNarrator},
    {SONARE_VC_PRESET_ROBOT_MASCOT, editing::voice_changer::VoiceCharacterPreset::RobotMascot},
    {SONARE_VC_PRESET_DARK_VILLAIN, editing::voice_changer::VoiceCharacterPreset::DarkVillain},
}};

// The table must cover every value of the C++ enum or vc_preset_from_c silently
// falls back to NeutralMonitor for the new preset.
static_assert(
    kVcPresetMapping.size() ==
        static_cast<std::size_t>(editing::voice_changer::VoiceCharacterPreset::DarkVillain) + 1,
    "kVcPresetMapping is out of sync with VoiceCharacterPreset");

editing::voice_changer::VoiceCharacterPreset vc_preset_from_c(SonareVoiceCharacterPreset preset) {
  for (const auto& row : kVcPresetMapping) {
    if (row.c_enum == preset) return row.cpp_enum;
  }
  return editing::voice_changer::VoiceCharacterPreset::NeutralMonitor;
}

}  // namespace
#endif

SonareError sonare_realtime_voice_changer_preset_config(SonareVoiceCharacterPreset preset,
                                                        SonareRealtimeVoiceChangerConfig* out) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const auto config =
      editing::voice_changer::realtime_voice_changer_preset(vc_preset_from_c(preset));
  vc_config_to_pod(config, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(preset, out);
#endif
}

SonareError sonare_realtime_voice_changer_create(const SonareRealtimeVoiceChangerConfig* config,
                                                 int sample_rate, int max_block_size,
                                                 int num_channels,
                                                 SonareRealtimeVoiceChanger** out) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!out || sample_rate <= 0 || max_block_size <= 0 || num_channels < 1 || num_channels > 2) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = nullptr;
  SONARE_C_TRY
  editing::voice_changer::RealtimeVoiceChangerConfig cfg;
  if (config) {
    // Mirror the JSON entry path: reject NaN/Inf early (would propagate into
    // filter coefficients) rather than relying on normalize() alone. Range
    // sanitization still happens inside set_config().
    editing::voice_changer::RealtimeVoiceChangerConfig normalized;
    std::string error;
    if (!editing::voice_changer::validate_realtime_voice_changer_config(vc_config_from_pod(*config),
                                                                        &normalized, &error)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    cfg = normalized;
  } else {
    cfg = editing::voice_changer::realtime_voice_changer_preset(
        editing::voice_changer::VoiceCharacterPreset::NeutralMonitor);
  }
  auto handle = std::make_unique<SonareRealtimeVoiceChanger>();
  handle->changer.set_config(cfg);
  handle->changer.prepare(sample_rate, max_block_size, num_channels);
  handle->max_block_size = max_block_size;
  handle->num_channels = num_channels;
  const size_t scratch_floats =
      static_cast<size_t>(num_channels) * static_cast<size_t>(max_block_size);
  handle->planar_scratch.assign(scratch_floats, 0.0f);
  handle->planar_channel_ptrs.assign(static_cast<size_t>(num_channels), nullptr);
  for (int ch = 0; ch < num_channels; ++ch) {
    handle->planar_channel_ptrs[static_cast<size_t>(ch)] =
        handle->planar_scratch.data() +
        static_cast<size_t>(ch) * static_cast<size_t>(max_block_size);
  }
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(config, sample_rate, max_block_size, num_channels, out);
#endif
}

SonareError sonare_realtime_voice_changer_set_config(
    SonareRealtimeVoiceChanger* handle, const SonareRealtimeVoiceChangerConfig* config) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || !config) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  editing::voice_changer::RealtimeVoiceChangerConfig normalized;
  std::string error;
  if (!editing::voice_changer::validate_realtime_voice_changer_config(vc_config_from_pod(*config),
                                                                      &normalized, &error)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  handle->changer.set_config(normalized);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, config);
#endif
}

SonareError sonare_realtime_voice_changer_get_config(const SonareRealtimeVoiceChanger* handle,
                                                     SonareRealtimeVoiceChangerConfig* out) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  vc_config_to_pod(handle->changer.config(), out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, out);
#endif
}

SonareError sonare_realtime_voice_changer_create_json(const char* preset_or_config_json,
                                                      int sample_rate, int max_block_size,
                                                      int num_channels,
                                                      SonareRealtimeVoiceChanger** out) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!out || sample_rate <= 0 || max_block_size <= 0 || num_channels < 1 || num_channels > 2) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = nullptr;
  SONARE_C_TRY
  const std::string config_text = preset_or_config_json && preset_or_config_json[0] != '\0'
                                      ? preset_or_config_json
                                      : "neutral-monitor";
  auto config = editing::voice_changer::realtime_voice_changer_config_from_json(config_text);
  auto handle = std::make_unique<SonareRealtimeVoiceChanger>();
  handle->changer.set_config(config);
  handle->changer.prepare(sample_rate, max_block_size, num_channels);
  handle->max_block_size = max_block_size;
  handle->num_channels = num_channels;
  // Pre-allocate planar scratch + channel-pointer table for interleaved
  // processing. Realtime-safe: never re-allocated after construction.
  const size_t scratch_floats =
      static_cast<size_t>(num_channels) * static_cast<size_t>(max_block_size);
  handle->planar_scratch.assign(scratch_floats, 0.0f);
  handle->planar_channel_ptrs.assign(static_cast<size_t>(num_channels), nullptr);
  for (int ch = 0; ch < num_channels; ++ch) {
    handle->planar_channel_ptrs[static_cast<size_t>(ch)] =
        handle->planar_scratch.data() +
        static_cast<size_t>(ch) * static_cast<size_t>(max_block_size);
  }
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(preset_or_config_json, sample_rate, max_block_size, num_channels, out);
#endif
}

void sonare_realtime_voice_changer_destroy(SonareRealtimeVoiceChanger* handle) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  delete handle;
#else
  (void)handle;
#endif
}

SonareError sonare_realtime_voice_changer_reset(SonareRealtimeVoiceChanger* handle) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->changer.reset();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle);
#endif
}

SonareError sonare_realtime_voice_changer_set_config_json(SonareRealtimeVoiceChanger* handle,
                                                          const char* preset_or_config_json) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || !preset_or_config_json) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->changer.set_config(
      editing::voice_changer::realtime_voice_changer_config_from_json(preset_or_config_json));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, preset_or_config_json);
#endif
}

SonareError sonare_realtime_voice_changer_process_mono(SonareRealtimeVoiceChanger* handle,
                                                       const float* input, float* output,
                                                       size_t num_samples) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || (!input && num_samples > 0) || (!output && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples > static_cast<size_t>(handle->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  handle->changer.process_block(input, output, static_cast<int>(num_samples));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, input, output, num_samples);
#endif
}

SonareError sonare_realtime_voice_changer_process_interleaved(SonareRealtimeVoiceChanger* handle,
                                                              const float* input, float* output,
                                                              size_t num_frames, int num_channels) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || (!input && num_frames > 0) || (!output && num_frames > 0) || num_channels < 1 ||
      num_channels > handle->num_channels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_frames > static_cast<size_t>(handle->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // RT-safe path: reuse the per-handle planar scratch (allocated in
  // create_json) and pointer table instead of allocating per call.
  const size_t stride = static_cast<size_t>(handle->max_block_size);
  for (int ch = 0; ch < num_channels; ++ch) {
    float* dst = handle->planar_scratch.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < num_frames; ++i) {
      dst[i] = input[i * static_cast<size_t>(num_channels) + static_cast<size_t>(ch)];
    }
  }
  handle->changer.process_block(handle->planar_channel_ptrs.data(), num_channels,
                                static_cast<int>(num_frames));
  for (int ch = 0; ch < num_channels; ++ch) {
    const float* src = handle->planar_scratch.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < num_frames; ++i) {
      output[i * static_cast<size_t>(num_channels) + static_cast<size_t>(ch)] = src[i];
    }
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, input, output, num_frames, num_channels);
#endif
}

SonareError sonare_realtime_voice_changer_process_planar_stereo(SonareRealtimeVoiceChanger* handle,
                                                                float* left, float* right,
                                                                size_t num_frames) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || (!left && num_frames > 0) || (!right && num_frames > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (handle->num_channels < 2) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_frames > static_cast<size_t>(handle->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Planar fast path: no interleave/deinterleave required, just point the
  // changer's process_block at the caller-owned buffers. Realtime-safe.
  float* channels[2] = {left, right};
  handle->changer.process_block(channels, 2, static_cast<int>(num_frames));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, left, right, num_frames);
#endif
}

SonareError sonare_realtime_voice_changer_latency_samples(const SonareRealtimeVoiceChanger* handle,
                                                          int* out_latency_samples) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || !out_latency_samples) return SONARE_ERROR_INVALID_PARAMETER;
  *out_latency_samples = handle->changer.latency_samples();
  return SONARE_OK;
#else
  SONARE_VC_STUB(handle, out_latency_samples);
#endif
}

SonareError sonare_realtime_voice_changer_config_json(const SonareRealtimeVoiceChanger* handle,
                                                      char** out_json) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!handle || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;
  SONARE_C_TRY
  *out_json = copy_c_string(
      editing::voice_changer::realtime_voice_changer_config_to_json(handle->changer.config()));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(handle, out_json);
#endif
}

const char* sonare_realtime_voice_changer_preset_names(void) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  // Backed by the canonical SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS macro
  // so every binding shares a single source of truth.
  return SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS;
#else
  return "";
#endif
}

#if defined(SONARE_WITH_VOICE_CHANGER)
// Keep the C++ ABI constant and the C macro in lock-step. The check lives here
// (the C ABI bridge layer) rather than inside editing/ to preserve the layer
// rule "editing/ must not depend on the public C API header sonare_c.h".
static_assert(editing::voice_changer::kVoiceChangerAbiVersion == SONARE_VOICE_CHANGER_ABI_VERSION,
              "C++ and C ABI version constants drifted");
#endif

uint32_t sonare_voice_changer_abi_version(void) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  return editing::voice_changer::kVoiceChangerAbiVersion;
#else
  // Voice changer not compiled in: report 0 so bindings can detect the absence
  // (every shipped build with the feature is >= 1).
  return 0u;
#endif
}

const char* sonare_voice_character_preset_id(SonareVoiceCharacterPreset preset) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  for (const auto& row : kVcPresetMapping) {
    if (row.c_enum == preset) {
      return editing::voice_changer::realtime_voice_changer_preset_id(row.cpp_enum);
    }
  }
  return nullptr;
#else
  (void)preset;
  return nullptr;
#endif
}

SonareError sonare_realtime_voice_changer_preset_json(const char* name, char** out_json) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!name || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;
  SONARE_C_TRY
  const auto preset = editing::voice_changer::realtime_voice_changer_preset_from_id(name);
  *out_json = copy_c_string(editing::voice_changer::realtime_voice_changer_preset_json(preset));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_VC_STUB(name, out_json);
#endif
}

SonareError sonare_realtime_voice_changer_validate_preset_json(const char* json,
                                                               char** out_normalized_json,
                                                               char** out_error) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  if (!json || !out_normalized_json || !out_error) return SONARE_ERROR_INVALID_PARAMETER;
  *out_normalized_json = nullptr;
  *out_error = nullptr;
  try {
    std::string normalized;
    std::string error;
    if (!editing::voice_changer::validate_realtime_voice_changer_preset_json(json, &normalized,
                                                                             &error)) {
      *out_error = copy_c_string(error.empty() ? "invalid preset JSON" : error);
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    *out_normalized_json = copy_c_string(normalized);
    return SONARE_OK;
  } catch (const std::exception& ex) {
    *out_error = copy_c_string(ex.what());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#else
  SONARE_VC_STUB(json, out_normalized_json, out_error);
#endif
}
