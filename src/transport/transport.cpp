#include "transport/transport.h"

#include <algorithm>

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
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  tempo_map_ = tempo_map ? tempo_map : &fallback_tempo_map();
  render_frame_ = 0;
  sample_position_ = 0;
  playing_ = false;
  looping_ = false;
  loop_start_ppq_ = 0.0;
  loop_end_ppq_ = 0.0;
}

TransportState Transport::snapshot() const noexcept {
  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  const double ppq = map.sample_to_ppq(sample_position_);
  const TimeSignature sig = map.time_signature_at_ppq(ppq);
  return {playing_,
          looping_,
          render_frame_,
          sample_position_,
          ppq,
          map.bpm_at_sample(sample_position_),
          map.bar_start_ppq(ppq),
          map.ppq_to_bar_beat(ppq).bar,
          sig,
          loop_start_ppq_,
          loop_end_ppq_,
          sample_rate_};
}

void Transport::advance(int num_frames) noexcept {
  const int frames = std::max(num_frames, 0);
  render_frame_ += frames;
  if (!playing_ || frames == 0) return;

  sample_position_ += frames;

  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  if (!looping_ || loop_end_ppq_ <= loop_start_ppq_) return;

  const int64_t loop_start = map.ppq_to_sample(loop_start_ppq_);
  const int64_t loop_end = map.ppq_to_sample(loop_end_ppq_);
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
  loop_start_ppq_ = start_ppq;
  loop_end_ppq_ = end_ppq;
  looping_ = enabled && end_ppq > start_ppq;
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
  if (!playing_ || !looping_ || num_frames <= 0 || loop_end_ppq_ <= loop_start_ppq_) {
    return false;
  }

  const TempoMap& map = tempo_map_ ? *tempo_map_ : fallback_tempo_map();
  const int64_t loop_end = map.ppq_to_sample(loop_end_ppq_);
  if (sample_position_ < loop_end && sample_position_ + num_frames >= loop_end) {
    const int offset = static_cast<int>(loop_end - sample_position_);
    out->add({offset, render_frame_ + offset, loop_end});
    return true;
  }
  return false;
}

}  // namespace sonare::transport
