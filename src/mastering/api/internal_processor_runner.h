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

#include "mastering/common/processor_base.h"
#include "util/exception.h"

namespace sonare::mastering::api::internal {

/// @brief Run @p processor over a mono buffer in place, compensating for the
///        processor's reported latency so the output length matches the input
///        length and the time alignment is preserved.
///
/// Behaviour:
///  - empty input is a no-op (no `prepare()` call);
///  - otherwise the processor is prepared at @p samples.size() to discover its
///    latency; when latency is zero the buffer is processed in place;
///  - when latency is positive the processor is re-prepared at
///    `samples.size() + latency`, the buffer is padded with `latency` trailing
///    zeros, processed, then the leading `latency` output samples are
///    discarded so the delayed tail is flushed out and the result is time-
///    aligned with the input.
inline void run_processor_mono(common::ProcessorBase& processor, std::vector<float>& samples,
                               int sample_rate) {
  if (samples.empty()) {
    return;
  }
  const int n = static_cast<int>(samples.size());
  // Prepare once at N to query latency (valid post-prepare for our processors).
  processor.prepare(sample_rate, n);
  const int latency = processor.latency_samples();
  if (latency <= 0) {
    float* channels[] = {samples.data()};
    processor.process(channels, 1, n);
    return;
  }
  // Re-prepare for the padded length (prepare() reinitializes processor state,
  // so a separate reset() before it would be redundant), then process
  // N signal + `latency` zeros and drop the leading `latency` output samples so
  // the result is time-aligned and the delayed tail is flushed out.
  processor.prepare(sample_rate, n + latency);
  std::vector<float> padded(samples.begin(), samples.end());
  padded.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  float* channels[] = {padded.data()};
  processor.process(channels, 1, n + latency);
  std::copy(padded.begin() + latency, padded.begin() + latency + n, samples.begin());
}

/// @brief Stereo counterpart to run_processor_mono(). @p left and @p right must
///        have identical length. Both channels are padded, processed, and
///        trimmed together so they stay sample-accurately aligned.
inline void run_processor_stereo(common::ProcessorBase& processor, std::vector<float>& left,
                                 std::vector<float>& right, int sample_rate) {
  if (left.empty()) {
    return;
  }
  if (left.size() != right.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channel lengths must match");
  }
  const int n = static_cast<int>(left.size());
  processor.prepare(sample_rate, n);
  const int latency = processor.latency_samples();
  if (latency <= 0) {
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, n);
    return;
  }
  processor.prepare(sample_rate, n + latency);
  std::vector<float> padded_left(left.begin(), left.end());
  std::vector<float> padded_right(right.begin(), right.end());
  padded_left.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  padded_right.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  float* channels[] = {padded_left.data(), padded_right.data()};
  processor.process(channels, 2, n + latency);
  std::copy(padded_left.begin() + latency, padded_left.begin() + latency + n, left.begin());
  std::copy(padded_right.begin() + latency, padded_right.begin() + latency + n, right.begin());
}

}  // namespace sonare::mastering::api::internal
