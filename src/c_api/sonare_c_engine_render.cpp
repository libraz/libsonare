#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/channel_layout.h"
#include "core/resample.h"
#include "engine/realtime_engine.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
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
static_assert(static_cast<int>(mastering::final::DitherType::NoiseShaped) == 3,
              "SonareEngineBounceOptions::dither ordinals must mirror DitherType");

// For an ordinal the option check has already admitted.
mastering::final::DitherType dither_type_from_int(int value) {
  return admitted_ordinal_to_enum<mastering::final::DitherType>(value);
}
#endif

#if defined(SONARE_WITH_MIXING)
SonareMeterTelemetryRecord meter_record(const engine::MeterTelemetryRecord& record) {
  SonareMeterTelemetryRecord out{};
  out.target_id = record.target_id;
  out.render_frame = record.render_frame;
  out.seq = record.seq;
  const int planes =
      std::clamp(record.channel_count, 0, static_cast<int>(SONARE_METER_MAX_CHANNELS));
  out.num_channels = planes;
  for (int ch = 0; ch < SONARE_METER_MAX_CHANNELS; ++ch) {
    const size_t index = static_cast<size_t>(ch);
    const bool valid = ch < planes;
    // Unused planes report the dB floor, so a host ignoring channel_count never reads clipping.
    out.peak_db[ch] = valid ? record.peak_db[index] : constants::kFloorDb;
    out.rms_db[ch] = valid ? record.rms_db[index] : constants::kFloorDb;
    out.true_peak_dbtp[ch] = valid ? record.true_peak_db[index] : constants::kFloorDb;
    out.input_peak_db[ch] = valid ? record.input_peak_db[index] : constants::kFloorDb;
  }
  out.max_true_peak_dbtp = record.max_true_peak_db;
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

SonareError sonare_engine_process(SonareEngine* engine, float* const* channels,
                                  float* const* monitor_out, int num_channels, int num_frames) {
  SONARE_C_RT_API_ENTRY;
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (monitor_out == nullptr) {
    engine->engine.process(channels, num_channels, num_frames);
  } else {
    engine->engine.process_with_monitor(channels, monitor_out, num_channels, num_frames);
  }
  return SONARE_OK;
}

void sonare_engine_render_offline_options_init(SonareEngineRenderOfflineOptions* options) {
  if (options == nullptr) return;
  *options = SonareEngineRenderOfflineOptions{};
  options->struct_size = sizeof(SonareEngineRenderOfflineOptions);
  options->finalize = 1;
}

SonareError sonare_engine_render_offline(SonareEngine* engine, float* const* out, int num_channels,
                                         int64_t total_frames, int block_size,
                                         const SonareEngineRenderOfflineOptions* options) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
  int finalize = 1;
  if (options != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(options->struct_size, sizeof(SonareEngineRenderOfflineOptions),
                              "options", "sonare_engine_render_offline_options_init");
        });
        err != SONARE_OK) {
      return err;
    }
    finalize = options->finalize;
  }
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(num_channels, kEngineRenderChannelsDomain, "num_channels");
      });
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(total_frames, kEngineRenderFramesDomain, "total_frames");
      });
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(block_size, kEngineRenderBlockSizeDomain, "block_size");
      });
      err != SONARE_OK) {
    return err;
  }
  // render_offline on a never-prepared engine renders nothing and cannot even
  // signal through telemetry (the ring is unreserved until prepare()). Report it
  // synchronously so the caller does not mistake a silent buffer for a render.
  if (engine->engine.max_block_size() <= 0) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the engine has not been prepared");
    return SONARE_ERROR_INVALID_STATE;
  }
  block_size = sonare_c_engine_detail::resolve_offline_block(engine->engine, block_size);
  // sonare_engine_prepare bounds capture/instrument/PDC/monitor
  // scratch to prepared_channels(); rendering more planes than that would
  // silently write zeros to every plane past the bound instead of erroring, so
  // the host reads a "successful" render that is actually silence for the
  // channels it asked for beyond the prepared maximum.
  if (num_channels > engine->engine.prepared_channels()) {
    return run_validation([&] {
      util::check::integer_in_range(num_channels, 1, engine->engine.prepared_channels(),
                                    "num_channels");
    });
  }
  // A block larger than the prepared one cannot be rendered as asked; refuse it
  // rather than render at the smaller block the engine would clamp to.
  if (block_size > engine->engine.max_block_size()) {
    return run_validation([&] {
      util::check::integer_in_range(block_size, 1, engine->engine.max_block_size(), "block_size");
    });
  }
  engine->engine.render_offline(out, num_channels, total_frames, block_size, finalize != 0);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_finish_offline_render(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  // Same never-prepared rule as the render entry points: with no reserved
  // telemetry ring there is nothing to release and nothing that could report,
  // so say so rather than returning OK for a no-op.
  if (engine->engine.max_block_size() <= 0) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the engine has not been prepared");
    return SONARE_ERROR_INVALID_STATE;
  }
  engine->engine.finish_offline_render();
  return SONARE_OK;
}

