#include "engine/clip_player.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "util/constants.h"

namespace sonare::engine {

using sonare::constants::kHalfPi;

void ClipBoundaryList::clear() noexcept {
  size = 0;
  overflowed = false;
}

bool ClipBoundaryList::add(int offset) noexcept {
  if (size >= offsets.size()) {
    overflowed = true;
    return false;
  }
  offsets[size++] = offset;
  return true;
}

void ClipBoundaryList::sort_unique() noexcept {
  std::sort(offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(size));
  size_t out = 0;
  for (size_t i = 0; i < size; ++i) {
    if (out == 0 || offsets[i] != offsets[out - 1]) {
      offsets[out++] = offsets[i];
    }
  }
  size = out;
}

void ClipPlayer::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = std::max(max_block_size, 1);
}

void ClipPlayer::process(float* const* channels, int num_channels, int num_samples) {
  process_at(channels, num_channels, num_samples, timeline_sample_);
  timeline_sample_ += std::max(num_samples, 0);
}

void ClipPlayer::set_tempo_map(const transport::TempoMap* tempo_map) noexcept {
  tempo_map_ = tempo_map;
}

void ClipPlayer::set_clips(std::vector<ClipSchedule> clips) {
  if (tempo_map_) {
    for (ClipSchedule& clip : clips) {
      clip.start_sample = tempo_map_->ppq_to_sample(clip.start_ppq);
    }
  }
  std::sort(clips.begin(), clips.end(), [](const ClipSchedule& a, const ClipSchedule& b) {
    if (a.start_sample != b.start_sample) return a.start_sample < b.start_sample;
    return a.id < b.id;
  });
  clip_count_.store(clips.size(), std::memory_order_relaxed);
  clips_.publish(std::make_shared<const std::vector<ClipSchedule>>(std::move(clips)));
}

void ClipPlayer::process_at(float* const* channels, int num_channels, int num_samples,
                            int64_t timeline_sample) noexcept {
  if (!channels || num_channels <= 0 || num_samples <= 0) return;

  // Adopt the latest published clip set. The engine drives a single block-start
  // acquire_clips() so a clip set is never swapped mid-block between sub-blocks;
  // this idempotent re-acquire (a wait-free pointer swap, no alloc) keeps the
  // standalone ClipPlayer contract working (set_clips then process_at directly).
  clips_.acquire();
  const std::vector<ClipSchedule>* clips = clips_.current();
  if (!clips) return;

  for (const ClipSchedule& clip : *clips) {
    if (!clip.buffer.channels || clip.buffer.num_channels <= 0 || clip.buffer.num_samples <= 0 ||
        clip.length_samples <= 0) {
      continue;
    }
    const int64_t clip_end = clip.start_sample + clip.length_samples;
    const int64_t block_end = timeline_sample + num_samples;
    if (block_end <= clip.start_sample || timeline_sample >= clip_end) {
      continue;
    }

    const int start = static_cast<int>(std::max<int64_t>(0, clip.start_sample - timeline_sample));
    const int end = static_cast<int>(std::min<int64_t>(num_samples, clip_end - timeline_sample));
    const int channels_to_copy = std::min(num_channels, clip.buffer.num_channels);
    for (int i = start; i < end; ++i) {
      const int64_t position = timeline_sample + i - clip.start_sample;
      int64_t local = local_position(clip, timeline_sample + i);
      if (local < 0 || local >= clip.buffer.num_samples) continue;
      const float gain = clip.gain * fade_gain(clip, position, clip.fade_curve);
      for (int ch = 0; ch < channels_to_copy; ++ch) {
        if (!channels[ch] || !clip.buffer.channels[ch]) continue;
        channels[ch][i] += clip.buffer.channels[ch][local] * gain;
      }
    }
  }
}

void ClipPlayer::collect_boundaries(int64_t block_start_sample, int num_frames,
                                    ClipBoundaryList* out) const noexcept {
  if (!out) return;
  out->clear();
  if (num_frames <= 0) return;
  const int64_t block_end = block_start_sample + num_frames;
  // Idempotent re-acquire so standalone callers (and the engine block-start
  // acquire_clips()) both see the published set; a wait-free pointer swap.
  clips_.acquire();
  const std::vector<ClipSchedule>* clips = clips_.current();
  if (!clips) return;
  for (const ClipSchedule& clip : *clips) {
    const int64_t start = clip.start_sample;
    const int64_t end = clip.start_sample + clip.length_samples;
    if (start > block_start_sample && start <= block_end) {
      out->add(static_cast<int>(start - block_start_sample));
    }
    if (end > block_start_sample && end <= block_end) {
      out->add(static_cast<int>(end - block_start_sample));
    }
  }
  out->sort_unique();
}

size_t ClipPlayer::clip_count() const noexcept {
  // Lock-free read of the control-thread-published count. Must NOT call
  // clips_.acquire() here: acquire() is the audio thread's exclusive
  // single-consumer operation on the RtPublisher SPSC rings, and clip_count()
  // is reachable from the control/host thread (C ABI, WASM) during playback.
  return clip_count_.load(std::memory_order_relaxed);
}

float ClipPlayer::fade_gain(const ClipSchedule& clip, int64_t position, FadeCurve curve) noexcept {
  // Linear-amplitude fraction in [0, 1] for the current position.
  float fraction = 1.0f;
  if (clip.fade_in_samples > 0 && position < clip.fade_in_samples) {
    fraction *= static_cast<float>(position) / static_cast<float>(clip.fade_in_samples);
  }
  if (clip.fade_out_samples > 0) {
    const int64_t fade_start = clip.length_samples - clip.fade_out_samples;
    if (position >= fade_start) {
      fraction *= static_cast<float>(std::max<int64_t>(0, clip.length_samples - position)) /
                  static_cast<float>(clip.fade_out_samples);
    }
  }
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  if (curve == FadeCurve::EqualPower) {
    // sin(pi/2 * x) holds constant energy across symmetric crossfades.
    return std::sin(kHalfPi * fraction);
  }
  return fraction;
}

int64_t ClipPlayer::local_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept {
  const int64_t position = timeline_sample - clip.start_sample;
  if (position < 0 || position >= clip.length_samples) return -1;
  const int64_t source_len =
      std::min<int64_t>(clip.length_samples, clip.buffer.num_samples - clip.clip_offset_samples);
  if (source_len <= 0) return -1;
  if (clip.loop) {
    return clip.clip_offset_samples + (position % source_len);
  }
  return clip.clip_offset_samples + position;
}

}  // namespace sonare::engine
