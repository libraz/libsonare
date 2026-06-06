#include "engine/capture.h"

#include <algorithm>

namespace sonare::engine {

void CaptureSink::prepare(CaptureSegment segment) noexcept {
  control_state_.segment = segment;
  control_.store(control_state_);  // publish the whole snapshot atomically
  reset();
}

void CaptureSink::arm(bool armed) noexcept {
  control_state_.armed = armed;
  control_.store(control_state_);
}

void CaptureSink::set_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept {
  control_state_.punch_start_sample = start_sample;
  control_state_.punch_end_sample = end_sample;
  control_state_.punch_enabled = enabled && end_sample > start_sample;
  control_.store(control_state_);
}

void CaptureSink::reset() noexcept {
  captured_frames_.store(0, std::memory_order_relaxed);
  overflow_count_.reset();
}

void CaptureSink::process(const float* const* input, int num_channels, int num_frames,
                          int64_t timeline_sample) noexcept {
  // Read a single consistent snapshot of the published control state. The
  // seqlock returns either the old or new whole snapshot, never a torn mix, when
  // racing a control-thread store().
  const Control control = snapshot_rt();
  if (!control.armed || !input || !control.segment.channels || num_channels <= 0 ||
      num_frames <= 0 || control.segment.num_channels <= 0 ||
      control.segment.capacity_frames <= 0) {
    return;
  }

  const int channels = std::min(num_channels, control.segment.num_channels);
  for (int i = 0; i < num_frames; ++i) {
    const int64_t sample = timeline_sample + i;
    if (control.punch_enabled &&
        (sample < control.punch_start_sample || sample >= control.punch_end_sample)) {
      continue;
    }
    const int64_t captured = captured_frames_.load(std::memory_order_relaxed);
    if (captured >= control.segment.capacity_frames) {
      overflow_count_.bump();
      continue;
    }
    for (int ch = 0; ch < channels; ++ch) {
      if (!input[ch] || !control.segment.channels[ch]) continue;
      control.segment.channels[ch][captured] = input[ch][i];
    }
    captured_frames_.store(captured + 1, std::memory_order_relaxed);
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
