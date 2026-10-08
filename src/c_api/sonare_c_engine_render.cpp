#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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

#if defined(SONARE_WITH_MIXING)
// Each legacy record is a byte prefix of its V2 record, which only appends input peaks.
static_assert(offsetof(SonareMeterTelemetryRecordV2, input_peak_db_l) ==
              sizeof(SonareMeterTelemetryRecord));
static_assert(offsetof(SonareMeterTelemetryRecordWideV2, input_peak_db) ==
              offsetof(SonareMeterTelemetryRecordWide, dropped_records) + sizeof(uint32_t));

SonareMeterTelemetryRecordV2 meter_record_v2(const engine::MeterTelemetryRecord& record) {
  SonareMeterTelemetryRecordV2 out{};
  out.target_id = record.target_id;
  out.render_frame = record.render_frame;
  out.seq = record.seq;
  out.peak_db_l = record.peak_db[0];
  out.peak_db_r = record.peak_db[1];
  out.rms_db_l = record.rms_db[0];
  out.rms_db_r = record.rms_db[1];
  out.true_peak_db_l = record.true_peak_db[0];
  out.true_peak_db_r = record.true_peak_db[1];
  out.max_true_peak_db = record.max_true_peak_db;
  out.correlation = record.correlation;
  out.mono_compat_width = record.mono_compat_width;
  out.momentary_lufs = record.momentary_lufs;
  out.short_term_lufs = record.short_term_lufs;
  out.integrated_lufs = record.integrated_lufs;
  out.gain_reduction_db = record.gain_reduction_db;
  out.dropped_records = record.dropped_records;
  out.input_peak_db_l = record.input_peak_db[0];
  out.input_peak_db_r = record.input_peak_db[1];
  return out;
}

