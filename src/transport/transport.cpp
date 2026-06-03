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
  sample_rate_ = sample_rate > 0.0 ? sample_rate : constants::kDefaultDawSampleRate;
  tempo_map_ = tempo_map ? tempo_map : &fallback_tempo_map();
  render_frame_ = 0;
  sample_position_ = 0;
  playing_ = false;
  loop_overflow_count_.store(0, std::memory_order_relaxed);
  loop_state_.store({});
}

TransportState Transport::snapshot() const noexcept {
  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  const double ppq = map.sample_to_ppq(sample_position_);
  const TimeSignature sig = map.time_signature_at_ppq(ppq);
  const LoopState loop = loop_state_.try_load();
  return {playing_,
          loop.enabled,
          render_frame_,
          sample_position_,
          ppq,
          map.bpm_at_sample(sample_position_),
          map.bar_start_ppq(ppq),
          map.ppq_to_bar_beat(ppq).bar,
          sig,
          loop.start_ppq,
          loop.end_ppq,
          sample_rate_};
}

void Transport::advance(int num_frames) noexcept {
  const int frames = std::max(num_frames, 0);
  render_frame_ += frames;
  if (!playing_ || frames == 0) return;

  sample_position_ += frames;

  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  const LoopState loop = loop_state_.try_load();
  if (!loop.enabled || loop.end_ppq <= loop.start_ppq) return;

  const int64_t loop_start = map.ppq_to_sample(loop.start_ppq);
  const int64_t loop_end = map.ppq_to_sample(loop.end_ppq);
  const int64_t loop_len = loop_end - loop_start;
  if (loop_len <= 0) return;

  while (sample_position_ >= loop_end) {
    sample_position_ = loop_start + (sample_position_ - loop_end);
  }
}

void Transport::seek_sample(int64_t sample) noexcept {
  sample_position_ = std::max<int64_t>(0, sample);
}

void Transport::seek_ppq(double ppq) noexcept {
  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  sample_position_ = std::max<int64_t>(0, map.ppq_to_sample(ppq));
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
  if (!playing_ || !loop.enabled || num_frames <= 0 || loop.end_ppq <= loop.start_ppq) {
    return false;
  }

  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  const int64_t loop_start = map.ppq_to_sample(loop.start_ppq);
  const int64_t loop_end = map.ppq_to_sample(loop.end_ppq);
  const int64_t loop_len = loop_end - loop_start;
  if (loop_len <= 0) return false;

  // Report every loop wrap that falls inside this block, not just the first.
  // With a short loop and a large block the playhead can wrap multiple times,
  // and reporting only the first wrap would leave the over-wrapped tail of the
  // block rendering from the wrong position (or as silence). We walk the
  // wrap points by repeatedly subtracting loop_len from the running position.
  const int64_t block_end = sample_position_ + num_frames;
  int64_t position = sample_position_;
  int64_t next_wrap = loop_end;
  bool added = false;
  // Use >= (not >) so a playhead sitting exactly on loop_end at block start is
  // reported as a wrap at offset 0, matching advance()'s `>= loop_end` wrap.
  // Otherwise the sub-block splitter and the post-advance position snapshot
  // disagree on the exact-boundary frame (e.g. after seeking onto loop_end).
  while (next_wrap >= position && next_wrap <= block_end) {
    const int offset = static_cast<int>(next_wrap - sample_position_);
    if (!out->add({offset, render_frame_ + offset, loop_end})) break;
    added = true;
    // After this wrap the playhead jumps back to loop_start; the following
    // wrap occurs another loop_len of forward travel later.
    position = next_wrap;
    next_wrap += loop_len;
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
