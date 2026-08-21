#pragma once

/// @file alignment_delay.h
/// @brief Integer and fractional-sample channel alignment delay.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rt/delay_line.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

// Bounds per-strip delay storage even for malformed scenes. At 48 kHz this is
// four seconds, while retaining headroom for practical manual alignment.
//
// Two different jobs share this bound, and they are handled differently on
// purpose. A delay a CALLER asked for is rejected at the public boundary
// (TrackMixerRuntime::set_track_channel_delay_samples and the C ABI entry
// points) so that no surface can silently substitute this ceiling for the value
// requested -- every surface either applies exactly the requested delay or
// reports InvalidParameter. A delay DERIVED internally from a processor's own
// reported latency is clamped instead, because an insert advertising an absurd
// latency must not be able to overflow the Q8 shift or exhaust memory, and
// there is no caller to report it to.
inline constexpr int kMaxAlignmentDelaySamples = 192000;

enum class FractionalDelayMode {
  None,
  // Default fractional-delay mode for alignment/PDC. The implementation is a
  // 3rd-order Lagrange FIR: stable and predictable for delay changes, with a
  // deliberate high-frequency magnitude droop for fractional delays.
  Lagrange3,
};

/// @brief Owns one bank of per-channel alignment/PDC delay.
/// @details This type -- not its call sites -- owns three properties that the
///          mixing runtime depends on and that a local edit at a call site must
///          not be able to take away:
///
///          1. **Storage is (re)allocated only when the storage requirement
///             actually changes.** Every mutator funnels through
///             prepare_storage(), which compares the required shape against the
///             shape currently held and returns without touching the delay
///             lines when they match. Re-applying the delay a bank already
///             carries therefore preserves its history instead of dropping the
///             audio in flight. storage_generation() makes that observable, so
///             the property is checked by a test rather than by review.
///          2. **A failed reallocation is reportable, not fatal.** The throwing
///             mutators keep the strong guarantee (the replacement is built
///             before anything is swapped in), and try_set_delay_samples_q8()
///             exposes the same operation to `noexcept` control-thread setters.
///          3. **A call wider than the prepared bank is recorded.** process()
///             still clamps -- the audio thread never allocates -- but the
///             overflow is counted so a host that under-prepared a wide layout
///             can be caught instead of silently leaving the upper planes
///             un-delayed.
class AlignmentDelay : public rt::ProcessorBase {
 public:
  explicit AlignmentDelay(int delay_samples = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  /// @brief Advances the delay state over @p num_samples without writing back.
  /// @details A compensation delay that only carries the signal some of the time
  ///          (a bypass substitute, a surround-plane aligner behind a bypassable
  ///          insert) would otherwise sit cold while unused and open with a
  ///          delay-line's worth of silence the moment it is switched in --
  ///          audible as a dropout on a bypass toggle. Feeding it the same
  ///          stream it would have processed keeps its state continuous, so the
  ///          switch costs only the intended change in processing, not a gap.
  ///          Read-only on @p channels, allocation-free, and clamped to the
  ///          prepared channel count exactly like @c process().
  void prime(const float* const* channels, int num_channels, int num_samples) noexcept;
  void reset() override;
  int latency_samples() const noexcept override { return delay_samples_; }
  // Reports the exact requested Q8 delay. latency_samples() intentionally
  // returns the integer floor for legacy callers; graph/mixing PDC should use
  // latency_samples_q8() to preserve the fractional part.
  int latency_samples_q8() const noexcept override { return delay_samples_q8_; }

  // Number of channels the delay should preallocate storage for. Must be set
  // (control thread) before prepare() so process() can run allocation-free for
  // the full channel count the host will pass. Defaults to a stereo pair.
  // Widening an already-prepared bank re-runs prepare_storage() here rather
  // than waiting for the next delay change: with the no-op guard in place, a
  // later same-value set_delay_samples_q8() would not reallocate, and the bank
  // would stay short of the width it was just told about.
  void set_prepared_channels(int num_channels);
  int prepared_channels() const noexcept { return prepared_channels_; }

  // Set the alignment delay. NOTE: these reallocate the per-channel delay-line
  // and fractional scratch storage (via prepare_storage) whenever the required
  // storage shape changes, so they are NOT audio-thread safe — call them from
  // the control thread while process() is not running concurrently (the same
  // contract as prepare()). Changing the delay during live playback requires
  // quiescing the processing thread first. Re-applying the value the bank
  // already carries allocates nothing and preserves the delay history.
  void set_delay_samples(int delay_samples);
  void set_delay_samples_q8(int delay_samples_q8,
                            FractionalDelayMode mode = FractionalDelayMode::Lagrange3);
  /// @brief set_delay_samples_q8() for a `noexcept` caller.
  /// @details Returns false when the storage could not be grown, leaving the
  ///          bank exactly as it was (delay, mode and history all intact), so a
  ///          `noexcept bool` control-thread setter can report the failure
  ///          instead of letting a bad_alloc escape and terminate the process.
  bool try_set_delay_samples_q8(int delay_samples_q8,
                                FractionalDelayMode mode = FractionalDelayMode::Lagrange3) noexcept;
  int delay_samples() const noexcept { return delay_samples_; }
  int delay_samples_q8() const noexcept { return delay_samples_q8_; }
  FractionalDelayMode fractional_mode() const noexcept { return fractional_mode_; }

  /// @brief Counts how many times this bank actually (re)allocated its storage.
  /// @details Incremented by prepare_storage() only when it replaces the delay
  ///          lines, so an unchanged generation across a control-thread edit is
  ///          proof that the delay history in flight survived it.
  uint64_t storage_generation() const noexcept { return storage_generation_; }
  /// @brief Widest excess channel count process()/prime() was ever handed.
  /// @details Zero means every call fitted the prepared bank. A non-zero value
  ///          means a caller asked for planes this bank cannot delay, so those
  ///          planes passed through un-delayed and are misaligned against the
  ///          ones below them -- the host under-prepared the bank.
  int channel_overflow_high_water() const noexcept { return channel_overflow_high_water_.load(); }

 private:
  struct FractionalState {
    std::vector<float> buffer{0.0f};
    size_t write_index = 0;
  };

  // Relaxed high-water counter written on the audio thread and read on the
  // control thread. Hand-written copy semantics only so the enclosing bank stays
  // copyable/movable: ChannelStrip keeps its per-insert banks in a std::vector
  // that resizes, and a bare std::atomic member would make that vector
  // uninstantiable. The resize happens on the control thread, under the same
  // not-concurrent-with-process() contract as prepare().
  struct HighWaterCounter {
    std::atomic<int> value{0};

    HighWaterCounter() = default;
    HighWaterCounter(const HighWaterCounter& other) noexcept
        : value(other.value.load(std::memory_order_relaxed)) {}
    HighWaterCounter& operator=(const HighWaterCounter& other) noexcept {
      if (this != &other) {
        value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
      }
      return *this;
    }

    int load() const noexcept { return value.load(std::memory_order_relaxed); }
    void raise_to(int candidate) noexcept {
      int seen = value.load(std::memory_order_relaxed);
      while (candidate > seen &&
             !value.compare_exchange_weak(seen, candidate, std::memory_order_relaxed)) {
      }
    }
  };

  // Exactly what prepare_storage() has to allocate for the current
  // configuration. Comparing it against the shape already held is what makes a
  // same-value delay change a no-op for every caller, present and future.
  struct StorageSpec {
    int channels = 0;
    size_t integer_delay = 0;
    // 0 when the fractional path is unused; the fractional read position is
    // derived from delay_samples_q8_ at process time, so two delays sharing an
    // integer part share their storage and their history.
    size_t fractional_size = 0;

    bool operator==(const StorageSpec& other) const noexcept {
      return channels == other.channels && integer_delay == other.integer_delay &&
             fractional_size == other.fractional_size;
    }
  };

  StorageSpec required_storage() const noexcept;
  void prepare_storage();
  void note_channel_overflow(int num_channels) noexcept;
  float process_fractional(FractionalState& state, float input) const noexcept;

  int delay_samples_ = 0;
  int delay_samples_q8_ = 0;
  int prepared_channels_ = 0;
  FractionalDelayMode fractional_mode_ = FractionalDelayMode::None;
  StorageSpec storage_{};
  uint64_t storage_generation_ = 0;
  // Written on the audio thread, read on the control thread; relaxed because it
  // is a diagnostic high-water mark with no ordering relationship to the audio.
  HighWaterCounter channel_overflow_high_water_{};
  std::vector<rt::DelayLine> delays_;
  std::vector<FractionalState> fractional_;
};

}  // namespace sonare::mixing
