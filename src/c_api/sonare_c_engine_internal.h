#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "automation/parameter.h"
#include "engine/clip_page_limits.h"
#include "engine/realtime_engine.h"
#include "sonare_c_internal.h"

namespace sonare_c_engine_detail {

/// Realtime page storage backing a clip's paged audio provider. Shared across
/// the engine C-ABI translation units (the clips TU supplies/clears pages, the
/// provider lifecycle entry points create/destroy it, and set_clips queries its
/// channel/sample geometry), so the type must have a single definition rather
/// than a file-local anonymous one — otherwise each TU would synthesise a
/// distinct type and break the opaque-handle identity.
class CClipPageProvider final : public sonare::engine::ClipPagedAudioProvider {
 public:
  CClipPageProvider(int channels, int64_t samples, int64_t page_frames)
      : channels_(channels),
        samples_(samples),
        page_frames_(page_frames),
        pages_(static_cast<size_t>(1 + (samples - 1) / page_frames)),
        page_ptrs_(pages_.size()) {
    for (auto& page_ptr : page_ptrs_) {
      page_ptr.store(nullptr, std::memory_order_relaxed);
    }
  }

  int num_channels() const noexcept override { return channels_; }
  int64_t num_samples() const noexcept override { return samples_; }
  int64_t page_frames() const noexcept override { return page_frames_; }
  int64_t page_count() const noexcept { return static_cast<int64_t>(pages_.size()); }

  bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
    if (!out || channel < 0 || channel >= channels_ || sample < 0 || sample >= samples_) {
      return false;
    }
    const int64_t page_index = sample / page_frames_;
    const int64_t offset = sample % page_frames_;
    if (page_index < 0 || page_index >= page_count()) return false;
    const uint32_t epoch = begin_read();
    const Page* page = page_ptrs_[static_cast<size_t>(page_index)].load(std::memory_order_acquire);
    bool found = false;
    if (page && channel < static_cast<int>(page->channels.size()) && offset >= 0 &&
        offset < page->frames) {
      *out = page->channels[static_cast<size_t>(channel)][static_cast<size_t>(offset)];
      found = true;
    }
    end_read(epoch);
    return found;
  }

  bool page_resident(int64_t page_index) const noexcept override {
    if (page_index < 0 || page_index >= page_count()) return false;
    return page_ptrs_[static_cast<size_t>(page_index)].load(std::memory_order_acquire) != nullptr;
  }

  bool supply(int64_t page_index, const float* const* channels, int channel_count, int64_t frames) {
    if (page_index < 0 || page_index >= page_count() || !channels || channel_count != channels_ ||
        frames <= 0 || frames > page_frames_) {
      return false;
    }
    const int64_t page_start = page_index * page_frames_;
    if (page_start >= samples_) return false;
    const int64_t max_frames = std::min<int64_t>(page_frames_, samples_ - page_start);
    if (frames != max_frames) return false;
    auto page = std::make_unique<Page>();
    page->frames = frames;
    page->channels.reserve(static_cast<size_t>(channels_));
    for (int ch = 0; ch < channels_; ++ch) {
      if (!channels[ch]) return false;
      page->channels.emplace_back(channels[ch], channels[ch] + frames);
    }
    const size_t index = static_cast<size_t>(page_index);
    std::unique_ptr<Page> retired = std::move(pages_[index]);
    pages_[index] = std::move(page);
    page_ptrs_[index].store(pages_[index].get(), std::memory_order_release);
    reclaim_after_epoch(std::move(retired));
    return true;
  }

  bool clear(int64_t page_index) {
    if (page_index < 0 || page_index >= page_count()) return false;
    const size_t index = static_cast<size_t>(page_index);
    page_ptrs_[index].store(nullptr, std::memory_order_release);
    reclaim_after_epoch(std::move(pages_[index]));
    return true;
  }

  size_t retired_page_count_for_test() const noexcept { return 0; }

 private:
  struct Page {
    int64_t frames = 0;
    std::vector<std::vector<float>> channels;
  };

  uint32_t begin_read() const noexcept {
    for (;;) {
      const uint32_t epoch = reader_epoch_.load(std::memory_order_acquire) & 1u;
      readers_[epoch].fetch_add(1, std::memory_order_acq_rel);
      if ((reader_epoch_.load(std::memory_order_acquire) & 1u) == epoch) return epoch;
      readers_[epoch].fetch_sub(1, std::memory_order_release);
    }
  }

  void end_read(uint32_t epoch) const noexcept {
    readers_[epoch & 1u].fetch_sub(1, std::memory_order_release);
  }

  void reclaim_after_epoch(std::unique_ptr<Page> retired) noexcept {
    if (!retired) return;
    const uint32_t old_epoch = reader_epoch_.fetch_xor(1u, std::memory_order_acq_rel) & 1u;
    while (readers_[old_epoch].load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    retired.reset();
  }

  int channels_ = 0;
  int64_t samples_ = 0;
  int64_t page_frames_ = 0;
  std::vector<std::unique_ptr<Page>> pages_;
  mutable std::vector<std::atomic<const Page*>> page_ptrs_;
  mutable std::array<std::atomic<uint32_t>, 2> readers_{};
  mutable std::atomic<uint32_t> reader_epoch_{0};
};

// Pin the automation curve ordinal mapping used by
// sonare_engine_set_automation_lane (via SonareAutomationPoint.curve_to_next).
// As of the AutomationCurve unification this scheme matches the sample-accurate
// mixer API (sonare_strip_schedule_*_automation), so a single canonical
// ordinal set covers both subsystems and all four bindings.
static_assert(static_cast<int>(sonare::automation::CurveType::Linear) == 0,
              "automation::CurveType::Linear must be ordinal 0 to keep "
              "SonareAutomationPoint.curve_to_next ABI stable");
static_assert(static_cast<int>(sonare::automation::CurveType::Exponential) == 1,
              "automation::CurveType::Exponential must be ordinal 1");
static_assert(static_cast<int>(sonare::automation::CurveType::Hold) == 2,
              "automation::CurveType::Hold must be ordinal 2");
static_assert(static_cast<int>(sonare::automation::CurveType::SCurve) == 3,
              "automation::CurveType::SCurve must be ordinal 3");

inline sonare::automation::CurveType curve_from_int(int curve) {
  switch (curve) {
    case 1:
      return sonare::automation::CurveType::Exponential;
    case 2:
      return sonare::automation::CurveType::Hold;
    case 3:
      return sonare::automation::CurveType::SCurve;
    case 0:
    default:
      return sonare::automation::CurveType::Linear;
  }
}

inline int curve_to_int(sonare::automation::CurveType curve) { return static_cast<int>(curve); }

// Fixed-field C-string copy shared across the C API. The canonical definition
// lives in sonare_c_internal.h; re-export it here so engine TUs keep using the
// unqualified sonare_c_engine_detail::copy_text spelling.
using sonare_c_detail::copy_text;

inline std::string fixed_text(const char* src, size_t capacity) {
  const char* end = std::find(src, src + capacity, '\0');
  return std::string(src, end);
}

}  // namespace sonare_c_engine_detail

/// Opaque handle exposed to the public C ABI; resolves to the shared page
/// storage. Defined exactly once here so every engine C-ABI TU shares the layout.
struct SonareClipPageProvider {
  std::shared_ptr<sonare_c_engine_detail::CClipPageProvider> provider;
};
