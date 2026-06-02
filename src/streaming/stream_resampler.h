#pragma once

/// @file stream_resampler.h
/// @brief Stateful, phase-continuous resampler for the streaming analyzer.
///
/// Unlike the one-shot @c sonare::resample() (which constructs a fresh filter,
/// flushes its tail with zeros, and trims to an analytic length on every call),
/// this wrapper keeps a single persistent r8brain resampler alive across all
/// chunks. The internal poly-phase filter state therefore carries over from one
/// process() call to the next, so consecutive chunks join seamlessly with no
/// per-chunk boundary discontinuity ("click") and no accumulated phase drift.
///
/// The trade-off is a small, constant filter latency at the very start of the
/// stream (a handful of milliseconds), which is acceptable for a real-time
/// visualization path and is identical to what any production streaming
/// resampler exhibits. This file is intentionally scoped to streaming only and
/// does not touch core/resample.cpp, whose one-shot API other callers rely on.

#include <memory>
#include <vector>

namespace sonare::streaming_detail {

/// @brief Continuous (stateful) audio resampler for the streaming path.
/// @details Wraps a single long-lived r8brain CDSPResampler so that filter
///          state is preserved between process() calls. Output samples are
///          appended as they emerge; the constant start-up filter latency means
///          the first call may return fewer samples than the steady-state ratio
///          would suggest, but no samples are ever fabricated with zero-padding
///          and no discontinuity is introduced at chunk boundaries.
class StreamResampler {
 public:
  /// @brief Constructs a continuous resampler.
  /// @param src_sr Source sample rate in Hz (must be > 0).
  /// @param dst_sr Destination sample rate in Hz (must be > 0).
  StreamResampler(int src_sr, int dst_sr);

  ~StreamResampler();

  // Non-copyable (owns a heap resampler with internal state), movable.
  StreamResampler(const StreamResampler&) = delete;
  StreamResampler& operator=(const StreamResampler&) = delete;
  StreamResampler(StreamResampler&&) noexcept;
  StreamResampler& operator=(StreamResampler&&) noexcept;

  /// @brief Resamples one chunk, preserving filter state across calls.
  /// @param samples Input samples at the source rate (may be nullptr iff n==0).
  /// @param n_samples Number of input samples.
  /// @param out Output vector; resampled samples are appended (not cleared).
  /// @details Phase is continuous with the previous call. Bad inputs (NaN/Inf)
  ///          are NOT sanitized here; the caller sanitizes before/after as
  ///          appropriate for its pipeline.
  void process(const float* samples, size_t n_samples, std::vector<float>& out);

  /// @brief Resets filter state, e.g. when the analyzer is reset for a new
  ///        stream. After this the next process() call restarts from the
  ///        cleared (post-construction) filter state.
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::streaming_detail
