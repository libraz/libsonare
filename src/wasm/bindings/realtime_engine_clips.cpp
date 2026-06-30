/// @file realtime_engine_clips.cpp
/// @brief Embind realtime-engine facade: clip scheduling & paged providers.

#ifdef __EMSCRIPTEN__

#include <atomic>

#include "engine/tempo_sync.h"
#include "realtime_engine_wasm.h"

class WasmClipPageProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  WasmClipPageProvider(int channels, int64_t samples, int64_t page_frames)
      : channels_(channels),
        samples_(samples),
        page_frames_(page_frames),
        pages_(static_cast<size_t>((samples + page_frames - 1) / page_frames)) {}

  int num_channels() const noexcept override { return channels_; }
  int64_t num_samples() const noexcept override { return samples_; }
  int64_t page_frames() const noexcept override { return page_frames_; }

  bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
    if (!out || channel < 0 || channel >= channels_ || sample < 0 || sample >= samples_) {
      return false;
    }
    const int64_t page_index = sample / page_frames_;
    const int64_t offset = sample % page_frames_;
    if (page_index < 0 || page_index >= static_cast<int64_t>(pages_.size())) return false;
    auto page = std::atomic_load_explicit(&pages_[static_cast<size_t>(page_index)],
                                          std::memory_order_acquire);
    if (!page || offset >= page->frames) return false;
    *out = page->channels[static_cast<size_t>(channel)][static_cast<size_t>(offset)];
    return true;
  }

  void supply(int64_t page_index, val channels_val) {
    if (page_index < 0 || page_index >= static_cast<int64_t>(pages_.size())) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid page index");
    }
    const int channel_count = channels_val["length"].as<int>();
    if (channel_count != channels_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip page channel count mismatch");
    }
    auto page = std::make_shared<Page>();
    page->channels.reserve(static_cast<size_t>(channels_));
    for (int ch = 0; ch < channel_count; ++ch) {
      std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
      if (ch == 0) {
        page->frames = static_cast<int64_t>(channel.size());
        const int64_t page_start = page_index * page_frames_;
        const int64_t max_frames = std::min<int64_t>(page_frames_, samples_ - page_start);
        if (page->frames != max_frames) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "invalid clip page frame count");
        }
      } else if (static_cast<int64_t>(channel.size()) != page->frames) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all clip page channels must have the same length");
      }
      page->channels.push_back(std::move(channel));
    }
    std::atomic_store_explicit(&pages_[static_cast<size_t>(page_index)],
                               std::shared_ptr<const Page>(std::move(page)),
                               std::memory_order_release);
  }

  void clear(int64_t page_index) {
    if (page_index < 0 || page_index >= static_cast<int64_t>(pages_.size())) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid page index");
    }
    std::shared_ptr<const Page> empty;
    std::atomic_store_explicit(&pages_[static_cast<size_t>(page_index)], empty,
                               std::memory_order_release);
  }

 private:
  struct Page {
    int64_t frames = 0;
    std::vector<std::vector<float>> channels;
  };

  int channels_ = 0;
  int64_t samples_ = 0;
  int64_t page_frames_ = 0;
  mutable std::vector<std::shared_ptr<const Page>> pages_;
};

namespace {

std::shared_ptr<WasmClipPageProvider> liveProviderById(
    const std::vector<std::shared_ptr<WasmClipPageProvider>>& providers, int id) {
  if (id <= 0 || static_cast<size_t>(id) > providers.size()) return nullptr;
  return providers[static_cast<size_t>(id - 1)];
}

}  // namespace

