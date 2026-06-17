#pragma once

/// @file limiter.h
/// @brief Lookahead peak limiter.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/lookahead_buffer.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct LimiterConfig {
  float threshold_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
};

class Limiter : public rt::ProcessorBase {
 public:
  explicit Limiter(LimiterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (release smoothing) are recomputed on the
  ///          audio thread when the snapshot is adopted, so no per-channel
  ///          state member is ever written concurrently with sample
  ///          processing. @b Note: @c lookahead_ms still drives the lookahead
  ///          buffer size, which is fixed at @ref prepare and is NOT updated
  ///          from a published snapshot — call prepare() again to change it.
  ///          May allocate (the snapshot @c shared_ptr) and is therefore NOT
  ///          realtime-safe itself; call from the configuration thread only.
  ///          Two threads MUST NOT call @ref set_config concurrently with each
  ///          other (single-producer hand-off). Throws @c std::invalid_argument
  ///          with the same rules as the constructor; on throw the published
  ///          configuration is unchanged (validation happens before publish,
  ///          never partway).
  void set_config(const LimiterConfig& config);
  void set_release_ms(float release_ms);
  /// @brief Realtime-safe release update for per-block automation.
  /// @details Recomputes @c release_coeff_ in place from @p release_ms without
  ///          publishing a new configuration snapshot (so no @c shared_ptr
  ///          allocation occurs). Safe to call from the audio thread once per
  ///          block. The control-thread @c config_ mirror is NOT updated and
  ///          the published snapshot is untouched, so a later snapshot adoption
  ///          will overwrite the in-place coefficient — call this only on the
  ///          audio thread driving the per-block release. Uses the same
  ///          release-coefficient math as @ref set_release_ms.
  void set_release_ms_in_place(float release_ms) noexcept;
  /// @brief Realtime-safe ceiling (threshold) update for per-block automation.
  /// @details Updates the scalar threshold the per-sample loop reads in place,
  ///          without publishing a new configuration snapshot (so no
  ///          @c shared_ptr allocation occurs). Safe to call from the audio
  ///          thread once per block. The control-thread @c config_ mirror is NOT
  ///          updated and the published snapshot is untouched, so a later
  ///          snapshot adoption will overwrite the in-place threshold.
  void set_threshold_in_place(float threshold_db) noexcept;
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const LimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }
  int latency_samples() const noexcept override { return lookahead_samples_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = release_ms (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const LimiterConfig& config);
  /// @brief Verifies the prepared lookahead state can cover @p num_channels.
  /// @details RT-safe: never resizes on the audio thread. The lookahead
  ///          buffers are preallocated to @c kRealtimePreparedChannels in
  ///          prepare(); a block requesting more channels throws instead of
  ///          allocating (which would malloc on the audio thread).
  void prepare_buffers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (release smoothing) from
  ///        @p config. RT-safe: scalar math only, no allocation.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks. Does NOT touch the lookahead buffers,
  ///          which are sized at @ref prepare and immutable across snapshot
  ///          adoptions.
  void update_coefficients(const LimiterConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const LimiterConfig* adopt_snapshot_for_block() noexcept;

  LimiterConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. Held by @c unique_ptr so Limiter
  ///        itself remains move-constructible (RtPublisher deletes its copy
  ///        and move operations to keep the SPSC ring indices position-
  ///        stable); the indirection is touched once per block from the audio
  ///        thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<LimiterConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const LimiterConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  std::vector<sonare::rt::LookaheadBuffer> lookahead_;
  // Per-channel delayed-sample scratch, allocated in prepare() so the per-block
  // process() path never reallocates. Sized to kRealtimePreparedChannels.
  std::vector<float> delayed_;
  // A single, shared gain smoother fed by the linked (max-peak) target so every
  // channel is scaled by the same gain. Per-channel smoothers would let the L/R
  // gain diverge on asymmetric content and shift the stereo image.
  sonare::rt::EnvelopeFollower gain_smoother_;
  float release_coeff_ = 0.0f;
  // Derived scalar threshold the per-sample loop reads. Set by
  // update_coefficients() on snapshot adoption and by set_threshold_in_place()
  // for RT-safe per-block automation, so a ceiling change needs no publish.
  float threshold_db_ = -1.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
