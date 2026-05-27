#include "engine/capture.h"

#include <algorithm>

namespace sonare::engine {

void CaptureSink::prepare(CaptureSegment segment) noexcept {
  segment_ = segment;
  reset();
}

void CaptureSink::arm(bool armed) noexcept { armed_ = armed; }

void CaptureSink::set_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept {
  punch_start_sample_ = start_sample;
  punch_end_sample_ = end_sample;
  punch_enabled_ = enabled && end_sample > start_sample;
}

void CaptureSink::reset() noexcept {
  captured_frames_ = 0;
  overflow_count_ = 0;
}

void CaptureSink::process(const float* const* input, int num_channels, int num_frames,
                          int64_t timeline_sample) noexcept {
  if (!armed_ || !input || !segment_.channels || num_channels <= 0 || num_frames <= 0 ||
      segment_.num_channels <= 0 || segment_.capacity_frames <= 0) {
    return;
  }

  const int channels = std::min(num_channels, segment_.num_channels);
  for (int i = 0; i < num_frames; ++i) {
    const int64_t sample = timeline_sample + i;
    if (punch_enabled_ && (sample < punch_start_sample_ || sample >= punch_end_sample_)) {
      continue;
    }
    if (captured_frames_ >= segment_.capacity_frames) {
      ++overflow_count_;
      continue;
    }
    for (int ch = 0; ch < channels; ++ch) {
      if (!input[ch] || !segment_.channels[ch]) continue;
      segment_.channels[ch][captured_frames_] = input[ch][i];
    }
    ++captured_frames_;
  }
}

void CaptureBoundaryList::clear() noexcept {
  size = 0;
  overflowed = false;
}

bool CaptureBoundaryList::add(int offset) noexcept {
  if (size >= offsets.size()) {
    overflowed = true;
    return false;
  }
  offsets[size++] = offset;
  return true;
}

void CaptureBoundaryList::sort_unique() noexcept {
  std::sort(offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(size));
  size_t out = 0;
  for (size_t i = 0; i < size; ++i) {
    if (out == 0 || offsets[i] != offsets[out - 1]) offsets[out++] = offsets[i];
  }
  size = out;
}

void collect_capture_boundaries(int64_t block_start_sample, int num_frames, int64_t punch_start,
                                int64_t punch_end, CaptureBoundaryList* out) noexcept {
  if (!out) return;
  out->clear();
  if (num_frames <= 0) return;
  const int64_t block_end = block_start_sample + num_frames;
  if (punch_start > block_start_sample && punch_start <= block_end) {
    out->add(static_cast<int>(punch_start - block_start_sample));
  }
  if (punch_end > block_start_sample && punch_end <= block_end) {
    out->add(static_cast<int>(punch_end - block_start_sample));
  }
  out->sort_unique();
}

}  // namespace sonare::engine
