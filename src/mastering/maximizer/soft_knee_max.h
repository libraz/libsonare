#pragma once

#include <cstdint>
#include <vector>

#include "mastering/maximizer/maximizer.h"
#include "rt/overflow_counter.h"

namespace sonare::mastering::maximizer {

struct SoftKneeMaxConfig {
  float input_gain_db = 0.0f;
  float ceiling_db = -1.0f;
  float knee_db = 6.0f;
  float release_ms = 50.0f;
};

class SoftKneeMax : public rt::ProcessorBase {
 public:
  explicit SoftKneeMax(SoftKneeMaxConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftKneeMaxConfig& config);
  const SoftKneeMaxConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return maximizer_.last_gain_reduction_db(); }
  /// @brief Non-finite samples this stage replaced with a finite in-domain one.
  /// @details Sums the knee's own substitutions and the inner maximizer's. The
  ///          knee runs before the maximizer and folds an infinity onto twice the
  ///          knee, so the maximizer never sees it; a NaN passes the knee and is
  ///          counted there instead. Monotonic since @ref prepare, which clears
  ///          both; @ref reset does not.
  std::uint32_t non_finite_substitution_count() const noexcept {
    return non_finite_substitution_count_.load() + maximizer_.non_finite_substitution_count();
  }
  // The soft-knee shaping is a memoryless pre-stage, so the delay is entirely
  // the inner maximizer's (in turn its limiter's lookahead).
  int latency_samples() const noexcept override { return maximizer_.latency_samples(); }

  // Parameters:
  //   0 = input_gain_db (applied per block, no coefficients)
  //   1 = ceiling_db (clamped <= 0; not audio-thread safe, rejected by mixer automation)
  //   2 = knee_db (clamped >= 0; applied per block, no coefficients)
  //   3 = release_ms (clamped >= 0; in-place via inner maximizer)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=inputGainDb, 1=ceilingDb, 2=kneeDb, 3=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

 private:
  static void validate_config(const SoftKneeMaxConfig& config);

  SoftKneeMaxConfig config_{};
  Maximizer maximizer_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  rt::OverflowCounter non_finite_substitution_count_{};
};

}  // namespace sonare::mastering::maximizer
