/// @file realtime_engine_clips.cpp
/// @brief Embind realtime-engine facade: clip scheduling & paged providers.

#ifdef __EMSCRIPTEN__

#include <array>
#include <atomic>
#include <thread>

#include "engine/clip_page_limits.h"
#include "engine/tempo_sync.h"
#include "realtime_engine_wasm.h"
#include "transport/tempo_map.h"
#include "util/numeric_validation.h"

class WasmClipPageProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  WasmClipPageProvider(int channels, int64_t samples, int64_t page_frames)
      : channels_(channels),
        samples_(samples),
        page_frames_(page_frames),
        pages_(static_cast<size_t>(1 + (samples - 1) / page_frames)),
        page_ptrs_(pages_.size()) {
    for (auto& page_ptr : page_ptrs_) page_ptr.store(nullptr, std::memory_order_relaxed);
  }

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
    const uint32_t epoch = beginRead();
    const Page* page = page_ptrs_[static_cast<size_t>(page_index)].load(std::memory_order_acquire);
    bool found = false;
    if (page && offset >= 0 && offset < page->frames) {
      *out = page->channels[static_cast<size_t>(channel)][static_cast<size_t>(offset)];
      found = true;
    }
    endRead(epoch);
    return found;
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
    auto page = std::make_unique<Page>();
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
    const size_t index = static_cast<size_t>(page_index);
    std::unique_ptr<Page> retired = std::move(pages_[index]);
    pages_[index] = std::move(page);
    page_ptrs_[index].store(pages_[index].get(), std::memory_order_release);
    reclaimAfterEpoch(std::move(retired));
  }

  void clear(int64_t page_index) {
    if (page_index < 0 || page_index >= static_cast<int64_t>(pages_.size())) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid page index");
    }
    const size_t index = static_cast<size_t>(page_index);
    page_ptrs_[index].store(nullptr, std::memory_order_release);
    reclaimAfterEpoch(std::move(pages_[index]));
  }

 private:
  struct Page {
    int64_t frames = 0;
    std::vector<std::vector<float>> channels;
  };

  uint32_t beginRead() const noexcept {
    for (;;) {
      const uint32_t epoch = reader_epoch_.load(std::memory_order_acquire) & 1u;
      readers_[epoch].fetch_add(1, std::memory_order_acq_rel);
      if ((reader_epoch_.load(std::memory_order_acquire) & 1u) == epoch) return epoch;
      readers_[epoch].fetch_sub(1, std::memory_order_release);
    }
  }

  void endRead(uint32_t epoch) const noexcept {
    readers_[epoch & 1u].fetch_sub(1, std::memory_order_release);
  }

  void reclaimAfterEpoch(std::unique_ptr<Page> retired) noexcept {
    if (!retired) return;
    const uint32_t old_epoch = reader_epoch_.fetch_xor(1u, std::memory_order_acq_rel) & 1u;
    while (readers_[old_epoch].load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
  }

  int channels_ = 0;
  int64_t samples_ = 0;
  int64_t page_frames_ = 0;
  std::vector<std::unique_ptr<Page>> pages_;
  mutable std::vector<std::atomic<const Page*>> page_ptrs_;
  mutable std::array<std::atomic<uint32_t>, 2> readers_{};
  mutable std::atomic<uint32_t> reader_epoch_{0};
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
  std::vector<uint32_t> new_clip_ids;
  std::vector<uint8_t> new_clip_tempo_baked;
  new_clip_ids.reserve(static_cast<size_t>(count));
  new_clip_tempo_baked.reserve(static_cast<size_t>(count));

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
    if (!std::isfinite(schedule.start_ppq) ||
        !sonare::transport::valid_public_ppq(schedule.start_ppq)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip startPpq is outside the public timeline range");
    }
    // clip_offset_samples / fade_*_samples are int64_t in ClipSchedule; read
    // them at full 64-bit precision (like length_samples below) so large
    // offsets above 2^31 samples do not silently truncate/sign-flip.
    schedule.clip_offset_samples = hasProperty(clip_val, "clipOffsetSamples")
                                       ? objectProperty(clip_val, "clipOffsetSamples").as<int64_t>()
                                       : 0;
    const int64_t source_samples = has_page_provider && schedule.page_provider
                                       ? schedule.page_provider->num_samples()
                                       : num_samples;
    if (schedule.clip_offset_samples < 0 || schedule.clip_offset_samples >= source_samples) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip offset is outside the source");
    }
    const int64_t default_length = source_samples - schedule.clip_offset_samples;
    schedule.length_samples = hasProperty(clip_val, "lengthSamples")
                                  ? objectProperty(clip_val, "lengthSamples").as<int64_t>()
                                  : default_length;
    if (schedule.length_samples <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip offset or length is outside the source");
    }
    schedule.loop = boolProperty(clip_val, "loop", false);
    schedule.gain = floatProperty(clip_val, "gain", 1.0f);
    if (!(std::isfinite(schedule.gain) && schedule.gain >= 0.0f)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip gain must be a finite non-negative number");
    }
    schedule.fade_in_samples = hasProperty(clip_val, "fadeInSamples")
                                   ? objectProperty(clip_val, "fadeInSamples").as<int64_t>()
                                   : 0;
    schedule.fade_out_samples = hasProperty(clip_val, "fadeOutSamples")
                                    ? objectProperty(clip_val, "fadeOutSamples").as<int64_t>()
                                    : 0;
    if (schedule.fade_in_samples < 0 || schedule.fade_out_samples < 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "clip fade lengths must be non-negative");
    }
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
          const sonare::engine::WarpAnchor anchor{
              objectProperty(anchor_val, "warpSample").as<double>(),
              objectProperty(anchor_val, "sourceSample").as<double>()};
          if (!std::isfinite(anchor.warp_sample) || !std::isfinite(anchor.source_sample) ||
              anchor.warp_sample < 0.0 || anchor.source_sample < 0.0 ||
              (!anchors->empty() && (!(anchor.warp_sample > anchors->back().warp_sample) ||
                                     !(anchor.source_sample > anchors->back().source_sample)))) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "warp anchors must be finite and strictly increasing");
          }
          anchors->push_back(anchor);
        }
        schedule.warp_anchors = std::move(anchors);
      }
    }
    const bool tempo_sync_baked = schedule.warp_mode == sonare::engine::WarpMode::kTempoSync;
    if (tempo_sync_baked) {
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
      const auto rounded_nonnegative_sample = [](double sample, size_t* out) noexcept {
        return sonare::numeric::checked_round_cast(sample, out) && sample >= 0.0;
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
          size_t source_start = 0;
          size_t source_end = 0;
          size_t target_start = 0;
          size_t target_end = 0;
          if (!rounded_nonnegative_sample(prev.source_sample, &source_start) ||
              !rounded_nonnegative_sample(next.source_sample, &source_end) ||
              !rounded_nonnegative_sample(prev.warp_sample, &target_start) ||
              !rounded_nonnegative_sample(next.warp_sample, &target_end)) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "tempo-sync warp anchor is out of sample range");
          }
          sonare::engine::TempoSyncWarpSegment segment;
          if (!sonare::numeric::checked_add(base_offset, source_start, &segment.source_offset)) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "tempo-sync warp source offset is out of range");
          }
          segment.source_samples = source_end > source_start ? source_end - source_start : 0;
          segment.target_samples = target_end > target_start ? target_end - target_start : 0;
          if (segment.source_offset > static_cast<size_t>(num_samples) ||
              segment.source_samples > static_cast<size_t>(num_samples) - segment.source_offset ||
              segment.source_samples == 0 || segment.target_samples == 0) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "tempo-sync warp anchors must span positive samples");
          }
          if (!sonare::numeric::checked_add(target_samples, segment.target_samples,
                                            &target_samples)) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "tempo-sync warp target length is out of range");
          }
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
    new_clip_ids.push_back(schedule.id);
    new_clip_tempo_baked.push_back(tempo_sync_baked ? 1u : 0u);
  }
  clip_storage_ = std::move(new_storage);
  clip_ptrs_ = std::move(new_ptrs);
  clip_ids_ = std::move(new_clip_ids);
  clip_tempo_baked_ = std::move(new_clip_tempo_baked);
  engine_.set_clips(std::move(schedules));
}

