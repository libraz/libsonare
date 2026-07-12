#pragma once

/// @file parallel_comp.h
/// @brief Parallel compressor with dry/wet blend.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct ParallelCompConfig {
  float threshold_db = -18.0f;
  float ratio = 4.0f;
  float attack_ms = 10.0f;
  float release_ms = 100.0f;
  float makeup_gain_db = 0.0f;
  float mix = 0.5f;
  bool linked_detection = true;
  bool output_limiter = true;
  float output_ceiling_db = 0.0f;
};

class ParallelComp : public rt::ProcessorBase,
                     public rt::RtConfigLifecycle<ParallelComp, ParallelCompConfig> {
  using ConfigBase = rt::RtConfigLifecycle<ParallelComp, ParallelCompConfig>;
  friend ConfigBase;

 public:
  explicit ParallelComp(ParallelCompConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (ParallelComp::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope follower attack/
  // release) are recomputed on the audio thread when the snapshot is adopted,
  // so no per-channel state is written concurrently with processing.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = makeup_gain_db
  //   5 = mix (clamped to [0, 1])
  //   6 = output_ceiling_db
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=ratio, 2=attackMs, 3=releaseMs, 4=makeupGainDb, 5=mix,
  // 6=outputCeilingDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const ParallelCompConfig& config);
  static float gain_reduction_db(float input_db, const ParallelCompConfig& config);
  float limit_output_sample(float sample, size_t channel_index, float ceiling,
                            const ParallelCompConfig& config) noexcept;
  void ensure_followers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (envelope follower
  ///        attack/release) from @p config. RT-safe: scalar math only, no
  ///        allocation; the follower rewrites preserve envelope state.
  void update_coefficients(const ParallelCompConfig& config);

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  std::vector<float> limiter_gains_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
