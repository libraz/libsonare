#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/tempo_sync.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"
#include "util/numeric_validation.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_engine_detail;

namespace {

bool rounded_nonnegative_sample(double sample, size_t* out) noexcept {
  if (!out || !std::isfinite(sample) || sample < 0.0) return false;
  return numeric::checked_round_cast(sample, out);
}

bool tempo_sync_segments_for_clip(const SonareEngineClip& clip,
                                  std::vector<engine::TempoSyncWarpSegment>* out) {
  if (!out || clip.length_samples <= 0 || clip.clip_offset_samples < 0 ||
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
  segment.target_samples = static_cast<size_t>(clip.length_samples);
  if (segment.source_samples == 0 || segment.target_samples == 0) return false;
  out->push_back(segment);
  return true;
}

}  // namespace

SonareError sonare_engine_set_clips(SonareRealtimeEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count) {
  SONARE_C_API_ENTRY;
  if (!engine || (clip_count > 0 && !clips)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<std::shared_ptr<engine::ClipAudioStorage>> clip_storage;
  clip_storage.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    const bool paged = clip.page_provider != nullptr;
    if (paged && !clip.page_provider->provider) return SONARE_ERROR_INVALID_PARAMETER;
    const int source_channels =
        paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
    const int64_t source_samples =
        paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
    if ((!paged && !clip.channels) || source_channels <= 0 || source_samples <= 0 ||
        !std::isfinite(clip.start_ppq) || !transport::valid_public_ppq(clip.start_ppq) ||
        clip.clip_offset_samples < 0 || clip.clip_offset_samples >= source_samples ||
        !(std::isfinite(clip.gain) && clip.gain >= 0.0f) || clip.fade_in_samples < 0 ||
        clip.fade_out_samples < 0 ||
        (clip.warp_mode != SONARE_ENGINE_WARP_MODE_OFF &&
         clip.warp_mode != SONARE_ENGINE_WARP_MODE_REPITCH &&
         clip.warp_mode != SONARE_ENGINE_WARP_MODE_TEMPO_SYNC &&
         clip.warp_mode != SONARE_ENGINE_WARP_MODE_TIME_STRETCH) ||
        (paged && clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) ||
        (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC && clip.loop != 0) ||
        ((clip.warp_mode == SONARE_ENGINE_WARP_MODE_REPITCH ||
          clip.warp_mode == SONARE_ENGINE_WARP_MODE_TIME_STRETCH) &&
         clip.loop != 0 && clip.warp_anchor_count >= 2) ||
        (clip.warp_anchor_count > 0 && !clip.warp_anchors)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    // Compute the default only after validating the offset so the subtraction
    // cannot overflow (INT64_MIN was previously accepted into this expression).
    const int64_t effective_length =
        clip.length_samples > 0 ? clip.length_samples : source_samples - clip.clip_offset_samples;
    if (effective_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
    for (size_t anchor_index = 0; anchor_index < clip.warp_anchor_count; ++anchor_index) {
      const SonareEngineWarpAnchor& anchor = clip.warp_anchors[anchor_index];
      if (!std::isfinite(anchor.warp_sample) || !std::isfinite(anchor.source_sample) ||
          anchor.warp_sample < 0.0 || anchor.source_sample < 0.0) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (anchor_index > 0) {
        const SonareEngineWarpAnchor& prev = clip.warp_anchors[anchor_index - 1];
        if (!(anchor.warp_sample > prev.warp_sample && anchor.source_sample > prev.source_sample)) {
          return SONARE_ERROR_INVALID_PARAMETER;
        }
      }
    }
    auto owned = std::make_shared<engine::ClipAudioStorage>();
    owned->channels.reserve(static_cast<size_t>(source_channels));
    owned->channel_ptrs.reserve(static_cast<size_t>(source_channels));
    if (paged) {
      // Paged providers are retained by shared_ptr on the ClipSchedule; no audio
      // is copied into ClipAudioStorage.
    } else if (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) {
      std::vector<engine::TempoSyncWarpSegment> segments;
      if (!tempo_sync_segments_for_clip(clip, &segments)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      engine::TempoSyncWarpBakeConfig bake_config;
      bake_config.sample_rate = static_cast<int>(std::lround(engine->engine.sample_rate()));
      std::vector<const float*> source_channel_ptrs;
      source_channel_ptrs.reserve(static_cast<size_t>(clip.num_channels));
      for (int ch = 0; ch < clip.num_channels; ++ch) {
        if (!clip.channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
        source_channel_ptrs.push_back(clip.channels[ch]);
      }
      owned->channels = engine::bake_tempo_sync_warp_channels(
          source_channel_ptrs, static_cast<size_t>(clip.num_samples), segments, bake_config);
    } else {
      for (int ch = 0; ch < clip.num_channels; ++ch) {
        if (!clip.channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
        owned->channels.emplace_back(clip.channels[ch], clip.channels[ch] + clip.num_samples);
      }
    }
    clip_storage.push_back(std::move(owned));
  }

  std::vector<engine::ClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    const bool paged = clip.page_provider != nullptr;
    const int source_channels =
        paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
    const int64_t source_samples =
        paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
    const int64_t effective_length =
        clip.length_samples > 0 ? clip.length_samples : source_samples - clip.clip_offset_samples;
    auto& owned = clip_storage[i];
    owned->channel_ptrs.clear();
    for (const auto& channel : owned->channels) {
      owned->channel_ptrs.push_back(channel.data());
    }
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
    schedules.push_back(schedule);
  }
  engine->engine.set_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clip_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.clip_count();
  return SONARE_OK;
}

SonareError sonare_clip_page_provider_create(int num_channels, int64_t num_samples,
                                             int64_t page_frames,
                                             SonareClipPageProvider** out_provider) {
  SONARE_C_API_ENTRY;
  if (!out_provider ||
      !engine::validate_clip_page_dimensions(num_channels, num_samples, page_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
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
  if (!provider || !provider->provider ||
      !provider->provider->supply(page_index, channels, num_channels, frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_clip_page_provider_clear(SonareClipPageProvider* provider, int64_t page_index) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  if (!provider || !provider->provider || !provider->provider->clear(page_index)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_pop_clip_page_request(SonareRealtimeEngine* engine,
                                                SonareClipPageRequest* out_request,
                                                int* out_has_request) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_request || !out_has_request) return SONARE_ERROR_INVALID_PARAMETER;
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

SonareError sonare_engine_set_clip_page_prefetch_frames(SonareRealtimeEngine* engine,
                                                        int64_t frames) {
  SONARE_C_API_ENTRY;
  if (!engine || frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_clip_page_prefetch_frames(frames);
  return SONARE_OK;
}

SonareError sonare_engine_clip_page_prefetch_frames(SonareRealtimeEngine* engine,
                                                    int64_t* out_frames) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_frames) return SONARE_ERROR_INVALID_PARAMETER;
  *out_frames = engine->engine.clip_page_prefetch_frames();
  return SONARE_OK;
}