val RealtimeEngineWasm::prebakedClipChannels(uint32_t clip_id) const {
  const auto it = std::find(clip_ids_.begin(), clip_ids_.end(), clip_id);
  if (it == clip_ids_.end()) return val::null();
  const size_t index = static_cast<size_t>(std::distance(clip_ids_.begin(), it));
  if (index >= clip_tempo_baked_.size() || clip_tempo_baked_[index] == 0 ||
      index >= clip_storage_.size()) {
    return val::null();
  }
  val channels = val::array();
  for (const auto& channel : clip_storage_[index]) {
    val copied = val::global("Float32Array").new_(channel.size());
    copied.call<void>("set", val(typed_memory_view(channel.size(), channel.data())));
    channels.call<void>("push", copied);
  }
  return channels;
}

int RealtimeEngineWasm::clipCount() const { return static_cast<int>(engine_.clip_count()); }

int RealtimeEngineWasm::createClipPageProvider(int num_channels, int64_t num_samples,
                                               int64_t page_frames) {
  if (!sonare::engine::validate_clip_page_dimensions(num_channels, num_samples, page_frames)) {
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

bool RealtimeEngineWasm::popClipPageRequestToScratch() {
  return engine_.pop_clip_page_request(clip_page_request_scratch_);
}

uint32_t RealtimeEngineWasm::clipPageRequestScratchClipId() const {
  return clip_page_request_scratch_.clip_id;
}

double RealtimeEngineWasm::clipPageRequestScratchSample() const {
  return static_cast<double>(clip_page_request_scratch_.sample);
}

uint32_t RealtimeEngineWasm::clipPageRequestOverflowCount() const {
  return engine_.clip_page_request_overflow_count();
}

void registerRealtimeEngineClips(class_<RealtimeEngineWasm>& cls) {
  cls.function("setClips", &RealtimeEngineWasm::setClips)
      .function("prebakedClipChannels", &RealtimeEngineWasm::prebakedClipChannels)
      .function("clipCount", &RealtimeEngineWasm::clipCount)
      .function("createClipPageProvider", &RealtimeEngineWasm::createClipPageProvider)
      .function("supplyClipPage", &RealtimeEngineWasm::supplyClipPage)
      .function("clearClipPage", &RealtimeEngineWasm::clearClipPage)
      .function("destroyClipPageProvider", &RealtimeEngineWasm::destroyClipPageProvider)
      .function("popClipPageRequest", &RealtimeEngineWasm::popClipPageRequest)
      .function("popClipPageRequestToScratch", &RealtimeEngineWasm::popClipPageRequestToScratch)
      .function("clipPageRequestScratchClipId", &RealtimeEngineWasm::clipPageRequestScratchClipId)
      .function("clipPageRequestScratchSample", &RealtimeEngineWasm::clipPageRequestScratchSample)
      .function("clipPageRequestOverflowCount", &RealtimeEngineWasm::clipPageRequestOverflowCount);
}

#endif  // __EMSCRIPTEN__
