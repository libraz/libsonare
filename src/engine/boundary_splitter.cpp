#include "engine/boundary_splitter.h"

#include <algorithm>

namespace sonare::engine {
namespace {

int clamp_offset(int offset, int num_frames) noexcept {
  return std::clamp(offset, 0, std::max(num_frames, 0));
}

}  // namespace

void BoundaryList::clear() noexcept {
  size_ = 0;
  overflowed_ = false;
  dropped_count_ = 0;
}

bool BoundaryList::add_offset(int offset, BoundarySource source,
                              const BoundaryBuildContext& context) noexcept {
  const int clamped = clamp_offset(offset, context.num_frames);
  return add_point({clamped, context.block_render_frame + clamped,
                    timeline_at_offset(clamped, context), boundary_source_mask(source)});
}

bool BoundaryList::add_point(BoundaryPoint point) noexcept { return append(point); }

void BoundaryList::sort_unique() noexcept {
  std::sort(points_.begin(), points_.begin() + static_cast<std::ptrdiff_t>(size_),
            [](const BoundaryPoint& a, const BoundaryPoint& b) {
              if (a.offset != b.offset) return a.offset < b.offset;
              return a.timeline_sample < b.timeline_sample;
            });

  size_t out = 0;
  for (size_t i = 0; i < size_; ++i) {
    if (out > 0 && points_[out - 1].offset == points_[i].offset) {
      points_[out - 1].sources |= points_[i].sources;
      // Prefer the later point's timeline at duplicate loop boundaries: it is
      // the start position of the next sub-block after a wrap.
      points_[out - 1].timeline_sample = points_[i].timeline_sample;
      points_[out - 1].render_frame = points_[i].render_frame;
    } else {
      points_[out++] = points_[i];
    }
  }
  size_ = out;
}

bool BoundaryList::finalize(const BoundaryBuildContext& context) noexcept {
  add_offset(0, BoundarySource::kBlockStart, context);
  add_offset(context.num_frames, BoundarySource::kBlockEnd, context);
  for (size_t i = 0; i < size_; ++i) {
    points_[i].render_frame = context.block_render_frame + points_[i].offset;
    points_[i].timeline_sample = timeline_at_offset(points_[i].offset, context);
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
    return context.loop_start_timeline_sample + within;
  }
  return context.block_timeline_sample + offset;
}

bool BoundaryList::append(BoundaryPoint point) noexcept {
  if (size_ >= points_.size()) {
    overflowed_ = true;
    ++dropped_count_;
    return false;
  }
  points_[size_++] = point;
  return true;
}

bool BoundaryList::ensure_block_start(const BoundaryBuildContext& context) noexcept {
  for (size_t i = 0; i < size_; ++i) {
    if (points_[i].offset == 0) {
      points_[i].sources |= boundary_source_mask(BoundarySource::kBlockStart);
      return true;
    }
  }

  const BoundaryPoint start{0, context.block_render_frame, timeline_at_offset(0, context),
                            boundary_source_mask(BoundarySource::kBlockStart)};
  if (size_ < points_.size()) {
    points_[size_++] = start;
    sort_unique();
    return true;
  }

  points_[0] = start;
  sort_unique();
  overflowed_ = true;
  ++dropped_count_;
  return false;
}

bool BoundaryList::ensure_block_end(const BoundaryBuildContext& context) noexcept {
  const int end_offset = std::max(context.num_frames, 0);
  for (size_t i = 0; i < size_; ++i) {
    if (points_[i].offset == end_offset) {
      points_[i].sources |= boundary_source_mask(BoundarySource::kBlockEnd);
      return true;
    }
  }

  const BoundaryPoint end{end_offset, context.block_render_frame + end_offset,
                          timeline_at_offset(end_offset, context),
                          boundary_source_mask(BoundarySource::kBlockEnd)};
  if (size_ < points_.size()) {
    points_[size_++] = end;
    sort_unique();
    return true;
  }

  points_[points_.size() - 1] = end;
  sort_unique();
  overflowed_ = true;
  ++dropped_count_;
  return false;
}

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

const BoundaryList& BoundarySplitter::finish() noexcept {
  boundaries_.finalize(context_);
  return boundaries_;
}

}  // namespace sonare::engine
