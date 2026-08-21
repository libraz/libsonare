#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/audio.h"
#include "core/channel_layout.h"
#include "core/resample.h"
#include "engine/realtime_engine.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#include "sonare_c_internal.h"
#include "util/constants.h"
#include "util/resource_limits.h"
#if defined(SONARE_WITH_MASTERING)
#include "mastering/final/dither.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

namespace {

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

}  // namespace

SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames) {
  SONARE_C_RT_API_ENTRY;
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process(channels, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames) {
  SONARE_C_RT_API_ENTRY;
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process_with_monitor(channels, monitor_out, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_render_offline_ex(SonareRealtimeEngine* engine, float* const* out,
                                            int num_channels, int64_t total_frames, int block_size,
                                            int finalize) {
  SONARE_C_API_ENTRY;
  if (!engine || !out || num_channels <= 0 || total_frames < 0 || block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // render_offline on a never-prepared engine renders nothing and cannot even
  // signal through telemetry (the ring is unreserved until prepare()). Report it
  // synchronously so the caller does not mistake a silent buffer for a render.
  if (engine->engine.max_block_size() <= 0) {
    return SONARE_ERROR_INVALID_STATE;
  }
  // sonare_engine_prepare_with_channels bounds capture/instrument/PDC/monitor
  // scratch to prepared_channels(); rendering more planes than that would
  // silently write zeros to every plane past the bound instead of erroring, so
  // the host reads a "successful" render that is actually silence for the
  // channels it asked for beyond the prepared maximum.
  if (num_channels > engine->engine.prepared_channels()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.render_offline(out, num_channels, total_frames, block_size, finalize != 0);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size) {
  return sonare_engine_render_offline_ex(engine, out, num_channels, total_frames, block_size, 1);
}

SonareError sonare_engine_finish_offline_render(SonareRealtimeEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  // Same never-prepared rule as the render entry points: with no reserved
  // telemetry ring there is nothing to release and nothing that could report,
  // so say so rather than returning OK for a no-op.
  if (engine->engine.max_block_size() <= 0) {
    return SONARE_ERROR_INVALID_STATE;
  }
  engine->engine.finish_offline_render();
  return SONARE_OK;
}

SonareError sonare_engine_bounce_options_default(SonareEngineBounceOptions* options) {
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
  // Zero the owned out-pointer/lengths BEFORE any validation early-return so a
  // failed validation always leaves a NULL owned pointer (matching the analysis
  // wrappers in sonare_c.cpp). Otherwise the standard
  // sonare_free_bounce_result(&r) idiom would delete[] an uninitialised pointer.
  if (out) {
    *out = {};
  }
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || options->target_sample_rate <= 0 ||
      options->source_sample_rate <= 0 || options->dither_bits < 0 ||
      !resource::engine_bounce_shape_fits(options->total_frames, options->num_channels,
                                          options->source_sample_rate,
                                          options->target_sample_rate)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Reject an out-of-range dither type instead of silently mapping it to None,
  // which would return SONARE_OK with undithered audio and no way to tell that
  // the request was ignored (same policy as the analysis options' chroma_method).
  if (options->dither < 0 || options->dither > 3) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // The bounce width must map to a supported speaker layout (1 mono, 2 stereo,
  // 6 = 5.1, 8 = 7.1). Counts like 3/4/5/7 have no layout and would silently
  // leave their extra planes unpanned, so reject them up front.
  if (sonare::channel_count(sonare::layout_from_channel_count(options->num_channels)) !=
      options->num_channels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // A never-prepared engine renders only silence and cannot report through the
  // (unreserved) telemetry ring, so fail closed instead of returning a bounce
  // result the caller would read as a valid silent render.
  if (engine->engine.max_block_size() <= 0) {
    return SONARE_ERROR_INVALID_STATE;
  }
  // See sonare_engine_render_offline: bouncing more channels than the engine
  // was prepared for would silently write zeros for every plane past the
  // bound instead of erroring.
  if (options->num_channels > engine->engine.prepared_channels()) {
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
  SONARE_C_API_ENTRY;
  if (out) *out = {};
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || !std::isfinite(options->start_ppq) ||
      options->start_ppq < 0.0 || !(std::isfinite(options->gain) && options->gain >= 0.0f) ||
      !resource::engine_offline_shape_fits(options->total_frames, options->num_channels, 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Freezing a never-prepared engine would capture pure silence with no error
  // channel (telemetry is unreserved before prepare()); fail closed instead.
  if (engine->engine.max_block_size() <= 0) {
    return SONARE_ERROR_INVALID_STATE;
  }
  // See sonare_engine_render_offline: freezing more channels than the engine
  // was prepared for would silently write zeros for every plane past the
  // bound instead of erroring.
  if (options->num_channels > engine->engine.prepared_channels()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
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
  SONARE_C_API_ENTRY;
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
  SONARE_C_API_ENTRY;
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

SonareError sonare_engine_drain_meter_telemetry_wide(SonareRealtimeEngine* engine,
                                                     SonareMeterTelemetryRecordWide* out,
                                                     size_t max_records, size_t* out_count) {
  SONARE_C_API_ENTRY;
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
    const int planes =
        std::clamp(record.channel_count, 0, static_cast<int>(SONARE_METER_MAX_CHANNELS));
    out[count].channel_count = planes;
    for (int ch = 0; ch < SONARE_METER_MAX_CHANNELS; ++ch) {
      const bool valid = ch < planes;
      // Unused surround planes report the dB floor (silence), not 0 dBFS: a host
      // that ignores channel_count and reads every plane must not see the unused
      // planes pinned to full scale (which would read as clipping).
      out[count].peak_db[ch] =
          valid ? record.peak_db[static_cast<size_t>(ch)] : constants::kFloorDb;
      out[count].rms_db[ch] = valid ? record.rms_db[static_cast<size_t>(ch)] : constants::kFloorDb;
      out[count].true_peak_db[ch] =
          valid ? record.true_peak_db[static_cast<size_t>(ch)] : constants::kFloorDb;
    }
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

SonareError sonare_engine_configure_scope_telemetry(SonareRealtimeEngine* engine,
                                                    int interval_frames, unsigned int band_count,
                                                    unsigned int* out_band_count) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_MIXING)
  SONARE_C_TRY
  const uint32_t applied =
      engine->engine.configure_scope_telemetry(interval_frames, static_cast<uint32_t>(band_count));
  if (out_band_count) *out_band_count = applied;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  (void)interval_frames;
  (void)band_count;
  if (out_band_count) *out_band_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_drain_scope_telemetry(SonareRealtimeEngine* engine,
                                                SonareScopeTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::ScopeTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_scope_telemetry(record)) {
    out[count].target_id = record.target_id;
    out[count].render_frame = record.render_frame;
    out[count].seq = record.seq;
    out[count].dropped_records = record.dropped_records;
    out[count].band_count = record.band_count;
    for (uint32_t b = 0; b < record.band_count && b < SONARE_SCOPE_MAX_BANDS; ++b) {
      out[count].bands[b] = record.bands[b];
    }
    out[count].point_count = record.point_count;
    for (uint32_t p = 0; p < record.point_count && p < SONARE_SCOPE_MAX_POINTS; ++p) {
      out[count].points[2 * p] = record.points[p].left;
      out[count].points[2 * p + 1] = record.points[p].right;
    }
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