void RealtimeEngineWasm::setClips(val clips) {
  const int count = clips["length"].as<int>();
  std::vector<std::vector<std::vector<float>>> new_storage;
  std::vector<std::vector<const float*>> new_ptrs;
  new_storage.reserve(static_cast<size_t>(count));
  new_ptrs.reserve(static_cast<size_t>(count));
  std::vector<sonare::engine::ClipSchedule> schedules;
  schedules.reserve(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i) {
    val clip_val = clips[i];
    const bool has_page_provider = hasProperty(clip_val, "pageProvider") &&
                                   !objectProperty(clip_val, "pageProvider").isNull() &&
                                   !objectProperty(clip_val, "pageProvider").isUndefined();
    val channels_val = has_page_provider ? val::array() : clip_val["channels"];
    const int channel_count = has_page_provider ? 0 : channels_val["length"].as<int>();
    if (!has_page_provider && channel_count <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip channels must not be empty");
    }
    new_storage.emplace_back();
    new_ptrs.emplace_back();
    auto& storage = new_storage.back();
    auto& pointers = new_ptrs.back();
    storage.reserve(static_cast<size_t>(channel_count));
    pointers.reserve(static_cast<size_t>(channel_count));
    int64_t num_samples = 0;
    for (int ch = 0; ch < channel_count; ++ch) {
      std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
      if (ch == 0) {
        num_samples = static_cast<int64_t>(channel.size());
        if (num_samples <= 0) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "clip channels must not be empty");
        }
      } else if (static_cast<int64_t>(channel.size()) != num_samples) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all clip channels must have the same length");
      }
      storage.push_back(std::move(channel));
      pointers.push_back(storage.back().data());
    }

    sonare::engine::ClipSchedule schedule{};
    schedule.id = static_cast<uint32_t>(intProperty(clip_val, "id", i + 1));
    schedule.track_id = hasProperty(clip_val, "trackId")
                            ? static_cast<uint32_t>(intProperty(clip_val, "trackId", 0))
                            : 0;
    if (has_page_provider) {
      const int provider_id = objectProperty(clip_val, "pageProvider").as<int>();
      auto provider = liveProviderById(clip_page_providers_, provider_id);
      if (!provider) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "pageProvider is not live");
      }
      schedule.page_provider = provider;
      schedule.buffer = {};
    } else {
      schedule.buffer = {pointers.data(), channel_count, num_samples};
    }
    schedule.start_ppq = objectProperty(clip_val, "startPpq").as<double>();
    // clip_offset_samples / fade_*_samples are int64_t in ClipSchedule; read
    // them at full 64-bit precision (like length_samples below) so large
    // offsets above 2^31 samples do not silently truncate/sign-flip.
    schedule.clip_offset_samples = hasProperty(clip_val, "clipOffsetSamples")
                                       ? objectProperty(clip_val, "clipOffsetSamples").as<int64_t>()
                                       : 0;
    const int64_t source_samples = has_page_provider && schedule.page_provider
                                       ? schedule.page_provider->num_samples()
                                       : num_samples;
    const int64_t default_length = source_samples - schedule.clip_offset_samples;
    schedule.length_samples = hasProperty(clip_val, "lengthSamples")
                                  ? objectProperty(clip_val, "lengthSamples").as<int64_t>()
                                  : default_length;
    if (schedule.clip_offset_samples < 0 || schedule.clip_offset_samples >= source_samples ||
        schedule.length_samples <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip offset or length is outside the source");
    }
    schedule.loop = boolProperty(clip_val, "loop", false);
    schedule.gain = floatProperty(clip_val, "gain", 1.0f);
    schedule.fade_in_samples = hasProperty(clip_val, "fadeInSamples")
                                   ? objectProperty(clip_val, "fadeInSamples").as<int64_t>()
                                   : 0;
    schedule.fade_out_samples = hasProperty(clip_val, "fadeOutSamples")
                                    ? objectProperty(clip_val, "fadeOutSamples").as<int64_t>()
                                    : 0;
    if (hasProperty(clip_val, "warpMode")) {
      val mode_val = objectProperty(clip_val, "warpMode");
      if (mode_val.typeOf().as<std::string>() == "string") {
        const std::string mode = mode_val.as<std::string>();
        if (mode == "off") {
          schedule.warp_mode = sonare::engine::WarpMode::kOff;
        } else if (mode == "repitch") {
          schedule.warp_mode = sonare::engine::WarpMode::kRepitch;
        } else if (mode == "tempo-sync") {
          schedule.warp_mode = sonare::engine::WarpMode::kTempoSync;
        } else {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown warp mode");
        }
      } else {
        const int mode = mode_val.as<int>();
        if (mode == 0) {
          schedule.warp_mode = sonare::engine::WarpMode::kOff;
        } else if (mode == 1) {
          schedule.warp_mode = sonare::engine::WarpMode::kRepitch;
        } else if (mode == 2) {
          schedule.warp_mode = sonare::engine::WarpMode::kTempoSync;
        } else {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown warp mode");
        }
      }
    }
    if (hasProperty(clip_val, "warpAnchors")) {
      val anchors_val = objectProperty(clip_val, "warpAnchors");
      const int anchor_count = anchors_val["length"].as<int>();
      if (anchor_count > 0) {
        auto anchors = std::make_shared<std::vector<sonare::engine::WarpAnchor>>();
        anchors->reserve(static_cast<size_t>(anchor_count));
        for (int anchor_index = 0; anchor_index < anchor_count; ++anchor_index) {
          val anchor_val = anchors_val[anchor_index];
          anchors->push_back({objectProperty(anchor_val, "warpSample").as<double>(),
                              objectProperty(anchor_val, "sourceSample").as<double>()});
        }
        schedule.warp_anchors = std::move(anchors);
      }
    }
    if (schedule.warp_mode == sonare::engine::WarpMode::kTempoSync) {
      if (has_page_provider) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "tempo-sync paged clips are not supported");
      }
      if (schedule.loop) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "tempo-sync direct clips do not support loop=true yet");
      }
      if (schedule.clip_offset_samples < 0 || schedule.clip_offset_samples >= num_samples) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "tempo-sync clip offset is outside the source");
      }
      const auto rounded_nonnegative_sample = [](double sample) noexcept -> size_t {
        if (!std::isfinite(sample) || sample <= 0.0) return 0;
        return static_cast<size_t>(std::llround(sample));
      };
      std::vector<sonare::engine::TempoSyncWarpSegment> segments;
      const size_t base_offset =
          static_cast<size_t>(std::max<int64_t>(0, schedule.clip_offset_samples));
      size_t target_samples = 0;
      if (schedule.warp_anchors && schedule.warp_anchors->size() >= 2) {
        segments.reserve(schedule.warp_anchors->size() - 1);
        for (size_t anchor_index = 1; anchor_index < schedule.warp_anchors->size();
             ++anchor_index) {
          const auto& prev = (*schedule.warp_anchors)[anchor_index - 1];
          const auto& next = (*schedule.warp_anchors)[anchor_index];
          const size_t source_start = rounded_nonnegative_sample(prev.source_sample);
          const size_t source_end = rounded_nonnegative_sample(next.source_sample);
          const size_t target_start = rounded_nonnegative_sample(prev.warp_sample);
          const size_t target_end = rounded_nonnegative_sample(next.warp_sample);
          sonare::engine::TempoSyncWarpSegment segment;
          segment.source_offset = base_offset + source_start;
          segment.source_samples = source_end > source_start ? source_end - source_start : 0;
          segment.target_samples = target_end > target_start ? target_end - target_start : 0;
          if (segment.source_offset > static_cast<size_t>(num_samples) ||
              segment.source_samples > static_cast<size_t>(num_samples) - segment.source_offset ||
              segment.source_samples == 0 || segment.target_samples == 0) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "tempo-sync warp anchors must span positive samples");
          }
          target_samples += segment.target_samples;
          segments.push_back(segment);
        }
      } else {
        sonare::engine::TempoSyncWarpSegment segment;
        segment.source_offset = base_offset;
        segment.source_samples = static_cast<size_t>(num_samples) - base_offset;
        segment.target_samples = static_cast<size_t>(std::max<int64_t>(1, schedule.length_samples));
        target_samples = segment.target_samples;
        segments.push_back(segment);
      }
      if (segments.empty() || target_samples == 0)
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "tempo-sync clip has an empty source or target span");
      sonare::engine::TempoSyncWarpBakeConfig bake_config;
      bake_config.sample_rate = static_cast<int>(std::lround(engine_.sample_rate()));
      std::vector<const float*> source_channel_ptrs;
      source_channel_ptrs.reserve(storage.size());
      for (const auto& channel : storage) {
        source_channel_ptrs.push_back(channel.data());
      }
      storage = sonare::engine::bake_tempo_sync_warp_channels(
          source_channel_ptrs, storage[0].size(), segments, bake_config);
      pointers.clear();
      for (const auto& channel : storage) {
        pointers.push_back(channel.data());
      }
      schedule.buffer = {pointers.data(), channel_count, static_cast<int64_t>(target_samples)};
      schedule.clip_offset_samples = 0;
      schedule.loop = false;
      schedule.warp_mode = sonare::engine::WarpMode::kOff;
      schedule.warp_anchors.reset();
    } else if (schedule.warp_mode == sonare::engine::WarpMode::kRepitch && schedule.loop &&
               schedule.warp_anchors && schedule.warp_anchors->size() >= 2) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "repitch warped clips do not support loop=true yet");
    }
    schedules.push_back(schedule);
  }
  clip_storage_ = std::move(new_storage);
  clip_ptrs_ = std::move(new_ptrs);
  engine_.set_clips(std::move(schedules));
}

