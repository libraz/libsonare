#pragma once

/// @file transient_shaper.h
/// @brief Envelope-difference transient shaper for attack and sustain control.

#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct TransientShaperConfig {
  float attack_gain_db = 3.0f;
  float sustain_gain_db = 0.0f;
  float fast_attack_ms = 0.0f;
  float fast_release_ms = 20.0f;
  float slow_attack_ms = 15.0f;
  float slow_release_ms = 200.0f;
  float sensitivity = 1.0f;
  float max_gain_db = 12.0f;
  float gain_smoothing_ms = 0.0f;
  float lookahead_ms = 0.0f;
};

class TransientShaper : public rt::ProcessorBase {
 public:
  explicit TransientShaper(TransientShaperConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (envelope followers and gain smoother) are
  ///          recomputed on the audio thread when the snapshot is adopted, so no
  ///          per-channel state member is ever written concurrently with sample
  ///          processing. May allocate (the snapshot @c shared_ptr) and is
  ///          therefore NOT realtime-safe itself; call from the configuration
  ///          thread only. Two threads MUST NOT call @ref set_config
  ///          concurrently with each other (single-producer hand-off). Throws
  ///          @c std::invalid_argument with the same rules as the constructor;
  ///          on throw the published configuration is unchanged (validation
  ///          happens before publish, never partway).
  ///          The @c lookahead_ms field MUST equal the value that was last
  ///          passed to @ref prepare — the lookahead delay lines are sized in
  ///          @ref prepare and not reallocated here.
  void set_config(const TransientShaperConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const TransientShaperConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = attack_gain_db
  //   1 = sustain_gain_db
  //   2 = fast_attack_ms (clamped to >= 0)
  //   3 = fast_release_ms (clamped to >= 0)
  //   4 = slow_attack_ms (clamped to >= 0)
  //   5 = slow_release_ms (clamped to >= 0)
  //   6 = sensitivity (clamped to >= 0)
  //   7 = max_gain_db (clamped to >= 0)
  //   8 = gain_smoothing_ms (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const TransientShaperConfig& config);
  void ensure_followers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (gain smoother and per-
  ///        channel envelope follower attack/release) from @p config. RT-safe:
  ///        scalar math only, no allocation. Does NOT resize lookahead buffers
  ///        — those stay sized to whatever @ref prepare was called with.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const TransientShaperConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const TransientShaperConfig* adopt_snapshot_for_block() noexcept;

  TransientShaperConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_->current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block. Held by @c unique_ptr so
  ///        TransientShaper itself remains move-constructible (RtPublisher
  ///        deletes its copy and move operations to keep the SPSC ring indices
  ///        position-stable); the indirection is touched once per block from
  ///        the audio thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<TransientShaperConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const TransientShaperConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  // Gain-smoother coefficient, cached because it depends only on sample rate and
  // gain_smoothing_ms; recomputed by update_coefficients() from prepare() and
  // adopt_snapshot_for_block().
  float gain_smoothing_coeff_ = 0.0f;
  std::vector<rt::EnvelopeFollower> fast_followers_;
  std::vector<rt::EnvelopeFollower> slow_followers_;
  std::vector<float> gain_state_db_;
  std::vector<std::vector<float>> lookahead_;
  std::vector<size_t> lookahead_index_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
