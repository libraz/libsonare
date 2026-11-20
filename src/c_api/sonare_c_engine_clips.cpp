#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/tempo_sync.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_engine_detail;

namespace {

bool rounded_nonnegative_sample(double sample, size_t* out) noexcept {
  if (!out || !std::isfinite(sample) || sample < 0.0) return false;
  return numeric::checked_round_cast(sample, out);
}

/// @p target_length is the clip's resolved length: length_samples, or the rest
/// of the source when that is 0.
bool tempo_sync_segments_for_clip(const SonareEngineClip& clip, int64_t target_length,
                                  std::vector<engine::TempoSyncWarpSegment>* out) {
  if (!out || target_length <= 0 || clip.clip_offset_samples < 0 ||
      clip.clip_offset_samples >= clip.num_samples) {
    return false;
  }
  out->clear();
  if (static_cast<uint64_t>(clip.clip_offset_samples) >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  const size_t base_offset = static_cast<size_t>(clip.clip_offset_samples);
  if (clip.warp_anchor_count >= 2 && clip.warp_anchors) {
    out->reserve(clip.warp_anchor_count - 1);
    for (size_t anchor_index = 1; anchor_index < clip.warp_anchor_count; ++anchor_index) {
      const auto& prev = clip.warp_anchors[anchor_index - 1];
      const auto& next = clip.warp_anchors[anchor_index];
      size_t source_start = 0;
      size_t source_end = 0;
      size_t target_start = 0;
      size_t target_end = 0;
      if (!rounded_nonnegative_sample(prev.source_sample, &source_start) ||
          !rounded_nonnegative_sample(next.source_sample, &source_end) ||
          !rounded_nonnegative_sample(prev.warp_sample, &target_start) ||
          !rounded_nonnegative_sample(next.warp_sample, &target_end)) {
        return false;
      }
      engine::TempoSyncWarpSegment segment;
      if (!numeric::checked_add(base_offset, source_start, &segment.source_offset)) return false;
      segment.source_samples = source_end > source_start ? source_end - source_start : 0;
      segment.target_samples = target_end > target_start ? target_end - target_start : 0;
      if (segment.source_offset > static_cast<size_t>(clip.num_samples) ||
          segment.source_samples > static_cast<size_t>(clip.num_samples) - segment.source_offset ||
          segment.source_samples == 0 || segment.target_samples == 0) {
        return false;
      }
      out->push_back(segment);
    }
    return !out->empty();
  }
  engine::TempoSyncWarpSegment segment;
  segment.source_offset = base_offset;
  segment.source_samples = static_cast<size_t>(clip.num_samples) - base_offset;
  segment.target_samples = static_cast<size_t>(target_length);
  if (segment.source_samples == 0 || segment.target_samples == 0) return false;
  out->push_back(segment);
  return true;
}

}  // namespace

namespace {

// Refuses a clip the schedule cannot be built from; throws a refusal naming the field.
void check_clip(const SonareEngineClip& clip, const std::string& prefix) {
  const auto subject = [&](const char* member) { return prefix + member; };
  const bool paged = clip.page_provider != nullptr;
  if (paged && !clip.page_provider->provider) {
    util::check::refuse(subject("page_provider").c_str(), "must hold a page provider");
  }
  const int source_channels =
      paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
  const int64_t source_samples =
      paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
  if (!paged) util::check::non_null(clip.channels, subject("channels").c_str());
  util::check::integer_at_least(source_channels, 1, subject("num_channels").c_str());
  util::check::integer_at_least<int64_t>(source_samples, 1, subject("num_samples").c_str());
  util::check::in_range(clip.start_ppq, 0.0, transport::kMaxPublicPpq,
                        subject("start_ppq").c_str());
  util::check::integer_in_range(clip.clip_offset_samples, 0, source_samples - 1,
                                subject("clip_offset_samples").c_str());
  util::check::at_least(clip.gain, 0.0, subject("gain").c_str());
  util::check::integer_at_least<int64_t>(clip.fade_in_samples, 0,
                                         subject("fade_in_samples").c_str());
  util::check::integer_at_least<int64_t>(clip.fade_out_samples, 0,
                                         subject("fade_out_samples").c_str());
  util::check::one_of(clip.warp_mode, std::array<int, 4>{0, 1, 2, 3}, subject("warp_mode").c_str());
  if (paged && clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) {
    util::check::refuse_relation({subject("warp_mode"), subject("page_provider")},
                                 "{1} must be NULL when {0} is tempo sync");
  }
  if (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC && clip.loop != 0) {
    util::check::refuse_relation({subject("loop"), subject("warp_mode")},
                                 "{0} must be 0 when {1} is tempo sync");
  }
  if ((clip.warp_mode == SONARE_ENGINE_WARP_MODE_REPITCH ||
       clip.warp_mode == SONARE_ENGINE_WARP_MODE_TIME_STRETCH) &&
      clip.loop != 0 && clip.warp_anchor_count >= 2) {
    util::check::refuse_relation(
        {subject("loop"), subject("warp_anchor_count")},
        "{0} must be 0 for a repitch or time-stretch clip when {1} is at least 2");
  }
  util::check::non_null_when_count(clip.warp_anchors, clip.warp_anchor_count,
                                   subject("warp_anchors").c_str(),
                                   subject("warp_anchor_count").c_str());
  // Only 0 asks for the source length; any other non-positive length is refused
  // rather than being read as that request.
  util::check::integer_at_least<int64_t>(clip.length_samples, 0, subject("length_samples").c_str());
  for (size_t anchor_index = 0; anchor_index < clip.warp_anchor_count; ++anchor_index) {
    const SonareEngineWarpAnchor& anchor = clip.warp_anchors[anchor_index];
    const std::string anchor_prefix =
        prefix + "warp_anchors[" + std::to_string(anchor_index) + "].";
    util::check::at_least(anchor.warp_sample, 0.0, (anchor_prefix + "warp_sample").c_str());
    util::check::at_least(anchor.source_sample, 0.0, (anchor_prefix + "source_sample").c_str());
    if (anchor_index > 0) {
      const SonareEngineWarpAnchor& prev = clip.warp_anchors[anchor_index - 1];
      if (!(anchor.warp_sample > prev.warp_sample && anchor.source_sample > prev.source_sample)) {
        util::check::refuse_relation(
            {anchor_prefix + "warp_sample", anchor_prefix + "source_sample"},
            "{0} and {1} must be greater than those of " + prefix + "warp_anchors[" +
                std::to_string(anchor_index - 1) + "]");
      }
    }
  }
}

// Validates one public clip and builds its schedule, copying or baking its audio.
// Nothing reaches the engine here, so a refused clip leaves the published set alone.
SonareError build_clip_schedule(const SonareEngine& handle, const SonareEngineClip& clip,
                                const std::string& prefix, engine::ClipSchedule* out) {
  if (const SonareError err = run_validation([&] { check_clip(clip, prefix); }); err != SONARE_OK) {
    return err;
  }
  const bool paged = clip.page_provider != nullptr;
  const int source_channels =
      paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
  const int64_t source_samples =
      paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
  // Compute the default only after validating the offset so the subtraction
  // cannot overflow. Only 0 asks for the source length; check_clip refused any
  // other non-positive length.
  const int64_t effective_length =
      clip.length_samples == 0 ? source_samples - clip.clip_offset_samples : clip.length_samples;
  auto owned = std::make_shared<engine::ClipAudioStorage>();
  owned->channels.reserve(static_cast<size_t>(source_channels));
  owned->channel_ptrs.reserve(static_cast<size_t>(source_channels));
  if (paged) {
    // Paged providers are retained by shared_ptr on the ClipSchedule; no audio
    // is copied into ClipAudioStorage.
  } else if (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) {
    std::vector<engine::TempoSyncWarpSegment> segments;
    if (!tempo_sync_segments_for_clip(clip, effective_length, &segments)) {
      return run_validation([&] {
        util::check::refuse(
            (prefix + "warp_anchors").c_str(),
            "must describe non-empty spans inside the source for a tempo-sync clip");
      });
    }
    engine::TempoSyncWarpBakeConfig bake_config;
    bake_config.sample_rate = static_cast<int>(handle.engine.sample_rate());
    std::vector<const float*> source_channel_ptrs;
    source_channel_ptrs.reserve(static_cast<size_t>(clip.num_channels));
    for (int ch = 0; ch < clip.num_channels; ++ch) {
      if (!clip.channels[ch]) {
        return refuse_null((prefix + "channels[" + std::to_string(ch) + "]").c_str());
      }
      source_channel_ptrs.push_back(clip.channels[ch]);
    }
    owned->channels = engine::bake_tempo_sync_warp_channels(
        source_channel_ptrs, static_cast<size_t>(clip.num_samples), segments, bake_config);
  } else {
    for (int ch = 0; ch < clip.num_channels; ++ch) {
      if (!clip.channels[ch]) {
        return refuse_null((prefix + "channels[" + std::to_string(ch) + "]").c_str());
      }
      owned->channels.emplace_back(clip.channels[ch], clip.channels[ch] + clip.num_samples);
    }
  }
  owned->channel_ptrs.clear();
  for (const auto& channel : owned->channels) {
    owned->channel_ptrs.push_back(channel.data());
  }
  owned->refresh_content_signature();
  engine::ClipSchedule schedule{};
  schedule.id = clip.id;
  schedule.track_id = clip.track_id;
  schedule.buffer =
      paged ? engine::ClipAudioBuffer{}
            : engine::ClipAudioBuffer{
                  owned->channel_ptrs.data(), source_channels,
                  static_cast<int64_t>(owned->channels.empty() ? 0 : owned->channels[0].size())};
  schedule.storage = owned;
  if (paged) schedule.page_provider = clip.page_provider->provider;
  schedule.start_ppq = clip.start_ppq;
  schedule.clip_offset_samples =
      clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC ? 0 : clip.clip_offset_samples;
  schedule.length_samples = effective_length;
  schedule.loop = clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC ? false : clip.loop != 0;
  schedule.gain = clip.gain;
  schedule.fade_in_samples = clip.fade_in_samples;
  schedule.fade_out_samples = clip.fade_out_samples;
  schedule.warp_mode =
      clip.warp_mode == SONARE_ENGINE_WARP_MODE_REPITCH        ? engine::WarpMode::kRepitch
      : clip.warp_mode == SONARE_ENGINE_WARP_MODE_TIME_STRETCH ? engine::WarpMode::kTimeStretch
                                                               : engine::WarpMode::kOff;
  if ((schedule.warp_mode == engine::WarpMode::kRepitch ||
       schedule.warp_mode == engine::WarpMode::kTimeStretch) &&
      clip.warp_anchor_count >= 2) {
    auto anchors = std::make_shared<std::vector<engine::WarpAnchor>>();
    anchors->reserve(clip.warp_anchor_count);
    for (size_t anchor_index = 0; anchor_index < clip.warp_anchor_count; ++anchor_index) {
      anchors->push_back({clip.warp_anchors[anchor_index].warp_sample,
                          clip.warp_anchors[anchor_index].source_sample});
    }
    schedule.warp_anchors = std::move(anchors);
  }
  *out = std::move(schedule);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_engine_set_clips(SonareEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(clips, clip_count);
  SONARE_C_TRY
  std::vector<engine::ClipSchedule> schedules(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareError status =
        build_clip_schedule(*engine, clips[i], "clips[" + std::to_string(i) + "].", &schedules[i]);
    if (status != SONARE_OK) return status;
  }
  engine->engine.set_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_upsert_clip(SonareEngine* engine, const SonareEngineClip* clip) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(clip);
  SONARE_C_TRY
  engine::ClipSchedule schedule{};
  const SonareError status = build_clip_schedule(*engine, *clip, "clip.", &schedule);
  if (status != SONARE_OK) return status;
  engine->engine.upsert_clip(std::move(schedule));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_remove_clip(SonareEngine* engine, uint32_t clip_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_TRY
  if (engine->engine.remove_clip(clip_id)) return SONARE_OK;
  return run_validation([&] { util::check::refuse_unknown_id("clip_id", "a clip", "engine"); });
  SONARE_C_CATCH
}

SonareError sonare_engine_clip_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->engine.clip_count();
  return SONARE_OK;
}

SonareError sonare_clip_page_provider_create(int num_channels, int64_t num_samples,
                                             int64_t page_frames,
                                             SonareClipPageProvider** out_provider) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(out_provider);
  if (!engine::validate_clip_page_dimensions(num_channels, num_samples, page_frames)) {
    return run_validation([&] {
      util::check::integer_in_range(num_channels, 1, engine::kMaxClipPageChannels, "num_channels");
      util::check::integer_at_least<int64_t>(num_samples, 1, "num_samples");
      util::check::integer_in_range(page_frames, 1, engine::kMaxClipPageFrames, "page_frames");
      util::check::refuse_relation({"num_samples", "page_frames"},
                                   "{0} must span at most " +
                                       std::to_string(engine::kMaxClipPageCount) +
                                       " pages of {1} frames");
    });
  }
  SONARE_C_TRY
  auto handle = std::make_unique<SonareClipPageProvider>();
  handle->provider = std::make_shared<CClipPageProvider>(num_channels, num_samples, page_frames);
  *out_provider = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_clip_page_provider_destroy(SonareClipPageProvider* provider) { delete provider; }

SonareError sonare_clip_page_provider_supply(SonareClipPageProvider* provider, int64_t page_index,
                                             const float* const* channels, int num_channels,
                                             int64_t frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(provider);
  if (!provider->provider) {
    return run_validation([&] { util::check::refuse("provider", "must hold a page provider"); });
  }
  if (provider->provider->supply(page_index, channels, num_channels, frames)) return SONARE_OK;
  return run_validation([&] {
    const CClipPageProvider& pages = *provider->provider;
    util::check::integer_in_range(page_index, 0, pages.page_count() - 1, "page_index");
    util::check::non_null(channels, "channels");
    util::check::one_of(num_channels, std::array<int, 1>{pages.num_channels()}, "num_channels");
    const int64_t page_start = page_index * pages.page_frames();
    const int64_t expected_frames =
        std::min<int64_t>(pages.page_frames(), pages.num_samples() - page_start);
    util::check::one_of(frames, std::array<int64_t, 1>{expected_frames}, "frames");
    for (int ch = 0; ch < pages.num_channels(); ++ch) {
      util::check::non_null(channels[ch], ("channels[" + std::to_string(ch) + "]").c_str());
    }
  });
  SONARE_C_CATCH
}

SonareError sonare_clip_page_provider_clear(SonareClipPageProvider* provider, int64_t page_index) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(provider);
  if (!provider->provider) {
    return run_validation([&] { util::check::refuse("provider", "must hold a page provider"); });
  }
  if (provider->provider->clear(page_index)) return SONARE_OK;
  return run_validation([&] {
    util::check::integer_in_range(page_index, 0, provider->provider->page_count() - 1,
                                  "page_index");
  });
  SONARE_C_CATCH
}

SonareError sonare_engine_pop_clip_page_request(SonareEngine* engine,
                                                SonareClipPageRequest* out_request,
                                                int* out_has_request) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_request);
  SONARE_C_REQUIRE_NONNULL(out_has_request);
  engine::ClipPageRequest request{};
  if (engine->engine.pop_clip_page_request(request)) {
    out_request->clip_id = request.clip_id;
    out_request->channel = request.channel;
    out_request->sample = request.sample;
    *out_has_request = 1;
  } else {
    *out_request = {};
    *out_has_request = 0;
  }
  return SONARE_OK;
}