int RealtimeEngineWasm::clipCount() const { return static_cast<int>(engine_.clip_count()); }

int RealtimeEngineWasm::createClipPageProvider(int num_channels, int64_t num_samples,
                                               int64_t page_frames) {
  if (num_channels <= 0 || num_samples <= 0 || page_frames <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "clip page provider dimensions must be positive");
  }
  clip_page_providers_.push_back(
      std::make_shared<WasmClipPageProvider>(num_channels, num_samples, page_frames));
  return static_cast<int>(clip_page_providers_.size());
}

void RealtimeEngineWasm::supplyClipPage(int provider_id, int64_t page_index, val channels) {
  auto provider = liveProviderById(clip_page_providers_, provider_id);
  if (!provider) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "pageProvider is not live");
  }
  provider->supply(page_index, channels);
}

void RealtimeEngineWasm::clearClipPage(int provider_id, int64_t page_index) {
  auto provider = liveProviderById(clip_page_providers_, provider_id);
  if (!provider) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "pageProvider is not live");
  }
  provider->clear(page_index);
}

void RealtimeEngineWasm::destroyClipPageProvider(int provider_id) {
  if (provider_id <= 0 || static_cast<size_t>(provider_id) > clip_page_providers_.size()) return;
  clip_page_providers_[static_cast<size_t>(provider_id - 1)].reset();
}

val RealtimeEngineWasm::popClipPageRequest() {
  sonare::engine::ClipPageRequest request{};
  if (!engine_.pop_clip_page_request(request)) return val::null();
  val out = val::object();
  out.set("clipId", request.clip_id);
  out.set("channel", request.channel);
  out.set("sample", static_cast<double>(request.sample));
  return out;
}

void registerRealtimeEngineClips(class_<RealtimeEngineWasm>& cls) {
  cls.function("setClips", &RealtimeEngineWasm::setClips)
      .function("clipCount", &RealtimeEngineWasm::clipCount)
      .function("createClipPageProvider", &RealtimeEngineWasm::createClipPageProvider)
      .function("supplyClipPage", &RealtimeEngineWasm::supplyClipPage)
      .function("clearClipPage", &RealtimeEngineWasm::clearClipPage)
      .function("destroyClipPageProvider", &RealtimeEngineWasm::destroyClipPageProvider)
      .function("popClipPageRequest", &RealtimeEngineWasm::popClipPageRequest);
}

#endif  // __EMSCRIPTEN__
