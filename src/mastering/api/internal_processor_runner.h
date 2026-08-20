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
#include <array>
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

namespace detail {

/// @brief Shared planar implementation behind run_processor_mono() and
///        run_processor_stereo().
///
/// Every chunk handed to @p processor is exactly `block_size` samples long —
/// the same size the processor was prepared at. That matters because some
/// processors only reach their fast path on a block whose length is a whole
/// multiple of an internal partition size: `LinearPhaseEq` hands an aligned
/// block to its partitioned FFT convolver and falls back to a direct
/// time-domain convolution otherwise, which for a long FIR kernel is orders of
/// magnitude slower. Feeding a ragged final chunk (`n mod block_size`) or a
/// short latency flush would drop those samples onto the slow path, so the
/// remainder and the flush are zero-padded up to a whole block instead.
///
/// The padding is exact rather than approximate: the stream simply ends, so
/// the samples past the input are genuinely zero, and every output sample
/// produced from them is discarded. Whole blocks that lie entirely inside the
/// input are still processed in place, so the scratch buffer stays bounded by
/// two blocks per channel regardless of track length.
template <std::size_t kChannels>
inline void run_processor_planar(rt::ProcessorBase& processor,
                                 const std::array<std::vector<float>*, kChannels>& buffers,
                                 int sample_rate) {
  constexpr int kNumChannels = static_cast<int>(kChannels);
  const int n = static_cast<int>(buffers[0]->size());
  const int probe_block_size = offline_processor_block_size(n);
  // Prepare once at the bounded block size to query latency (valid
  // post-prepare for our processors). The channel-aware overload lets offline
  // mono processing avoid reserving realtime-only per-channel scratch space.
  processor.prepare(sample_rate, probe_block_size, kNumChannels);
  const int latency = std::max(processor.latency_samples(), 0);
  const int block_size = offline_processor_block_size(n + latency);
  processor.prepare(sample_rate, block_size, kNumChannels);

  std::array<float*, kChannels> channels{};

  // Blocks that fit entirely inside the input run in place.
  const int in_place_samples = (n / block_size) * block_size;
  for (int offset = 0; offset < in_place_samples; offset += block_size) {
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
      channels[ch] = buffers[ch]->data() + offset;
    }
    processor.process(channels.data(), kNumChannels, block_size);
  }

  // The leftover input tail plus the latency flush, zero-padded to a whole
  // number of blocks.
  const int remainder = n - in_place_samples;
  const int flush_samples = remainder + latency;
  std::array<std::vector<float>, kChannels> scratch{};
  if (flush_samples > 0) {
    const int padded = ((flush_samples + block_size - 1) / block_size) * block_size;
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
      scratch[ch].assign(static_cast<std::size_t>(padded), 0.0f);
      std::copy(buffers[ch]->begin() + in_place_samples, buffers[ch]->end(), scratch[ch].begin());
    }
    for (int offset = 0; offset < padded; offset += block_size) {
      for (std::size_t ch = 0; ch < kChannels; ++ch) {
        channels[ch] = scratch[ch].data() + offset;
      }
      processor.process(channels.data(), kNumChannels, block_size);
    }
    // scratch[0 .. remainder) is the processed tail of the input; the padding
    // that follows it is discarded except for the `latency` flush samples.
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
      std::copy(scratch[ch].begin(), scratch[ch].begin() + remainder,
                buffers[ch]->begin() + in_place_samples);
    }
  }

  if (latency <= 0) {
    return;
  }
  // Drop the leading `latency` delayed samples and pull the flushed tail in
  // behind them so the result stays time-aligned with the input.
  for (std::size_t ch = 0; ch < kChannels; ++ch) {
    std::vector<float>& buffer = *buffers[ch];
    const float* tail = scratch[ch].data() + remainder;
    if (latency < n) {
      std::move(buffer.begin() + latency, buffer.end(), buffer.begin());
      std::copy(tail, tail + latency, buffer.end() - latency);
    } else {
      std::copy(tail + (latency - n), tail + latency, buffer.begin());
    }
  }
}

}  // namespace detail

/// @brief Run @p processor over a mono buffer in place, compensating for the
///        processor's reported latency so the output length matches the input
///        length and the time alignment is preserved.
///
/// Behaviour:
///  - empty input is a no-op (no `prepare()` call);
///  - otherwise the processor is prepared and run in fixed-size chunks to
///    bound its working memory independently of track duration;
///  - every chunk is exactly the prepared block size — the ragged input tail
///    and the latency flush are zero-padded up to a whole block — so that a
///    processor with a block-size-dependent fast path keeps it for the whole
///    render;
///  - when latency is positive, `latency` trailing zero samples are streamed
///    through the same chunks, then the leading delayed output is discarded so
///    the result remains time-aligned with the input.
inline void run_processor_mono(rt::ProcessorBase& processor, std::vector<float>& samples,
                               int sample_rate) {
  if (samples.empty()) {
    return;
  }
  const std::array<std::vector<float>*, 1> buffers{&samples};
  detail::run_processor_planar(processor, buffers, sample_rate);
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
  const std::array<std::vector<float>*, 2> buffers{&left, &right};
  detail::run_processor_planar(processor, buffers, sample_rate);
}

}  // namespace sonare::mastering::api::internal
