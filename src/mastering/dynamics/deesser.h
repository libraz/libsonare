#pragma once

/// @file deesser.h
/// @brief Split-band de-esser for attenuating sibilant high-frequency energy.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
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

class DeEsser : public rt::ProcessorBase, public rt::RtConfigLifecycle<DeEsser, DeEsserConfig> {
  using ConfigBase = rt::RtConfigLifecycle<DeEsser, DeEsserConfig>;
  friend ConfigBase;

 public:
  explicit DeEsser(DeEsserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (DeEsser::validate_config) before publishing a lock-free snapshot
  // the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (bandpass biquads / envelope
  // follower) are recomputed on the audio thread when the snapshot is adopted,
  // so no per-channel state is written concurrently with processing.
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

  using Biquad = rt::BiquadState;

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  Biquad filter_coeffs_;
  std::vector<Biquad> bandpass_;
  std::vector<Biquad> bandpass2_;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
