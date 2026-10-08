#include "engine/boundary_splitter.h"

#include <algorithm>

#include "util/numeric_validation.h"

namespace sonare::engine {
namespace {

int clamp_offset(int offset, int num_frames) noexcept {
  return std::clamp(offset, 0, std::max(num_frames, 0));
}

}  // namespace

void BoundaryList::prepare(size_t capacity) {
  const size_t next_capacity = std::max(kCapacity, capacity);
  std::vector<BoundaryPoint> next_points;
  std::vector<size_t> next_offset_indices;
  if (next_capacity > kCapacity) {
    // prepare() runs on CONTROL. Keeping the vector sized means the AUDIO
    // append path only assigns into already-owned storage.
    next_points.resize(next_capacity);
    next_offset_indices.resize(next_capacity);
  }

  // Build both replacement buffers before changing the active capacity. A
  // failed allocation therefore leaves the previous storage selection and
  // bounds usable by the current renderer.
  prepared_points_.swap(next_points);
  prepared_offset_indices_.swap(next_offset_indices);
  capacity_limit_ = next_capacity;
  // The fresh index table is already zeroed; the old points address the old one.
  size_ = 0;
  clear();
}

void BoundaryList::clear() noexcept {
  forget_offset_indices();
  size_ = 0;
  overflowed_ = false;
  dropped_count_ = 0;
}

bool BoundaryList::add_offset(int offset, BoundarySource source,
                              const BoundaryBuildContext& context) noexcept {
  const int clamped = clamp_offset(offset, context.num_frames);
  return add_point({clamped, numeric::saturating_add(context.block_render_frame, int64_t{clamped}),
                    timeline_at_offset(clamped, context), boundary_source_mask(source)});
}

bool BoundaryList::add_point(BoundaryPoint point) noexcept { return append(point); }

void BoundaryList::sort_unique() noexcept {
  forget_offset_indices();
  BoundaryPoint* storage = points();
  std::sort(storage, storage + size_, [](const BoundaryPoint& a, const BoundaryPoint& b) {
    if (a.offset != b.offset) return a.offset < b.offset;
    return a.timeline_sample < b.timeline_sample;
  });

  size_t out = 0;
  for (size_t i = 0; i < size_; ++i) {
    if (out > 0 && storage[out - 1].offset == storage[i].offset) {
      storage[out - 1].sources |= storage[i].sources;
      // Prefer the later point's timeline at duplicate loop boundaries: it is
      // the start position of the next sub-block after a wrap.
      storage[out - 1].timeline_sample = storage[i].timeline_sample;
      storage[out - 1].render_frame = storage[i].render_frame;
    } else {
      storage[out++] = storage[i];
    }
  }
  size_ = out;
  rebuild_offset_indices();
}

bool BoundaryList::finalize(const BoundaryBuildContext& context) noexcept {
  add_offset(0, BoundarySource::kBlockStart, context);
  add_offset(context.num_frames, BoundarySource::kBlockEnd, context);
  for (size_t i = 0; i < size_; ++i) {
    points()[i].render_frame = context.block_render_frame + points()[i].offset;
    points()[i].timeline_sample = timeline_at_offset(points()[i].offset, context);
  }
  sort_unique();
  const bool start_ok = ensure_block_start(context);
  const bool end_ok = ensure_block_end(context);
  return start_ok && end_ok;
}

int64_t BoundaryList::timeline_at_offset(int offset, const BoundaryBuildContext& context) noexcept {
  if (context.loop_wrap && offset >= context.loop_wrap_offset) {
    const int64_t past_wrap = offset - context.loop_wrap_offset;
    // Fold the position past the first wrap back into one loop length so a
    // block long enough to wrap multiple times still maps each sub-block offset
    // to the right point on the looped timeline (not a runaway position past
    // loop_end). loop_len_samples <= 0 keeps the original single-wrap mapping.
    const int64_t within =
        context.loop_len_samples > 0 ? past_wrap % context.loop_len_samples : past_wrap;
    return numeric::saturating_add(context.loop_start_timeline_sample, within);
  }
  return numeric::saturating_add(context.block_timeline_sample, static_cast<int64_t>(offset));
}

bool BoundaryList::append(BoundaryPoint point) noexcept {
  BoundaryPoint* storage = points();
  // Merge while appending. A duplicate arriving after the prepared capacity
  // is full still carries its source bits into the existing point instead of
  // becoming a false overflow.
  const size_t existing = find_offset(point.offset);
  if (existing < size_) {
    BoundaryPoint& current = storage[existing];
    current.sources |= point.sources;
    current.render_frame = point.render_frame;
    current.timeline_sample = point.timeline_sample;
    return true;
  }
  if (size_ >= capacity_limit_) {
    overflowed_ = true;
    ++dropped_count_;
    return false;
  }
  const size_t index = size_;
  storage[index] = point;
  ++size_;
  if (capacity_limit_ > kCapacity && point.offset >= 0) {
    const size_t offset = static_cast<size_t>(point.offset);
    if (offset < prepared_offset_indices_.size()) {
      prepared_offset_indices_[offset] = index + 1;
    }
  }
  return true;
}

size_t BoundaryList::find_offset(int offset) const noexcept {
  if (capacity_limit_ > kCapacity && offset >= 0) {
    const size_t index = static_cast<size_t>(offset);
    if (index < prepared_offset_indices_.size()) {
      const size_t encoded = prepared_offset_indices_[index];
      // A prepared in-range offset has an authoritative map entry: zero means
      // this offset is absent, so return the append position immediately
      // instead of falling back to a scan over every existing point.
      return encoded == 0 ? size_ : encoded - 1;
    }
  }

  const BoundaryPoint* storage = points();
  for (size_t i = 0; i < size_; ++i) {
    if (storage[i].offset != offset) continue;
    return i;
  }
  return size_;
}

void BoundaryList::forget_offset_index(int offset) noexcept {
  if (offset < 0) return;
  const size_t index = static_cast<size_t>(offset);
  if (index < prepared_offset_indices_.size()) prepared_offset_indices_[index] = 0;
}

void BoundaryList::forget_offset_indices() noexcept {
  if (capacity_limit_ <= kCapacity) return;
  const BoundaryPoint* storage = points();
  for (size_t i = 0; i < size_; ++i) forget_offset_index(storage[i].offset);
}

void BoundaryList::rebuild_offset_indices() noexcept {
  // sort_unique() already reset the entries of every point it was given.
  if (capacity_limit_ <= kCapacity) return;
  const BoundaryPoint* storage = points();
  for (size_t i = 0; i < size_; ++i) {
    if (storage[i].offset < 0) continue;
    const size_t offset = static_cast<size_t>(storage[i].offset);
    if (offset < prepared_offset_indices_.size()) {
      prepared_offset_indices_[offset] = i + 1;
    }
  }
}

bool BoundaryList::ensure_block_start(const BoundaryBuildContext& context) noexcept {
  for (size_t i = 0; i < size_; ++i) {
    if (points()[i].offset == 0) {
      points()[i].sources |= boundary_source_mask(BoundarySource::kBlockStart);
      return true;
    }
  }

  const BoundaryPoint start{0, context.block_render_frame, timeline_at_offset(0, context),
                            boundary_source_mask(BoundarySource::kBlockStart)};
  if (size_ < capacity_limit_) {
    points()[size_] = start;
    ++size_;
    sort_unique();
    return true;
  }

  forget_offset_index(points()[0].offset);
  points()[0] = start;
  sort_unique();
  overflowed_ = true;
  ++dropped_count_;
  return false;
}

bool BoundaryList::ensure_block_end(const BoundaryBuildContext& context) noexcept {
  const int end_offset = std::max(context.num_frames, 0);
  for (size_t i = 0; i < size_; ++i) {
    if (points()[i].offset == end_offset) {
      points()[i].sources |= boundary_source_mask(BoundarySource::kBlockEnd);
      return true;
    }
  }

  const BoundaryPoint end{end_offset, context.block_render_frame + end_offset,
                          timeline_at_offset(end_offset, context),
                          boundary_source_mask(BoundarySource::kBlockEnd)};
  if (size_ < capacity_limit_) {
    points()[size_] = end;
    ++size_;
    sort_unique();
    return true;
  }

  forget_offset_index(points()[capacity_limit_ - 1].offset);
  points()[capacity_limit_ - 1] = end;
  sort_unique();
  overflowed_ = true;
  ++dropped_count_;
  return false;
}

void BoundarySplitter::prepare(size_t capacity) { boundaries_.prepare(capacity); }

void BoundarySplitter::begin(BoundaryBuildContext context) noexcept {
  context.num_frames = std::max(context.num_frames, 0);
  context.loop_wrap_offset = clamp_offset(context.loop_wrap_offset, context.num_frames);
  context_ = context;
  boundaries_.clear();
}

bool BoundarySplitter::add_loop(int offset) noexcept {
  const int clamped = clamp_offset(offset, context_.num_frames);
  // timeline_at_offset folds offsets relative to the FIRST wrap, so once a wrap
  // is recorded keep loop_wrap_offset at the earliest one. Later (multi-wrap)
  // calls only add their boundary points; they must not move the fold origin.
  if (!context_.loop_wrap) {
    context_.loop_wrap = true;
    context_.loop_wrap_offset = clamped;
  } else {
    context_.loop_wrap_offset = std::min(context_.loop_wrap_offset, clamped);
  }
  return boundaries_.add_offset(clamped, BoundarySource::kLoop, context_);
}

bool BoundarySplitter::add_command(int offset) noexcept {
  return boundaries_.add_offset(offset, BoundarySource::kCommand, context_);
}

bool BoundarySplitter::add_automation(int offset) noexcept {
  return boundaries_.add_offset(offset, BoundarySource::kAutomation, context_);
}

bool BoundarySplitter::add_clip(int offset) noexcept {
  return boundaries_.add_offset(offset, BoundarySource::kClip, context_);
}

bool BoundarySplitter::add_marker(int offset) noexcept {
  return boundaries_.add_offset(offset, BoundarySource::kMarker, context_);
}

bool BoundarySplitter::add_midi(int offset) noexcept {
  return boundaries_.add_offset(offset, BoundarySource::kMidi, context_);
}

const BoundaryList& BoundarySplitter::finish() noexcept {
  boundaries_.finalize(context_);
  return boundaries_;
}

}  // namespace sonare::engine
