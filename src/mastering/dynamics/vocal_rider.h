#pragma once

/// @file vocal_rider.h
/// @brief Automatic level rider that moves signal toward a target loudness.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct VocalRiderConfig {
  float target_db = -18.0f;
  float max_boost_db = 6.0f;
  float max_cut_db = 6.0f;
  float attack_ms = 50.0f;
  float release_ms = 500.0f;
  float output_gain_db = 0.0f;
  float gain_smoothing_ms = 100.0f;
  float noise_floor_db = -60.0f;
  bool linked_detection = true;
};

class VocalRider : public rt::ProcessorBase {
 public:
  explicit VocalRider(VocalRiderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (envelope follower attack/release) are
  ///          recomputed on the audio thread when the snapshot is adopted, so
  ///          no per-channel state member is ever written concurrently with
  ///          sample processing. May allocate (the snapshot @c shared_ptr) and
  ///          is therefore NOT realtime-safe itself; call from the configuration
  ///          thread only. Two threads MUST NOT call @ref set_config
  ///          concurrently with each other (single-producer hand-off). Throws
  ///          @c std::invalid_argument with the same rules as the constructor;
  ///          on throw the published configuration is unchanged (validation
  ///          happens before publish, never partway).
  void set_config(const VocalRiderConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const VocalRiderConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = target_db
  //   1 = max_boost_db (clamped to >= 0)
  //   2 = max_cut_db (clamped to >= 0)
  //   3 = attack_ms (clamped to >= 0)
  //   4 = release_ms (clamped to >= 0)
  //   5 = output_gain_db
  //   6 = gain_smoothing_ms (clamped to >= 0)
  //   7 = noise_floor_db
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=targetDb, 1=maxBoostDb, 2=maxCutDb, 3=attackMs,
  //   4=releaseMs, 5=outputGainDb, 6=gainSmoothingMs, 7=noiseFloorDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const VocalRiderConfig& config);
  void ensure_followers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (envelope follower
  ///        attack/release) from @p config. RT-safe: scalar math only, no
  ///        allocation; the follower rewrites preserve envelope state.
  void update_coefficients(const VocalRiderConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const VocalRiderConfig* adopt_snapshot_for_block() noexcept;

  VocalRiderConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_->current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block. Held by @c unique_ptr so
  ///        VocalRider itself remains move-constructible (RtPublisher deletes
  ///        its copy and move operations to keep the SPSC ring indices
  ///        position-stable); the indirection is touched once per block from
  ///        the audio thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<VocalRiderConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const VocalRiderConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<rt::EnvelopeFollower> followers_;
  float linked_gain_state_db_ = 0.0f;
  // Per-channel smoothed gain state for the unlinked path, persisted across
  // blocks so toggling linked/unlinked does not introduce a discontinuity.
  std::vector<float> unlinked_gain_state_db_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
