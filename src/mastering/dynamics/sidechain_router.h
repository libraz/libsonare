#pragma once

/// @file sidechain_router.h
/// @brief Sidechain ducking processor with optional external detector input.

#include <memory>
#include <vector>

#include "rt/envelope_follower.h"
#include "rt/lookahead_buffer.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
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

class SidechainRouter : public rt::ProcessorBase,
                        public rt::RtConfigLifecycle<SidechainRouter, SidechainRouterConfig> {
  using ConfigBase = rt::RtConfigLifecycle<SidechainRouter, SidechainRouterConfig>;
  friend ConfigBase;

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

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (SidechainRouter::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope follower attack/
  // release and the sidechain HPF) are recomputed on the audio thread when the
  // snapshot is adopted, so no per-channel state is written concurrently with
  // processing. The lookahead_ms field MUST equal the value last passed to
  // prepare() — the lookahead delay lines are sized in prepare() and not
  // reallocated here.
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
