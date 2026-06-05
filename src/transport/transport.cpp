#include "transport/transport.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::transport {
namespace {

const TempoMap& fallback_tempo_map() noexcept {
  static TempoMap map;
  return map;
}

const TempoMap& map_or_fallback(const TempoMap* map) noexcept {
  return map ? *map : fallback_tempo_map();
}

TransportState make_snapshot(const TempoMap& map, double sample_rate, bool playing, bool looping,
                             int64_t render_frame, int64_t sample_position, double loop_start_ppq,
                             double loop_end_ppq) noexcept {
  const double ppq = map.sample_to_ppq(sample_position);
  const TimeSignature sig = map.time_signature_at_ppq(ppq);
  return {playing,
          looping,
          render_frame,
          sample_position,
          ppq,
          map.bpm_at_sample(sample_position),
          map.bar_start_ppq(ppq),
          map.ppq_to_bar_beat(ppq).bar,
          sig,
          loop_start_ppq,
          loop_end_ppq,
          sample_rate};
}

}  // namespace

bool BoundaryList::add(Boundary boundary) noexcept {
  if (size_ >= items_.size()) {
    overflowed_ = true;
    return false;
  }
  items_[size_++] = boundary;
  return true;
}

void Transport::prepare(double sample_rate, const TempoMap* tempo_map) {
  sample_rate_.store(sample_rate > 0.0 ? sample_rate : constants::kDefaultDawSampleRate,
                     std::memory_order_release);
  tempo_map_.store(tempo_map ? tempo_map : &fallback_tempo_map(), std::memory_order_release);
  render_frame_.store(0, std::memory_order_release);
  sample_position_.store(0, std::memory_order_release);
  playing_.store(false, std::memory_order_release);
  loop_overflow_count_.store(0, std::memory_order_relaxed);
  loop_state_.store({});
}

TransportState Transport::snapshot() const noexcept {
  const TempoMap& map = map_or_fallback(tempo_map_.load(std::memory_order_acquire));
  const LoopState loop = loop_state_.try_load();
  return make_snapshot(map, sample_rate_.load(std::memory_order_acquire), playing(), loop.enabled,
                       render_frame(), sample_position(), loop.start_ppq, loop.end_ppq);
}

TransportState Transport::snapshot_control() const noexcept {
  const TempoMap& map = map_or_fallback(tempo_map_.load(std::memory_order_acquire));
  const LoopState loop = loop_state_.load();
  return make_snapshot(map, sample_rate_.load(std::memory_order_acquire), playing(), loop.enabled,
                       render_frame(), sample_position(), loop.start_ppq, loop.end_ppq);
}

void Transport::advance(int num_frames) noexcept {
  const int frames = std::max(num_frames, 0);
  render_frame_.fetch_add(frames, std::memory_order_acq_rel);
  if (!playing() || frames == 0) return;

  int64_t position = sample_position_.load(std::memory_order_acquire) + frames;

  const TempoMap& map = map_or_fallback(tempo_map_.load(std::memory_order_acquire));
  const LoopState loop = loop_state_.try_load();
  if (!loop.enabled || loop.end_ppq <= loop.start_ppq) {
    sample_position_.store(position, std::memory_order_release);
    return;
  }

  const int64_t loop_start = map.ppq_to_sample(loop.start_ppq);
  const int64_t loop_end = map.ppq_to_sample(loop.end_ppq);
  const int64_t loop_len = loop_end - loop_start;
  if (loop_len <= 0) {
    sample_position_.store(position, std::memory_order_release);
    return;
  }

  while (position >= loop_end) {
    position = loop_start + (position - loop_end);
  }
  sample_position_.store(position, std::memory_order_release);
}

void Transport::seek_sample(int64_t sample) noexcept {
  sample_position_.store(std::max<int64_t>(0, sample), std::memory_order_release);
}

