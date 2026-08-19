#include "engine/warp_stretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/constants.h"

namespace sonare::engine {

using sonare::constants::kTwoPiD;

void WarpStretchVoice::prepare(int max_block_size, int channels) {
  channels_ = std::clamp(channels, 1, kMaxChannels);
  const int block = std::max(max_block_size, 1);
  // The frame loop stops once the next frame lands at or past the requested
  // count, so the last frame can begin one sample short of it and still span a
  // whole window.
  capacity_ = block + kSynthesisHop + kFrameSize;
  window_.assign(static_cast<size_t>(kFrameSize), 0.0f);
  for (int i = 0; i < kFrameSize; ++i) {
    // Periodic Hann: two 50%-overlapped copies sum to exactly one, so the
    // overlap-add needs no normalization pass.
    window_[static_cast<size_t>(i)] =
        static_cast<float>(0.5 - 0.5 * std::cos(kTwoPiD * i / kFrameSize));
  }
  for (int ch = 0; ch < kMaxChannels; ++ch) {
    overlap_[static_cast<size_t>(ch)].assign(static_cast<size_t>(capacity_), 0.0f);
  }
  match_.assign(static_cast<size_t>(kSynthesisHop), 0.0f);
  search_.assign(static_cast<size_t>(2 * kSearchRadius + kSynthesisHop + 1), 0.0f);
  reset();
}

void WarpStretchVoice::reset() noexcept {
  active_ = false;
  have_previous_ = false;
  clip_id_ = 0;
  idle_blocks_ = 0;
  next_output_ = 0;
  next_frame_offset_ = 0;
  filled_ = 0;
  for (int ch = 0; ch < kMaxChannels; ++ch) {
    std::fill(overlap_[static_cast<size_t>(ch)].begin(), overlap_[static_cast<size_t>(ch)].end(),
              0.0f);
  }
  std::fill(match_.begin(), match_.end(), 0.0f);
}

void WarpStretchVoice::mark_idle() noexcept {
  if (idle_blocks_ < 0xFFFFFFFFu) ++idle_blocks_;
}

int WarpStretchVoice::best_offset(int64_t desired, WarpSourceReader reader,
                                  void* context) noexcept {
  if (!have_previous_) return 0;
  // Copy the whole candidate span once. Correlating straight through the reader
  // would fan every candidate out into a paged-provider lookup per sample.
  const int span = 2 * kSearchRadius + kSynthesisHop + 1;
  for (int i = 0; i < span; ++i) {
    search_[static_cast<size_t>(i)] = reader(context, 0, desired - kSearchRadius + i);
  }
  const auto score = [this](int offset) noexcept {
    double numerator = 0.0;
    double energy = 0.0;
    for (int i = 0; i < kSynthesisHop; ++i) {
      const double s = search_[static_cast<size_t>(offset + i)];
      numerator += s * match_[static_cast<size_t>(i)];
      energy += s * s;
    }
    // Normalized by the candidate's energy only: the template is fixed across
    // candidates, so its norm cannot change the ranking, and dividing by it
    // would only add a square root per candidate.
    return numerator / std::sqrt(energy + 1e-12);
  };
  int best = kSearchRadius;
  double best_score = score(best);
  for (int offset = 0; offset <= 2 * kSearchRadius; offset += kCoarseStride) {
    const double value = score(offset);
    if (value > best_score) {
      best_score = value;
      best = offset;
    }
  }
  const int low = std::max(0, best - kCoarseStride + 1);
  const int high = std::min(2 * kSearchRadius, best + kCoarseStride - 1);
  for (int offset = low; offset <= high; ++offset) {
    const double value = score(offset);
    if (value > best_score) {
      best_score = value;
      best = offset;
    }
  }
  return best - kSearchRadius;
}

void WarpStretchVoice::synthesize_frame(int offset, WarpSourceReader reader,
                                        WarpPositionMapper mapper, void* context) noexcept {
  const int64_t output_position = next_output_ + offset;
  const double mapped = mapper(context, output_position);
  int64_t start = static_cast<int64_t>(std::llround(mapped));
  if (have_previous_) {
    start += best_offset(start, reader, context);
  }

  for (int ch = 0; ch < channels_; ++ch) {
    float* dst = overlap_[static_cast<size_t>(ch)].data() + offset;
    for (int i = 0; i < kFrameSize; ++i) {
      dst[i] += window_[static_cast<size_t>(i)] * reader(context, ch, start + i);
    }
  }
  // The next frame must continue this segment, so the template is this
  // segment's own continuation one synthesis hop later.
  for (int i = 0; i < kSynthesisHop; ++i) {
    match_[static_cast<size_t>(i)] = reader(context, 0, start + kSynthesisHop + i);
  }
  have_previous_ = true;
  filled_ = std::max(filled_, offset + kFrameSize);
}

bool WarpStretchVoice::render(uint32_t clip_id, int64_t output_start, int count, float** out,
                              int channels, WarpSourceReader reader, WarpPositionMapper mapper,
                              void* context) noexcept {
  if (capacity_ <= 0 || count <= 0 || !out || !reader || !mapper) return false;
  if (count + kSynthesisHop + kFrameSize > capacity_) return false;

  if (!active_ || clip_id_ != clip_id || next_output_ != output_start) {
    reset();
    active_ = true;
    clip_id_ = clip_id;
    next_output_ = output_start;
  }
  idle_blocks_ = 0;

  while (next_frame_offset_ < count) {
    synthesize_frame(next_frame_offset_, reader, mapper, context);
    next_frame_offset_ += kSynthesisHop;
  }

  const int last_source = channels_ - 1;
  for (int ch = 0; ch < channels; ++ch) {
    if (!out[ch]) continue;
    const float* src = overlap_[static_cast<size_t>(std::min(ch, last_source))].data();
    std::memcpy(out[ch], src, static_cast<size_t>(count) * sizeof(float));
  }

  // Drop the drained samples and slide the accumulator down. filled_ bounds the
  // move so a long look-ahead does not copy the untouched tail every block.
  const int remaining = std::max(0, filled_ - count);
  for (int ch = 0; ch < channels_; ++ch) {
    float* buffer = overlap_[static_cast<size_t>(ch)].data();
    if (remaining > 0) {
      std::memmove(buffer, buffer + count, static_cast<size_t>(remaining) * sizeof(float));
    }
    std::fill(buffer + remaining, buffer + capacity_, 0.0f);
  }
  filled_ = remaining;
  next_frame_offset_ = std::max(0, next_frame_offset_ - count);
  next_output_ += count;
  return true;
}

}  // namespace sonare::engine
