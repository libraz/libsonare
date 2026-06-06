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

void ClipPlayer::set_clips(std::vector<ClipSchedule> clips,
                           const transport::TempoMap* tempo_map_override) {
  const transport::TempoMap* map = tempo_map_override ? tempo_map_override : tempo_map_;
  if (map) {
    for (ClipSchedule& clip : clips) {
      clip.start_sample = map->ppq_to_sample(clip.start_ppq);
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
    if (source_channel_count(clip) <= 0 || source_sample_count(clip) <= 0 ||
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
    for (int i = start; i < end; ++i) {
      const int64_t position = timeline_sample + i - clip.start_sample;
      const double source_pos = source_position(clip, timeline_sample + i);
      if (!(source_pos >= 0.0) ||
          source_pos >= static_cast<double>(std::max<int64_t>(source_sample_count(clip), 0))) {
        continue;
      }
      const float gain = clip.gain * fade_gain(clip, position);
      if (num_channels == 1) {
        if (!channels[0]) continue;
        float mono = 0.0f;
        int contributing = 0;
        const int source_channels = source_channel_count(clip);
        for (int src_ch = 0; src_ch < source_channels; ++src_ch) {
          const float sample = sample_channel(clip, src_ch, source_pos);
          mono += sample;
          ++contributing;
        }
        if (contributing > 0) {
          channels[0][i] += (mono / static_cast<float>(contributing)) * gain;
        }
        continue;
      }
      for (int ch = 0; ch < num_channels; ++ch) {
        if (!channels[ch]) continue;
        const int src_ch = std::min(ch, source_channel_count(clip) - 1);
        channels[ch][i] += sample_channel(clip, src_ch, source_pos) * gain;
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

float curve_gain(float fraction, FadeCurve curve) noexcept {
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  switch (curve) {
    case FadeCurve::EqualPower:
      return std::sin(kHalfPi * fraction);
    case FadeCurve::Exponential:
      return fraction * fraction;
    case FadeCurve::Logarithmic:
      return std::sqrt(fraction);
    case FadeCurve::Linear:
    default:
      return fraction;
  }
}

float ClipPlayer::fade_gain(const ClipSchedule& clip, int64_t position) noexcept {
  float gain = 1.0f;
  const int64_t fade_position = position + std::max<int64_t>(0, clip.fade_reference_offset_samples);
  const int64_t fade_length = clip.fade_reference_length_samples > 0
                                  ? clip.fade_reference_length_samples
                                  : clip.length_samples;
  if (clip.fade_in_samples > 0 && fade_position < clip.fade_in_samples) {
    const float fraction =
        static_cast<float>(fade_position) / static_cast<float>(clip.fade_in_samples);
    gain *= curve_gain(fraction, clip.fade_in_curve);
  }
  if (clip.fade_out_samples > 0) {
    const int64_t fade_start = fade_length - clip.fade_out_samples;
    if (fade_position >= fade_start) {
      const float fraction = static_cast<float>(std::max<int64_t>(0, fade_length - fade_position)) /
                             static_cast<float>(clip.fade_out_samples);
      gain *= curve_gain(fraction, clip.fade_out_curve);
    }
  }
  return std::clamp(gain, 0.0f, 1.0f);
}

int64_t ClipPlayer::local_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept {
  const double source_pos = source_position(clip, timeline_sample);
  if (!(source_pos >= 0.0)) return -1;
  const double rounded = std::round(source_pos);
  if (std::abs(source_pos - rounded) > 1.0e-9) return -1;
  return static_cast<int64_t>(rounded);
}

double map_warp_to_source(const std::vector<WarpAnchor>& anchors, double warp_sample) noexcept {
  if (anchors.size() < 2) return warp_sample;
  const WarpAnchor* prev = &anchors.front();
  const WarpAnchor* next = &anchors[1];
  if (warp_sample <= anchors.front().warp_sample) {
    prev = &anchors.front();
    next = &anchors[1];
  } else if (warp_sample >= anchors.back().warp_sample) {
    prev = &anchors[anchors.size() - 2];
    next = &anchors.back();
  } else {
    for (size_t i = 1; i < anchors.size(); ++i) {
      if (warp_sample <= anchors[i].warp_sample) {
        prev = &anchors[i - 1];
        next = &anchors[i];
        break;
      }
    }
  }
  const double warp_span = next->warp_sample - prev->warp_sample;
  const double source_span = next->source_sample - prev->source_sample;
  if (!(warp_span > 0.0) || !(source_span > 0.0)) return warp_sample;
  return prev->source_sample + (warp_sample - prev->warp_sample) * (source_span / warp_span);
}

double ClipPlayer::source_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept {
  const int64_t position = timeline_sample - clip.start_sample;
  if (position < 0 || position >= clip.length_samples) return -1;
  const int64_t source_len =
      std::min<int64_t>(clip.length_samples, source_sample_count(clip) - clip.clip_offset_samples);
  if (source_len <= 0) return -1;
  const auto map_position = [&clip](double pos) noexcept {
    if (clip.warp_mode == WarpMode::kRepitch && clip.warp_anchors &&
        clip.warp_anchors->size() >= 2) {
      return map_warp_to_source(
          *clip.warp_anchors,
          pos + static_cast<double>(std::max<int64_t>(0, clip.warp_reference_offset_samples)));
    }
    return pos;
  };
  if (clip.loop) {
    const int64_t loop_len = clip.loop_length_samples > 0
                                 ? std::min<int64_t>(clip.loop_length_samples, source_len)
                                 : source_len;
    return static_cast<double>(clip.clip_offset_samples) +
           map_position(static_cast<double>(position % loop_len));
  }
  return static_cast<double>(clip.clip_offset_samples) +
         map_position(static_cast<double>(position));
}

int ClipPlayer::source_channel_count(const ClipSchedule& clip) noexcept {
  if (clip.page_provider) return clip.page_provider->num_channels();
  return clip.buffer.num_channels;
}

int64_t ClipPlayer::source_sample_count(const ClipSchedule& clip) noexcept {
  if (clip.page_provider) return clip.page_provider->num_samples();
  return clip.buffer.num_samples;
}

float ClipPlayer::sample_channel(const ClipSchedule& clip, int src_ch, double source_pos) noexcept {
  const int source_channels = source_channel_count(clip);
  if (src_ch < 0 || src_ch >= source_channels) {
    return 0.0f;
  }
  const auto last = static_cast<int64_t>(source_sample_count(clip) - 1);
  if (last < 0) return 0.0f;
  const auto read = [this, &clip, src_ch](int64_t sample) noexcept {
    if (clip.page_provider) {
      float out = 0.0f;
      if (clip.page_provider->sample_at(src_ch, sample, &out)) return out;
      if (page_request_sink_) {
        page_request_sink_->on_clip_page_miss(
            {clip.id, static_cast<uint32_t>(std::max(src_ch, 0)), sample});
      }
      return 0.0f;
    }
    if (!clip.buffer.channels || !clip.buffer.channels[src_ch]) return 0.0f;
    return clip.buffer.channels[src_ch][sample];
  };
  if (source_pos <= 0.0) return read(0);
  if (source_pos >= static_cast<double>(last)) return read(last);
  const auto left = static_cast<int64_t>(std::floor(source_pos));
  const auto right = std::min<int64_t>(left + 1, last);
  const float frac = static_cast<float>(source_pos - static_cast<double>(left));
  const float a = read(left);
  const float b = read(right);
  return a + (b - a) * frac;
}

}  // namespace sonare::engine
