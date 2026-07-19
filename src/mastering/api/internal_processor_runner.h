#pragma once

/// @file internal_processor_runner.h
/// @brief Shared per-processor execution helpers used by the offline mastering
///        API surfaces (`MasteringChain`, `apply_named_processor`, and the
///        WASM bindings). The helpers transparently compensate for each
///        processor's reported latency so callers receive a time-aligned
///        output of exactly the requested length.
///
/// All three offline surfaces previously kept private copies of nearly
/// identical logic; centralising it here keeps the latency-compensation policy
/// in one place. The streaming variant (`StreamingMasteringChain`) intentionally
/// does **not** use these helpers because it operates block-by-block on a
/// continuous stream where any reported latency is the caller's responsibility
/// to account for.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "rt/processor_base.h"
#include "util/exception.h"

namespace sonare::mastering::api::internal {

/// Upper bound for offline processor work buffers. Keeping this independent of
/// track length prevents processors such as TruePeakLimiter from allocating an
/// oversampled scratch buffer for an entire song.
inline constexpr int kOfflineProcessorBlockSize = 64 * 1024;

inline int offline_processor_block_size(int total_samples) {
  return std::min(total_samples, kOfflineProcessorBlockSize);
}

/// @brief Run @p processor over a mono buffer in place, compensating for the
///        processor's reported latency so the output length matches the input
///        length and the time alignment is preserved.
///
/// Behaviour:
///  - empty input is a no-op (no `prepare()` call);
///  - otherwise the processor is prepared and run in fixed-size chunks to
///    bound its working memory independently of track duration;
///  - when latency is positive, `latency` trailing zero samples are streamed
///    through the same chunks, then the leading delayed output is discarded so
///    the result remains time-aligned with the input.
inline void run_processor_mono(rt::ProcessorBase& processor, std::vector<float>& samples,
                               int sample_rate) {
  if (samples.empty()) {
    return;
  }
  const int n = static_cast<int>(samples.size());
  const int probe_block_size = offline_processor_block_size(n);
  // Prepare once at the bounded block size to query latency (valid
  // post-prepare for our processors). The channel-aware overload lets offline
  // mono processing avoid reserving realtime-only per-channel scratch space.
  processor.prepare(sample_rate, probe_block_size, 1);
  const int latency = processor.latency_samples();
  const int block_size = offline_processor_block_size(n + std::max(latency, 0));
  processor.prepare(sample_rate, block_size, 1);
  for (int offset = 0; offset < n; offset += block_size) {
    const int count = std::min(block_size, n - offset);
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
  if (latency <= 0) return;

  std::vector<float> tail(static_cast<size_t>(latency));
  for (int offset = 0; offset < latency; offset += block_size) {
    const int count = std::min(block_size, latency - offset);
    float* channels[] = {tail.data() + offset};
    processor.process(channels, 1, count);
  }
  if (latency < n) {
    std::move(samples.begin() + latency, samples.end(), samples.begin());
    std::copy(tail.begin(), tail.end(), samples.end() - latency);
  } else {
    std::copy(tail.begin() + (latency - n), tail.end(), samples.begin());
  }
}

/// @brief Stereo counterpart to run_processor_mono(). @p left and @p right must
///        have identical length. Both channels are padded, processed, and
///        trimmed together so they stay sample-accurately aligned.
inline void run_processor_stereo(rt::ProcessorBase& processor, std::vector<float>& left,
                                 std::vector<float>& right, int sample_rate) {
  if (left.empty()) {
    return;
  }
  if (left.size() != right.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channel lengths must match");
  }
  const int n = static_cast<int>(left.size());
  const int probe_block_size = offline_processor_block_size(n);
  processor.prepare(sample_rate, probe_block_size, 2);
  const int latency = processor.latency_samples();
  const int block_size = offline_processor_block_size(n + std::max(latency, 0));
  processor.prepare(sample_rate, block_size, 2);
  for (int offset = 0; offset < n; offset += block_size) {
    const int count = std::min(block_size, n - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
  }
  if (latency <= 0) return;

  std::vector<float> left_tail(static_cast<size_t>(latency));
  std::vector<float> right_tail(static_cast<size_t>(latency));
  for (int offset = 0; offset < latency; offset += block_size) {
    const int count = std::min(block_size, latency - offset);
    float* channels[] = {left_tail.data() + offset, right_tail.data() + offset};
    processor.process(channels, 2, count);
  }
  if (latency < n) {
    std::move(left.begin() + latency, left.end(), left.begin());
    std::move(right.begin() + latency, right.end(), right.begin());
    std::copy(left_tail.begin(), left_tail.end(), left.end() - latency);
    std::copy(right_tail.begin(), right_tail.end(), right.end() - latency);
  } else {
    std::copy(left_tail.begin() + (latency - n), left_tail.end(), left.begin());
    std::copy(right_tail.begin() + (latency - n), right_tail.end(), right.begin());
  }
}

}  // namespace sonare::mastering::api::internal
