#include "engine/clip_player.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "util/constants.h"

namespace sonare::engine {

using sonare::constants::kHalfPi;

namespace {
/// Linear balance law for a stereo output channel. pan in [-1, +1]: positive
/// (right) attenuates the left channel, negative (left) attenuates the right;
/// center and any channel beyond the first stereo pair are left at unity.
float pan_channel_gain(float pan, int channel) noexcept {
  pan = std::clamp(pan, -1.0f, 1.0f);
  if (channel == 0) return pan > 0.0f ? 1.0f - pan : 1.0f;
  if (channel == 1) return pan < 0.0f ? 1.0f + pan : 1.0f;
  return 1.0f;
}
}  // namespace

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
  const size_t count = clips.size();
  if (clips_.publish(std::make_shared<const std::vector<ClipSchedule>>(std::move(clips)))) {
    clip_count_.store(count, std::memory_order_relaxed);
  }
}

void ClipPlayer::process_at(float* const* channels, int num_channels, int num_samples,
                            int64_t timeline_sample) noexcept {
  process_filtered_at(0, nullptr, 0, TrackFilterMode::kAll, channels, num_channels, num_samples,
                      timeline_sample);
}

void ClipPlayer::process_track_at(uint32_t track_id, float* const* channels, int num_channels,
                                  int num_samples, int64_t timeline_sample) noexcept {
  if (track_id == 0) return;
  process_filtered_at(track_id, nullptr, 0, TrackFilterMode::kOnlyTrack, channels, num_channels,
                      num_samples, timeline_sample);
}

void ClipPlayer::process_excluding_tracks_at(const uint32_t* track_ids, size_t track_count,
                                             float* const* channels, int num_channels,
                                             int num_samples, int64_t timeline_sample) noexcept {
  process_filtered_at(0, track_ids, track_count, TrackFilterMode::kExcludeTracks, channels,
                      num_channels, num_samples, timeline_sample);
}