void Transport::seek_ppq(double ppq) noexcept {
  const TempoMap& map = map_or_fallback(tempo_map_.load(std::memory_order_acquire));
  sample_position_.store(std::max<int64_t>(0, map.ppq_to_sample(ppq)), std::memory_order_release);
}

void Transport::set_loop(double start_ppq, double end_ppq, bool enabled) noexcept {
  // Reject non-finite bounds so NaN/Inf loop points never reach the audio
  // thread (a NaN end would also silently disable the loop while leaving the
  // raw NaN published).
  const bool valid = std::isfinite(start_ppq) && std::isfinite(end_ppq) && end_ppq > start_ppq;
  if (!valid) {
    loop_state_.store({0.0, 0.0, false});
    return;
  }
  loop_state_.store({start_ppq, end_ppq, enabled});
}

bool Transport::seek_marker(uint32_t marker_id, const MarkerMap& markers) noexcept {
  Marker marker{};
  if (!markers.marker_by_id(marker_id, &marker)) return false;
  seek_ppq(marker.ppq);
  return true;
}

bool Transport::set_loop_from_markers(uint32_t start_marker_id, uint32_t end_marker_id,
                                      const MarkerMap& markers) noexcept {
  Marker start{};
  Marker end{};
  if (!markers.marker_by_id(start_marker_id, &start) ||
      !markers.marker_by_id(end_marker_id, &end) || end.ppq <= start.ppq) {
    return false;
  }
  set_loop(start.ppq, end.ppq, true);
  return true;
}

bool Transport::collect_loop_boundaries(int num_frames, BoundaryList* out) const noexcept {
  if (!out) return false;
  out->clear();
  const LoopState loop = loop_state_.try_load();
  const bool is_playing = playing();
  const int64_t current_sample = sample_position();
  const int64_t current_render = render_frame();
  if (!is_playing || !loop.enabled || num_frames <= 0 || loop.end_ppq <= loop.start_ppq) {
    return false;
  }

  const TempoMap& map = map_or_fallback(tempo_map_.load(std::memory_order_acquire));
  const int64_t loop_start = map.ppq_to_sample(loop.start_ppq);
  const int64_t loop_end = map.ppq_to_sample(loop.end_ppq);
  const int64_t loop_len = loop_end - loop_start;
  if (loop_len <= 0) return false;

  // Report every loop wrap that falls inside this block, not just the first.
  // With a short loop and a large block the playhead can wrap multiple times,
  // and reporting only the first wrap would leave the over-wrapped tail of the
  // block rendering from the wrong position (or as silence). We walk the
  // wrap points by repeatedly subtracting loop_len from the running position.
  int64_t next_wrap_offset = loop_end - current_sample;
  bool added = false;
  // Use offset 0 when the playhead is already at or beyond loop_end at block
  // start, matching advance()'s `>= loop_end` wrap. If it starts beyond loop_end,
  // carry the overshoot forward so the following wrap lands one loop length
  // after the wrapped position rather than being skipped.
  if (current_sample >= loop_end) {
    const int64_t overshoot = (current_sample - loop_end) % loop_len;
    if (!out->add({0, current_render, loop_end})) {
      if (out->overflowed()) {
        loop_overflow_count_.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }
    added = true;
    next_wrap_offset = loop_len - overshoot;
    if (next_wrap_offset == 0) next_wrap_offset = loop_len;
  }
  while (next_wrap_offset <= num_frames) {
    const int offset = static_cast<int>(next_wrap_offset);
    if (!out->add({offset, current_render + offset, loop_end})) break;
    added = true;
    next_wrap_offset += loop_len;
  }
  // Surface a dropped-wrap as a diagnostic counter instead of silently
  // truncating. With a loop shorter than the block / kCapacity wraps the tail
  // of the block renders from the wrong position; the counter lets hosts detect
  // the misconfiguration. Mirrors AutomationEngine::collect_boundaries.
  if (out->overflowed()) {
    loop_overflow_count_.fetch_add(1, std::memory_order_relaxed);
  }
  return added;
}

}  // namespace sonare::transport
