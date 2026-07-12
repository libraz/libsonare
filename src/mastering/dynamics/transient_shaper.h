#pragma once

/// @file transient_shaper.h
/// @brief Envelope-difference transient shaper for attack and sustain control.

#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
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

class TransientShaper : public rt::ProcessorBase,
                        public rt::RtConfigLifecycle<TransientShaper, TransientShaperConfig> {
  using ConfigBase = rt::RtConfigLifecycle<TransientShaper, TransientShaperConfig>;
  friend ConfigBase;

 public:
  explicit TransientShaper(TransientShaperConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (TransientShaper::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope followers and gain
  // smoother) are recomputed on the audio thread when the snapshot is adopted,
  // so no per-channel state is written concurrently with processing. The
  // lookahead_ms field MUST equal the value last passed to prepare() — the
  // lookahead delay lines are sized in prepare() and not reallocated here.
  float last_gain_db() const { return last_gain_db_; }

  /// Reports the lookahead delay so the host can compensate (PDC), matching the
  /// other lookahead-bearing dynamics processors.
  int latency_samples() const noexcept override { return lookahead_samples_; }

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
  // Automatable parameters: 0=attackGainDb, 1=sustainGainDb, 2=fastAttackMs,
  //   3=fastReleaseMs, 4=slowAttackMs, 5=slowReleaseMs, 6=sensitivity,
  //   7=maxGainDb, 8=gainSmoothingMs.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

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
  // Cached lookahead delay in samples (set in prepare()), reported as latency.
  int lookahead_samples_ = 0;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