#if defined(SONARE_WITH_MASTERING)
static_assert(SONARE_ENGINE_DEFAULT_DITHER_SEED == mastering::final::DitherConfig{}.seed,
              "SONARE_ENGINE_DEFAULT_DITHER_SEED must name the dither's built-in seed");
#endif

void sonare_engine_bounce_options_init(SonareEngineBounceOptions* options) {
  if (options == nullptr) return;
  *options = SonareEngineBounceOptions{};
  options->struct_size = sizeof(SonareEngineBounceOptions);
  options->block_size = SONARE_ENGINE_BLOCK_SIZE_AUTO;
  options->target_sample_rate = SONARE_ENGINE_SAMPLE_RATE_PREPARED;
  options->source_sample_rate = SONARE_ENGINE_SAMPLE_RATE_PREPARED;
  options->target_lufs = std::numeric_limits<float>::quiet_NaN();
  options->dither_bits = 16;
  options->dither_seed = SONARE_ENGINE_DEFAULT_DITHER_SEED;
}

void sonare_engine_freeze_options_init(SonareEngineFreezeOptions* options) {
  if (options == nullptr) return;
  *options = SonareEngineFreezeOptions{};
  options->struct_size = sizeof(SonareEngineFreezeOptions);
  options->block_size = SONARE_ENGINE_BLOCK_SIZE_AUTO;
  options->gain = 1.0f;
}