SonareError sonare_engine_clip_page_request_overflow_count(SonareEngine* engine,
                                                           uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->engine.clip_page_request_overflow_count();
  return SONARE_OK;
}

SonareError sonare_engine_warp_stretch_overflow_count(SonareEngine* engine, uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = engine->engine.warp_stretch_overflow_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_warp_voice_capacity(SonareEngine* engine, uint32_t voices) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (!engine->engine.set_warp_voice_capacity(voices)) {
    return run_validation(
        [&] { util::check::integer_in_range(voices, 0, SONARE_ENGINE_MAX_WARP_VOICES, "voices"); });
  }
  return SONARE_OK;
}

SonareError sonare_engine_warp_voice_capacity(SonareEngine* engine, uint32_t* out_voices) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_voices);
  *out_voices = engine->engine.warp_voice_capacity();
  return SONARE_OK;
}

SonareError sonare_engine_set_clip_page_prefetch_frames(SonareEngine* engine, int64_t frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err =
          run_validation([&] { util::check::integer_at_least<int64_t>(frames, 0, "frames"); });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_clip_page_prefetch_frames(frames);
  return SONARE_OK;
}

SonareError sonare_engine_clip_page_prefetch_frames(SonareEngine* engine, int64_t* out_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_frames);
  *out_frames = engine->engine.clip_page_prefetch_frames();
  return SONARE_OK;
}