void ClipPlayer::process_filtered_at(uint32_t track_id, const uint32_t* track_ids,
                                     size_t track_count, TrackFilterMode filter_mode,
                                     float* const* channels, int num_channels, int num_samples,
                                     int64_t timeline_sample) noexcept {
  if (!channels || num_channels <= 0 || num_samples <= 0) return;
  const bool scoped_page_miss_block = !external_page_miss_block_;
  if (!external_page_miss_block_) {
    begin_page_miss_block();
  }

  // Adopt the latest published clip set. The engine drives a single block-start
  // acquire_clips() so a clip set is never swapped mid-block between sub-blocks;
  // this idempotent re-acquire (a wait-free pointer swap, no alloc) keeps the
  // standalone ClipPlayer contract working (set_clips then process_at directly).
  clips_.acquire();
  const std::vector<ClipSchedule>* clips = clips_.current();
  if (!clips) {
    if (scoped_page_miss_block) end_page_miss_block();
    return;
  }

  for (const ClipSchedule& clip : *clips) {
    if (filter_mode == TrackFilterMode::kOnlyTrack) {
      if (clip.track_id != track_id) continue;
    } else if (filter_mode == TrackFilterMode::kExcludeTracks) {
      bool matched = false;
      for (size_t i = 0; i < track_count; ++i) {
        if (clip.track_id == track_ids[i]) {
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
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
      const LoopRead read = resolve_loop_read(clip, timeline_sample + i);
      const double source_pos = read.pos;
      if (!(source_pos >= 0.0) ||
          source_pos >= static_cast<double>(std::max<int64_t>(source_sample_count(clip), 0))) {
        continue;
      }
      // One source-channel read, summing the loop-seam crossfade partner when
      // active (partner_gain == 0 for the common single-read path, so this is the
      // unchanged read otherwise).
      const auto read_source = [&](int src_ch) noexcept {
        float value = sample_channel(clip, src_ch, source_pos) * read.gain;
        if (read.partner_gain > 0.0f) {
          value += sample_channel(clip, src_ch, read.partner) * read.partner_gain;
        }
        return value;
      };
      const float gain = clip.gain * fade_gain(clip, position);
      if (num_channels == 1) {
        if (!channels[0]) continue;
        // Mono live monitoring previews the mono BOUNCE: it renders the panned
        // stereo pair (clip pan applied per channel, a mono source fanned to
        // both channels, a stereo/multichannel source split L/R) and downmixes
        // 0.5*(L+R), matching project_bounce.cpp. With the linear balance law a
        // centered clip (pan=0) leaves both channels at unity, so 0.5*(L+R)
        // collapses to the source read and only off-center clips change — live
        // and mono bounce now agree in level and balance for any pan.
        const int last_src = source_channel_count(clip) - 1;
        float lr = 0.0f;
        for (int ch = 0; ch < 2; ++ch) {
          const int src_ch = std::min(ch, last_src);
          lr += read_source(src_ch) * pan_channel_gain(clip.pan, ch);
        }
        channels[0][i] += 0.5f * lr * gain;
        continue;
      }
      for (int ch = 0; ch < num_channels; ++ch) {
        if (!channels[ch]) continue;
        const int src_ch = std::min(ch, source_channel_count(clip) - 1);
        const float ch_gain = gain * pan_channel_gain(clip.pan, ch);
        channels[ch][i] += read_source(src_ch) * ch_gain;
      }
    }
  }
  if (scoped_page_miss_block) end_page_miss_block();
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
  // Clamp each fade to the clip's own length. An oversized fade-out would
  // otherwise drive the fade-out start (fade_length - fade_out_samples) negative,
  // so every sample falls inside the fade ramp and the whole clip attenuates.
  const int64_t fade_in_samples = std::min(std::max<int64_t>(0, clip.fade_in_samples), fade_length);
  const int64_t fade_out_samples =
      std::min(std::max<int64_t>(0, clip.fade_out_samples), fade_length);
  if (fade_in_samples > 0 && fade_position < fade_in_samples) {
    const float fraction = static_cast<float>(fade_position) / static_cast<float>(fade_in_samples);
    gain *= curve_gain(fraction, clip.fade_in_curve);
  }
  if (fade_out_samples > 0) {
    const int64_t fade_start = fade_length - fade_out_samples;
    if (fade_position >= fade_start) {
      const float fraction = static_cast<float>(std::max<int64_t>(0, fade_length - fade_position)) /
                             static_cast<float>(fade_out_samples);
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
    const auto it = std::upper_bound(
        anchors.begin(), anchors.end(), warp_sample,
        [](double sample, const WarpAnchor& anchor) { return sample < anchor.warp_sample; });
    next = &*it;
    prev = &*(it - 1);
  }
  const double warp_span = next->warp_sample - prev->warp_sample;
  const double source_span = next->source_sample - prev->source_sample;
  if (!(warp_span > 0.0) || !(source_span > 0.0)) return warp_sample;
  return prev->source_sample + (warp_sample - prev->warp_sample) * (source_span / warp_span);
}

double ClipPlayer::source_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept {
  return resolve_loop_read(clip, timeline_sample).pos;
}

ClipPlayer::LoopRead ClipPlayer::resolve_loop_read(const ClipSchedule& clip,
                                                   int64_t timeline_sample) noexcept {
  LoopRead read;
  if (clip.warp_mode == WarpMode::kTempoSync) return read;
  const int64_t position = timeline_sample - clip.start_sample;
  if (position < 0 || position >= clip.length_samples) return read;
  // The warp anchors map a clip-timeline position (measured from clip start) to an
  // absolute source position, so under active warp the map alone resolves the read
  // — clip_offset_samples must NOT be added on top. For a comp part that starts
  // mid-clip, warp_reference_offset_samples carries the part's clip-timeline onset,
  // so map_warp(warp_reference_offset_samples + position) already yields the source
  // the warp curve assigns to that timeline region. Adding clip_offset_samples
  // there (the non-warp source-start mechanism) double-counts the onset offset and
  // a non-first comp part reads too far into the source. A whole clip leaves both
  // offsets at 0, so the warped and non-warped paths agree.
  const bool warp_active =
      clip.warp_mode == WarpMode::kRepitch && clip.warp_anchors && clip.warp_anchors->size() >= 2;
  // Under active warp the source read is driven entirely by the warp map, so
  // clip_offset_samples is NOT consumed here — subtracting it (as the non-warp path
  // does) would wrongly drive source_len <= 0 and silence a comp part whose
  // clip_offset_samples approaches the source length. The outer read guard
  // (source_pos vs source_sample_count) still bounds the actual reads.
  const int64_t source_len =
      warp_active ? std::min<int64_t>(clip.length_samples, source_sample_count(clip))
                  : std::min<int64_t>(clip.length_samples,
                                      source_sample_count(clip) - clip.clip_offset_samples);
  if (source_len <= 0) return read;
  const double warp_ref =
      static_cast<double>(std::max<int64_t>(0, clip.warp_reference_offset_samples));
  const auto resolve = [&clip, warp_active, warp_ref](double pos) noexcept {
    if (warp_active) {
      return map_warp_to_source(*clip.warp_anchors, pos + warp_ref);
    }
    return static_cast<double>(clip.clip_offset_samples) + pos;
  };
  if (clip.loop) {
    const int64_t loop_len = clip.loop_length_samples > 0
                                 ? std::min<int64_t>(clip.loop_length_samples, source_len)
                                 : source_len;
    const int64_t local = position % loop_len;
    read.pos = resolve(static_cast<double>(local));
    // Optional loop-seam crossfade: blend the loop tail with the source material
    // immediately before the loop start (pre-roll), so a non-zero-aligned seam
    // does not click. Period-preserving (the loop still repeats every loop_len),
    // so it needs loop_crossfade_samples of pre-roll before the loop start and is
    // clamped to the available pre-roll (clip_offset_samples) and to half the
    // loop. Disabled under warp; when no pre-roll is available it falls back to
    // the hard integer-modulo wrap (the common DAW hard-loop contract).
    if (clip.loop_crossfade_samples > 0 && !warp_active) {
      const int64_t xfade =
          std::min({clip.loop_crossfade_samples, clip.clip_offset_samples, loop_len / 2});
      if (xfade > 0 && local >= loop_len - xfade) {
        const double frac =
            static_cast<double>(local - (loop_len - xfade)) / static_cast<double>(xfade);
        // Pre-roll partner: clip_offset + (local - loop_len) lands in
        // [clip_offset - xfade, clip_offset), guaranteed valid by the clamp above.
        read.partner = resolve(static_cast<double>(local - loop_len));
        read.gain = static_cast<float>(std::cos(frac * kHalfPi));
        read.partner_gain = static_cast<float>(std::sin(frac * kHalfPi));
      }
    }
    return read;
  }
  read.pos = resolve(static_cast<double>(position));
  return read;
}

int ClipPlayer::source_channel_count(const ClipSchedule& clip) noexcept {
  if (clip.page_provider) return clip.page_provider->num_channels();
  return clip.buffer.num_channels;
}

int64_t ClipPlayer::source_sample_count(const ClipSchedule& clip) noexcept {
  if (clip.page_provider) return clip.page_provider->num_samples();
  return clip.buffer.num_samples;
}

void ClipPlayer::begin_page_miss_block() noexcept {
  external_page_miss_block_ = true;
  page_miss_cache_size_ = 0;
  page_miss_cache_overflowed_ = false;
}

void ClipPlayer::notify_page_miss(const ClipSchedule& clip, int src_ch, int64_t sample) noexcept {
  if (!page_request_sink_ || !clip.page_provider) return;
  const int64_t frames = std::max<int64_t>(clip.page_provider->page_frames(), 1);
  const int64_t page_index = sample >= 0 ? sample / frames : sample;
  const uint32_t channel = static_cast<uint32_t>(std::max(src_ch, 0));
  for (size_t i = 0; i < page_miss_cache_size_; ++i) {
    const PageMissCacheEntry& entry = page_miss_cache_[i];
    if (entry.clip_id == clip.id && entry.channel == channel && entry.page_index == page_index) {
      return;
    }
  }
  if (!page_miss_cache_overflowed_) {
    if (page_miss_cache_size_ < page_miss_cache_.size()) {
      page_miss_cache_[page_miss_cache_size_++] = {clip.id, channel, page_index};
    } else {
      page_miss_cache_overflowed_ = true;
    }
  }
  page_request_sink_->on_clip_page_miss({clip.id, channel, sample});
}

float ClipPlayer::sample_channel(const ClipSchedule& clip, int src_ch, double source_pos) noexcept {
  const int source_channels = source_channel_count(clip);
  if (src_ch < 0 || src_ch >= source_channels) {
    return 0.0f;
  }
  const auto last = static_cast<int64_t>(source_sample_count(clip) - 1);
  if (last < 0) return 0.0f;
  const auto read = [this, &clip, src_ch](int64_t sample, float* out) noexcept {
    if (clip.page_provider) {
      if (out && clip.page_provider->sample_at(src_ch, sample, out)) {
        return true;
      }
      notify_page_miss(clip, src_ch, sample);
      return false;
    }
    if (!out || !clip.buffer.channels || !clip.buffer.channels[src_ch]) return false;
    *out = clip.buffer.channels[src_ch][sample];
    return true;
  };
  float a = 0.0f;
  if (source_pos <= 0.0) return read(0, &a) ? a : 0.0f;
  if (source_pos >= static_cast<double>(last)) return read(last, &a) ? a : 0.0f;
  const auto left = static_cast<int64_t>(std::floor(source_pos));
  const auto right = std::min<int64_t>(left + 1, last);
  const float frac = static_cast<float>(source_pos - static_cast<double>(left));
  float b = 0.0f;
  const bool has_a = read(left, &a);
  if (frac <= 1.0e-7f) return has_a ? a : 0.0f;
  const bool has_b = read(right, &b);
  if (!has_a && !has_b) return 0.0f;
  if (!has_a) return b;
  if (!has_b) return a;
  return a + (b - a) * frac;
}

}  // namespace sonare::engine