SonareError sonare_engine_bounce_offline(SonareEngine* engine,
                                         const SonareEngineBounceOptions* options,
                                         SonareEngineBounceResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  // A failed call leaves a NULL owned pointer, so sonare_free_engine_bounce_result(&r) is safe.
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(options);
  if (const SonareError err = run_validation([&] {
        require_struct_size(options->struct_size, sizeof(SonareEngineBounceOptions), "options",
                            "sonare_engine_bounce_options_init");
        util::check::integer_within(options->total_frames, kEngineBounceFramesDomain,
                                    "options.total_frames");
        sonare_c_engine_detail::check_offline_block(options->block_size, "options.block_size");
        util::check::integer_within(options->num_channels, kEngineRenderChannelsDomain,
                                    "options.num_channels");
      });
      err != SONARE_OK) {
    return err;
  }
  // The bounce width must map to a supported speaker layout (1 mono, 2 stereo,
  // 6 = 5.1, 8 = 7.1). Counts like 3/4/5/7 have no layout and would silently
  // leave their extra planes unpanned, so reject them up front.
  if (sonare::channel_count(sonare::layout_from_channel_count(options->num_channels)) !=
      options->num_channels) {
    return run_validation([&] {
      util::check::refuse("options.num_channels",
                          util::refusal::one_of_predicate(std::array<int, 4>{1, 2, 6, 8}));
    });
  }
  if (const SonareError err = run_validation([&] {
        sonare_c_engine_detail::check_engine_rate(options->target_sample_rate,
                                                  "options.target_sample_rate");
        sonare_c_engine_detail::check_engine_rate(options->source_sample_rate,
                                                  "options.source_sample_rate");
        if (!std::isnan(options->target_lufs)) {
          util::check::finite(options->target_lufs, "options.target_lufs");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  // Reject an out-of-range dither type instead of silently mapping it to None,
  // which would return SONARE_OK with undithered audio and no way to tell that
  // the request was ignored (same policy as the analysis options' chroma_method).
  if (!valid_c_enum(options->dither, 0, 3)) {
    return run_validation([&] {
      util::check::one_of(options->dither, std::array<int, 4>{0, 1, 2, 3}, "options.dither");
    });
  }
  if (options->dither != 0) {
#if defined(SONARE_WITH_MASTERING)
    if (const SonareError err = run_validation([&] {
          util::check::integer_within(options->dither_bits, kEngineDitherBitsDomain,
                                      "options.dither_bits");
        });
        err != SONARE_OK) {
      return err;
    }
#else
    set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                   "options.dither is not supported because mastering support is not "
                   "compiled in");
    return SONARE_ERROR_NOT_SUPPORTED;
#endif
  }
  // prepare admits only whole-hertz rates, so the prepared rate converts exactly.
  const int source_sample_rate = options->source_sample_rate == SONARE_ENGINE_SAMPLE_RATE_PREPARED
                                     ? static_cast<int>(engine->engine.sample_rate())
                                     : options->source_sample_rate;
  const int target_sample_rate = options->target_sample_rate == SONARE_ENGINE_SAMPLE_RATE_PREPARED
                                     ? source_sample_rate
                                     : options->target_sample_rate;
  if (source_sample_rate <= 0) {
    return run_validation([&] {
      util::check::refuse("options.source_sample_rate",
                          "must name a rate when the engine has no prepared sample rate");
    });
  }
  // Every option-derived refusal precedes the render, which consumes engine state.
  if (!resource::engine_bounce_shape_fits(options->total_frames, options->num_channels,
                                          source_sample_rate, target_sample_rate,
                                          options->dither != 0)) {
    return run_validation([&] {
      util::check::refuse_relation(
          {"options.total_frames", "options.num_channels", "options.source_sample_rate",
           "options.target_sample_rate"},
          "{0}, {1}, {2} and {3} give a result larger than the supported size");
    });
  }
  SONARE_C_REQUIRE_NONNULL(out);
  // A never-prepared engine renders only silence and cannot report through the
  // (unreserved) telemetry ring, so fail closed instead of returning a bounce
  // result the caller would read as a valid silent render.
  if (engine->engine.max_block_size() <= 0) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the engine has not been prepared");
    return SONARE_ERROR_INVALID_STATE;
  }
  // See sonare_engine_render_offline: bouncing more channels than the engine
  // was prepared for would silently write zeros for every plane past the
  // bound instead of erroring.
  const int block_size = sonare_c_engine_detail::resolve_offline_block(
      engine->engine, sonare_c_engine_detail::offline_block_from_c(options->block_size));
  if (options->num_channels > engine->engine.prepared_channels()) {
    return run_validation([&] {
      util::check::integer_in_range(options->num_channels, 1, engine->engine.prepared_channels(),
                                    "options.num_channels");
    });
  }
  if (block_size > engine->engine.max_block_size()) {
    return run_validation([&] {
      util::check::integer_in_range(block_size, 1, engine->engine.max_block_size(),
                                    "options.block_size");
    });
  }
  // The render runs at the prepared rate, which an explicit source_sample_rate must name exactly.
  if (options->source_sample_rate != SONARE_ENGINE_SAMPLE_RATE_PREPARED &&
      engine->engine.sample_rate() != static_cast<double>(options->source_sample_rate)) {
    return run_validation([&] {
      util::check::refuse("options.source_sample_rate",
                          "must equal the prepared sample rate " +
                              std::to_string(static_cast<long long>(engine->engine.sample_rate())));
    });
  }
  // Offline pre-roll: a bounce is a one-shot render, so the first audible block
  // must open at the settled fader/pan/gate values instead of ramping in from
  // the defaults the way a live stream legitimately does (see
  // RealtimeEngine::prime_offline_parameters, and the same pre-roll in the
  // project bounce path).
  engine->engine.prime_offline_parameters(options->num_channels, block_size);
  std::vector<std::vector<float>> channels = engine->engine.render_offline_aligned(
      options->num_channels, options->total_frames, block_size);

  channels = resample_channels(channels, source_sample_rate, target_sample_rate);
  std::vector<float> interleaved = interleave_channels(channels);
  if (options->normalize_lufs) {
    static_assert(SONARE_DEFAULT_BOUNCE_TARGET_LUFS == -14.0f,
                  "SONARE_DEFAULT_BOUNCE_TARGET_LUFS must match the WASM/Node "
                  "facade default to keep cross-binding bounce behaviour identical");
    const float effective_target_lufs =
        std::isnan(options->target_lufs) ? SONARE_DEFAULT_BOUNCE_TARGET_LUFS : options->target_lufs;
    metering::normalize_interleaved_to_lufs(interleaved, channels[0].size(), options->num_channels,
                                            target_sample_rate, effective_target_lufs);
  }
#if defined(SONARE_WITH_MASTERING)
  if (options->dither != 0) {
    mastering::final::DitherConfig config{};
    config.type = dither_type_from_int(options->dither);
    config.target_bits = options->dither_bits;
    config.seed = options->dither_seed;
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

SonareError sonare_engine_freeze_offline(SonareEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(options);
  if (const SonareError err = run_validation([&] {
        require_struct_size(options->struct_size, sizeof(SonareEngineFreezeOptions), "options",
                            "sonare_engine_freeze_options_init");
        util::check::integer_within(options->total_frames, kEngineBounceFramesDomain,
                                    "options.total_frames");
        sonare_c_engine_detail::check_offline_block(options->block_size, "options.block_size");
        util::check::integer_within(options->num_channels, kEngineRenderChannelsDomain,
                                    "options.num_channels");
      });
      err != SONARE_OK) {
    return err;
  }
  if (!std::isfinite(options->start_ppq) || options->start_ppq < 0.0) {
    return run_validation(
        [&] { util::check::at_least(options->start_ppq, 0.0, "options.start_ppq"); });
  }
  if (!(std::isfinite(options->gain) && options->gain >= 0.0f)) {
    return run_validation([&] { util::check::at_least(options->gain, 0.0, "options.gain"); });
  }
  if (!resource::engine_offline_shape_fits(options->total_frames, options->num_channels, 1)) {
    return run_validation([&] {
      util::check::refuse_relation({"options.total_frames", "options.num_channels"},
                                   "{0} times {1} exceeds the supported size");
    });
  }
  SONARE_C_REQUIRE_NONNULL(out);
  // Freezing a never-prepared engine would capture pure silence with no error
  // channel (telemetry is unreserved before prepare()); fail closed instead.
  if (engine->engine.max_block_size() <= 0) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the engine has not been prepared");
    return SONARE_ERROR_INVALID_STATE;
  }
  // See sonare_engine_render_offline: freezing more channels than the engine
  // was prepared for would silently write zeros for every plane past the
  // bound instead of erroring.
  const int block_size = sonare_c_engine_detail::resolve_offline_block(
      engine->engine, sonare_c_engine_detail::offline_block_from_c(options->block_size));
  if (options->num_channels > engine->engine.prepared_channels()) {
    return run_validation([&] {
      util::check::integer_in_range(options->num_channels, 1, engine->engine.prepared_channels(),
                                    "options.num_channels");
    });
  }
  if (block_size > engine->engine.max_block_size()) {
    return run_validation([&] {
      util::check::integer_in_range(block_size, 1, engine->engine.max_block_size(),
                                    "options.block_size");
    });
  }
  // Offline pre-roll, as in sonare_engine_bounce_offline: a freeze captures the
  // lane at its settled values, so the frozen clip does not carry a fade-in that
  // the live lane never had.
  engine->engine.prime_offline_parameters(options->num_channels, block_size);
  out->clip_id = engine->engine
                     .freeze_offline(options->num_channels, options->total_frames, block_size,
                                     options->clip_id, options->start_ppq, options->gain)
                     .id;
  out->frames = options->total_frames;
  out->num_channels = options->num_channels;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_drain_telemetry(SonareEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(written);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out, max_records);
  SONARE_C_REQUIRE_NONNULL(written);

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

SonareError sonare_engine_drain_meter_telemetry(SonareEngine* engine,
                                                SonareMeterTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out, max_records);
  SONARE_C_REQUIRE_NONNULL(out_count);

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::MeterTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_meter_telemetry(record)) {
    out[count] = meter_record(record);
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_meter_target_insert_gain_reduction(SonareEngine* engine,
                                                             uint32_t target_id, float* out_db,
                                                             size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out_db, capacity);
  SONARE_C_REQUIRE_NONNULL(out_count);

#if defined(SONARE_WITH_MIXING)
  static_assert(sonare::engine::InsertGainReductionBoard::kCapacity == SONARE_METER_MAX_INSERTS,
                "the board holds exactly the documented insert bound");
  static_assert(2 * sonare::mixing::ChannelStrip::kMaxInserts <= SONARE_METER_MAX_INSERTS,
                "a strip's pre- plus post-fader inserts fit the documented bound");
  if (!engine->engine.read_meter_target_insert_gain_reduction(target_id, out_db, capacity,
                                                              out_count)) {
    *out_count = 0;
    return run_validation(
        [&] { util::check::refuse_unknown_id("target_id", "a meter target", "engine"); });
  }
  return SONARE_OK;
#else
  (void)target_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
  SONARE_C_CATCH
}

SonareError sonare_engine_configure_scope_telemetry(SonareEngine* engine, int interval_frames,
                                                    size_t band_count, size_t* out_band_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_band_count);
  SONARE_C_REQUIRE_NONNULL(engine);
#if defined(SONARE_WITH_MIXING)
  SONARE_C_TRY
  const uint32_t applied = engine->engine.configure_scope_telemetry(
      interval_frames, static_cast<uint32_t>(std::min<size_t>(band_count, UINT32_MAX)));
  if (out_band_count) *out_band_count = applied;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  (void)interval_frames;
  (void)band_count;
  if (out_band_count) *out_band_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_drain_scope_telemetry(SonareEngine* engine,
                                                SonareScopeTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out, max_records);
  SONARE_C_REQUIRE_NONNULL(out_count);

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
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
