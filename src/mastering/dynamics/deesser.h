#pragma once

/// @file deesser.h
/// @brief Split-band de-esser for attenuating sibilant high-frequency energy.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct DeEsserConfig {
  float frequency_hz = 6000.0f;
  float threshold_db = -24.0f;
  float ratio = 4.0f;
  float attack_ms = 1.0f;
  float release_ms = 60.0f;
  float range_db = 12.0f;
  float bandpass_q = 1.5f;
};

class DeEsser : public rt::ProcessorBase {
 public:
  explicit DeEsser(DeEsserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (bandpass biquads / envelope follower) are
  ///          recomputed on the audio thread when the snapshot is adopted, so
  ///          no per-channel state member is ever written concurrently with
  ///          sample processing. May allocate (the snapshot @c shared_ptr) and
  ///          is therefore NOT realtime-safe itself; call from the configuration
  ///          thread only. Two threads MUST NOT call @ref set_config
  ///          concurrently with each other (single-producer hand-off). Throws
  ///          @c std::invalid_argument with the same rules as the constructor;
  ///          on throw the published configuration is unchanged (validation
  ///          happens before publish, never partway).
  void set_config(const DeEsserConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const DeEsserConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = frequency_hz (clamped to > 0)
  //   1 = threshold_db
  //   2 = ratio (clamped to >= 1)
  //   3 = attack_ms (clamped to >= 0)
  //   4 = release_ms (clamped to >= 0)
  //   5 = range_db (clamped to >= 0)
  //   6 = bandpass_q (clamped to > 0)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=frequencyHz, 1=thresholdDb, 2=ratio, 3=attackMs, 4=releaseMs,
  // 5=rangeDb, 6=bandpassQ
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const DeEsserConfig& config);
  static float gain_reduction_db(float input_db, const DeEsserConfig& config);
  void ensure_state(int num_channels);
  /// @brief Recomputes scalar derived coefficients (bandpass biquads,
  ///        envelope follower attack/release) from @p config. RT-safe: scalar
  ///        math only, no allocation; the biquad rewrites preserve z1/z2 state
  ///        so the audio thread can call this between blocks.
  void update_coefficients(const DeEsserConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const DeEsserConfig* adopt_snapshot_for_block() noexcept;

  using Biquad = rt::BiquadState;

  DeEsserConfig config_{};
  /// @brief Audio-thread working configuration. The per-sample loop reads this
  ///        (not the published snapshot directly) so RT-safe set_parameter can
  ///        mutate it in place and re-derive coefficients without allocating a
  ///        new snapshot. adopt_snapshot_for_block copies a freshly published
  ///        snapshot into it at block start; set_parameter mutates it directly.
  DeEsserConfig active_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_->current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block. Held by @c unique_ptr so
  ///        DeEsser itself remains move-constructible (RtPublisher deletes
  ///        its copy and move operations to keep the SPSC ring indices
  ///        position-stable); the indirection is touched once per block from
  ///        the audio thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<DeEsserConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const DeEsserConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  Biquad filter_coeffs_;
  std::vector<Biquad> bandpass_;
  std::vector<Biquad> bandpass2_;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
