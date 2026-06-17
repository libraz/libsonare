#pragma once

/// @file sidechain_router.h
/// @brief Sidechain ducking processor with optional external detector input.

#include <memory>
#include <vector>

#include "rt/envelope_follower.h"
#include "rt/lookahead_buffer.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct SidechainRouterConfig {
  float threshold_db = -24.0f;
  float ratio = 4.0f;
  float attack_ms = 5.0f;
  float release_ms = 100.0f;
  float range_db = 18.0f;
  bool sidechain_hpf_enabled = false;
  float sidechain_hpf_hz = 90.0f;
  bool mono_summing = false;
  bool key_listen = false;
  float lookahead_ms = 0.0f;
};

class SidechainRouter : public rt::ProcessorBase {
 public:
  explicit SidechainRouter(SidechainRouterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override { return lookahead_samples_; }

  // Borrows channel pointers until the next set_sidechain(), clear_sidechain(),
  // or process() call that consumes them. The caller owns the buffers and must
  // keep them alive and unchanged for that interval.
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override;
  void clear_sidechain() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (envelope follower attack/release and the
  ///          sidechain HPF) are recomputed on the audio thread when the
  ///          snapshot is adopted, so no per-channel state member is ever
  ///          written concurrently with sample processing. May allocate (the
  ///          snapshot @c shared_ptr) and is therefore NOT realtime-safe
  ///          itself; call from the configuration thread only. Two threads
  ///          MUST NOT call @ref set_config concurrently with each other
  ///          (single-producer hand-off). Throws @c std::invalid_argument with
  ///          the same rules as the constructor; on throw the published
  ///          configuration is unchanged (validation happens before publish,
  ///          never partway). The @c lookahead_ms field MUST equal the value
  ///          that was last passed to @ref prepare — the lookahead delay lines
  ///          are sized in @ref prepare and not reallocated here.
  void set_config(const SidechainRouterConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const SidechainRouterConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = range_db (clamped to >= 0)
  // lookahead_ms and the sidechain HPF settings are omitted because they resize
  // buffers or are gated by mode switches.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=ratio, 2=attackMs, 3=releaseMs, 4=rangeDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const SidechainRouterConfig& config);
  static float gain_reduction_db(float input_db, const SidechainRouterConfig& config);
  /// @brief Verifies the prepared lookahead/HPF state can cover @p num_channels.
  /// @details RT-safe: never resizes on the audio thread. Per-channel state is
  ///          preallocated to @c kRealtimePreparedChannels in prepare(); a block
  ///          (or sidechain) requesting more channels throws instead of
  ///          allocating, mirroring @ref Limiter::prepare_buffers.
  void ensure_capacity(int num_channels) const;
  /// @brief Computes the shared (linked) detector level for a single sample.
  /// @details Reads the detector source (external sidechain when set, otherwise
  ///          the main channels), applies the per-channel sidechain HPF once per
  ///          source channel when enabled, then folds the source down to a
  ///          single linked detector value (mono sum when @c mono_summing,
  ///          otherwise the loudest channel). Because the HPF runs exactly once
  ///          per source channel per sample, the detector is never
  ///          double-filtered across output channels.
  float detector_sample(float* const* channels, int num_channels, int sample,
                        const SidechainRouterConfig& cfg);
  /// @brief Recomputes scalar derived coefficients (envelope follower attack/
  ///        release and the sidechain HPF) from @p config. RT-safe: scalar
  ///        math only, no allocation. Does NOT resize lookahead buffers — those
  ///        stay sized to whatever @ref prepare was called with.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const SidechainRouterConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const SidechainRouterConfig* adopt_snapshot_for_block() noexcept;

  SidechainRouterConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_->current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block. Held by @c unique_ptr so
  ///        SidechainRouter itself remains move-constructible (RtPublisher
  ///        deletes its copy and move operations to keep the SPSC ring indices
  ///        position-stable); the indirection is touched once per block from
  ///        the audio thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<SidechainRouterConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const SidechainRouterConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
  // A single shared (linked) envelope follower so every output channel receives
  // the same gain, preserving the stereo image (mirrors Compressor/Limiter).
  sonare::rt::EnvelopeFollower follower_;
  // Per-channel main-signal delay lines; the gain is linked, so a single gain
  // delay line is sufficient. Both are sized to kRealtimePreparedChannels in
  // prepare() and never resized on the audio thread.
  std::vector<sonare::rt::LookaheadBuffer> lookahead_;
  sonare::rt::LookaheadBuffer gain_lookahead_;
  // Per-source-channel one-pole HPF state for the sidechain detector. Each
  // source channel is filtered exactly once per sample (no shared index, no
  // double-filtering). Sized to kRealtimePreparedChannels in prepare().
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
  float hpf_b0_ = 1.0f;
  float hpf_a1_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
