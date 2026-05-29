#pragma once

/// @file isp_limiter.h
/// @brief Per-channel inter-sample-peak (true-peak / ISP) limiter for the
///        realtime voice changer output stage.
///
/// Reuses the rt::TruePeakFilter (4x BS.1770-style upsampler) for inter-sample
/// peak detection and a small lookahead buffer + sliding-max window to align
/// the gain envelope with the delayed dry signal. Gain is applied at the base
/// sample rate (detect-only / no downsampler) so the per-block CPU cost stays
/// well below the existing mastering::maximizer::TruePeakLimiter, which runs a
/// full polyphase upsample → limit → downsample pipeline.
///
/// Rationale for not reusing mastering::maximizer::TruePeakLimiter:
///   - It is a multichannel processor (always allocates 2-channel arrays) and
///     its set_config() re-prepares the inner state — not safe to drive from
///     the RT-safe per-block config-snapshot path used by RealtimeVoiceChanger.
///   - Pulling sonare_mastering as a link dependency of sonare_voice_changer
///     would drag in the full mastering chain (compressor / EQ / saturation /
///     multiband). The voice changer already implements its own gate /
///     compressor / EQ from primitives in rt/; mirroring that pattern keeps
///     binary size minimal.
///   - The voice changer needs a per-channel object so each instance carries
///     its own delay line and detector state; the existing TruePeakLimiter
///     mixes channels through a linked detector which is inappropriate when
///     each channel is processed independently.

#include <cstddef>
#include <vector>

#include "rt/lookahead_buffer.h"
#include "rt/sliding_max.h"
#include "rt/true_peak_filter.h"

namespace sonare::editing::voice_changer {

/// @brief Configuration for the ISP (inter-sample peak) limiter stage.
struct IspLimiterConfig {
  /// True-peak ceiling in dBTP. Defaults to -1.0 dBTP (EBU R128 / AES streaming
  /// recommendation). The stage is detect-only at oversampled rate and applies
  /// gain at the base rate, so the realized true-peak ceiling holds up to a
  /// small numerical tolerance (~0.1 dB) under steady-state material.
  float ceiling_dbtp = -1.0f;
  /// Release time constant in ms for the gain ramp-up after a transient.
  float release_ms = 50.0f;
};

/// @brief Mono ISP limiter with 4x oversampled inter-sample peak detection.
///
/// Lifecycle:
///   - construct → prepare(sample_rate, max_block_size) → process_block(...)
///   - set_config / reset are realtime-safe; prepare() is the only allocation
///     point.
///
/// Latency: equal to @ref latency_samples (returns 0 before prepare()).
/// The latency comes from the upsampler FIR group delay (taps_per_phase / 2 at
/// base rate, == 6 samples for the 4x BS.1770-style design) and is the same
/// lookahead used to align the per-sample gain with the delayed signal.
class IspLimiter {
 public:
  IspLimiter();

  /// @brief Non-RT: allocates internal scratch / history / lookahead buffers.
  void prepare(double sample_rate, int max_block_size);
  /// @brief RT-safe: clears all per-sample state, keeps allocations.
  void reset() noexcept;
  /// @brief RT-safe coefficient update (no allocation, no re-prepare).
  void set_config(const IspLimiterConfig& config) noexcept;
  /// @brief Process a mono block in place. RT-safe and noexcept; pre-condition
  ///        violations (no prepare, num_samples > max_block_size, null buffer)
  ///        cause a silent no-op rather than throwing.
  void process_block(float* buffer, int num_samples) noexcept;
  /// @brief Reports the limiter's signal-path latency in samples.
  int latency_samples() const noexcept;

 private:
  void update_time_constants();

  rt::TruePeakFilter filter_;
  rt::LookaheadBuffer lookahead_;
  rt::SlidingMax<float> oversampled_peak_window_{1};
  std::vector<float> oversampled_;
  std::vector<float> history_;
  std::vector<float> scratch_;
  // Pointer-of-pointer scratch buffers reused per block so process_block does
  // not have to take the address of stack-local variables (which would let the
  // pointer participate in escape analysis and inhibit inlining).
  std::vector<const float*> input_ptr_;
  std::vector<float*> output_ptr_;
  std::vector<std::vector<float>> history_holder_;
  std::vector<std::vector<float>> scratch_holder_;

  IspLimiterConfig config_{};
  double sample_rate_ = 0.0;
  int max_block_size_ = 0;
  int oversample_factor_ = 4;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  float attack_alpha_ = 1.0f;
  float release_alpha_ = 1.0f;
  float gain_ = 1.0f;
};

}  // namespace sonare::editing::voice_changer