SonareMeterTelemetryRecordWideV2 meter_record_wide_v2(const engine::MeterTelemetryRecord& record) {
  SonareMeterTelemetryRecordWideV2 out{};
  out.target_id = record.target_id;
  out.render_frame = record.render_frame;
  out.seq = record.seq;
  const int planes =
      std::clamp(record.channel_count, 0, static_cast<int>(SONARE_METER_MAX_CHANNELS));
  out.channel_count = planes;
  for (int ch = 0; ch < SONARE_METER_MAX_CHANNELS; ++ch) {
    const size_t index = static_cast<size_t>(ch);
    const bool valid = ch < planes;
    // Unused planes report the dB floor, so a host ignoring channel_count never reads clipping.
    out.peak_db[ch] = valid ? record.peak_db[index] : constants::kFloorDb;
    out.rms_db[ch] = valid ? record.rms_db[index] : constants::kFloorDb;
    out.true_peak_db[ch] = valid ? record.true_peak_db[index] : constants::kFloorDb;
    out.input_peak_db[ch] = valid ? record.input_peak_db[index] : constants::kFloorDb;
  }
  out.max_true_peak_db = record.max_true_peak_db;
  out.correlation = record.correlation;
  out.mono_compat_width = record.mono_compat_width;
  out.momentary_lufs = record.momentary_lufs;
  out.short_term_lufs = record.short_term_lufs;
  out.integrated_lufs = record.integrated_lufs;
  out.gain_reduction_db = record.gain_reduction_db;
  out.dropped_records = record.dropped_records;
  return out;
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
  // A block larger than the prepared one cannot be rendered as asked; refuse it
  // rather than render at the smaller block the engine would clamp to.
  if (block_size > engine->engine.max_block_size()) {
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
  options->target_sample_rate = 0;
  options->source_sample_rate = 0;
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
      options->num_channels <= 0 || options->target_sample_rate < 0 ||
      options->source_sample_rate < 0 || options->dither_bits < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // 0 selects the prepared rate for the source and the source rate for the target.
  // prepare admits only whole-hertz rates, so the prepared rate converts exactly.
  const int source_sample_rate = options->source_sample_rate == 0
                                     ? static_cast<int>(engine->engine.sample_rate())
                                     : options->source_sample_rate;
  const int target_sample_rate =
      options->target_sample_rate == 0 ? source_sample_rate : options->target_sample_rate;
  if (source_sample_rate <= 0 ||
      !resource::engine_bounce_shape_fits(options->total_frames, options->num_channels,
                                          source_sample_rate, target_sample_rate,
                                          options->dither != 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Reject an out-of-range dither type instead of silently mapping it to None,
  // which would return SONARE_OK with undithered audio and no way to tell that
  // the request was ignored (same policy as the analysis options' chroma_method).
  if (options->dither < 0 || options->dither > 3) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Every option-derived refusal precedes the render, which consumes engine state.
  if (options->dither != 0) {
#if defined(SONARE_WITH_MASTERING)
    if (options->dither_bits != 0 &&
        (options->dither_bits < mastering::final::kMinDitherTargetBits ||
         options->dither_bits > mastering::final::kMaxDitherTargetBits)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
#else
    return SONARE_ERROR_NOT_SUPPORTED;
#endif
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
  if (options->num_channels > engine->engine.prepared_channels() ||
      options->block_size > engine->engine.max_block_size()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // The render runs at the prepared rate, which an explicit source_sample_rate must name exactly.
  if (options->source_sample_rate > 0 &&
      engine->engine.sample_rate() != static_cast<double>(options->source_sample_rate)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Offline pre-roll: a bounce is a one-shot render, so the first audible block
  // must open at the settled fader/pan/gate values instead of ramping in from
  // the defaults the way a live stream legitimately does (see
  // RealtimeEngine::prime_offline_parameters, and the same pre-roll in the
  // project bounce path).
  engine->engine.prime_offline_parameters(options->num_channels, options->block_size);
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

  channels = resample_channels(channels, source_sample_rate, target_sample_rate);
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
                                            target_sample_rate, effective_target_lufs);
  }
#if defined(SONARE_WITH_MASTERING)
  if (options->dither != 0) {
    mastering::final::DitherConfig config{};
    config.type = dither_type_from_int(options->dither);
    config.target_bits = options->dither_bits > 0 ? options->dither_bits : 16;
    config.seed = options->dither_seed == 0 ? config.seed : options->dither_seed;
    Audio dithered = mastering::final::dither_interleaved(
        Audio::from_buffer(interleaved.data(), interleaved.size(), target_sample_rate),
        static_cast<size_t>(options->num_channels), config);
    interleaved.assign(dithered.data(), dithered.data() + dithered.size());
  }
#endif
  const auto loudness = metering::lufs_interleaved(interleaved.data(), channels[0].size(),
                                                   options->num_channels, target_sample_rate);
  out->sample_count = interleaved.size();
  out->frames = static_cast<int64_t>(channels[0].size());
  out->num_channels = options->num_channels;
  out->sample_rate = target_sample_rate;
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
  if (options->num_channels > engine->engine.prepared_channels() ||
      options->block_size > engine->engine.max_block_size()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Offline pre-roll, as in sonare_engine_bounce_offline: a freeze captures the
  // lane at its settled values, so the frozen clip does not carry a fade-in that
  // the live lane never had.
  engine->engine.prime_offline_parameters(options->num_channels, options->block_size);
  out->clip_id =
      engine->engine
          .freeze_offline(options->num_channels, options->total_frames, options->block_size,
                          options->clip_id, options->start_ppq, options->gain)
          .id;
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
    const SonareMeterTelemetryRecordV2 full = meter_record_v2(record);
    std::memcpy(&out[count], &full, sizeof(SonareMeterTelemetryRecord));
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
    const SonareMeterTelemetryRecordWideV2 full = meter_record_wide_v2(record);
    std::memcpy(&out[count], &full, offsetof(SonareMeterTelemetryRecordWideV2, input_peak_db));
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_drain_meter_telemetry_v2(SonareRealtimeEngine* engine,
                                                   SonareMeterTelemetryRecordV2* out,
                                                   size_t max_records, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::MeterTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_meter_telemetry(record)) {
    out[count] = meter_record_v2(record);
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_drain_meter_telemetry_wide_v2(SonareRealtimeEngine* engine,
                                                        SonareMeterTelemetryRecordWideV2* out,
                                                        size_t max_records, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::MeterTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_meter_telemetry(record)) {
    out[count] = meter_record_wide_v2(record);
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_meter_target_insert_gain_reduction(SonareRealtimeEngine* engine,
                                                             uint32_t target_id, float* out_db,
                                                             size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
  if (!engine || !out_count || (capacity > 0 && !out_db)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  static_assert(sonare::engine::InsertGainReductionBoard::kCapacity == SONARE_METER_MAX_INSERTS,
                "the board holds exactly the documented insert bound");
  static_assert(2 * sonare::mixing::ChannelStrip::kMaxInserts <= SONARE_METER_MAX_INSERTS,
                "a strip's pre- plus post-fader inserts fit the documented bound");
  if (!engine->engine.read_meter_target_insert_gain_reduction(target_id, out_db, capacity,
                                                              out_count)) {
    *out_count = 0;
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
#else
  (void)target_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_configure_scope_telemetry(SonareRealtimeEngine* engine,
                                                    int interval_frames, unsigned int band_count,
                                                    unsigned int* out_band_count) {
  SONARE_C_API_ENTRY;
  if (out_band_count) *out_band_count = 0;
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
