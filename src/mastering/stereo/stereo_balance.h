#pragma once

/// @file stereo_balance.h
/// @brief Left/right balance and pan style gain processor.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

/// Which curve pair the balance is read through.
enum  /// A change of the balance is glided over a fixed 5 ms so a control moved
      /// between blocks does not step the gains; a balance that does not change
      /// renders exactly as a fixed gain pair.
    class StereoBalanceLaw {
      kNormalized,        ///< unity at centre (see `constant_power`); the default.
      kRawConstantPower,  ///< the constant-power curve's literal gains: centre is -3 dB.
    };
inline constexpr int kStereoBalanceLawCount = 2;

struct StereoBalanceConfig {
  float balance = 0.0f;
  bool constant_power = true;
  /// `kRawConstantPower` overrides `constant_power`.
  StereoBalanceLaw law = StereoBalanceLaw::kNormalized;
};

/// A change of the balance is glided over a fixed 5 ms so a control moved
/// between blocks does not step the gains; a balance that does not change
/// renders exactly as a fixed gain pair.
class StereoBalance : public rt::ProcessorBase {
 public:
  explicit StereoBalance(StereoBalanceConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const StereoBalanceConfig& config);
  const StereoBalanceConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = balance (clamped to [-1, 1])
  //   1 = law (StereoBalanceLaw; a fractional or unnamed value is refused)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=balance, 1=law
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const StereoBalanceConfig& config);
  static void gains(const StereoBalanceConfig& config, float& left, float& right);

  StereoBalanceConfig config_{};
  bool prepared_ = false;
  /// Gain glide: length in samples, built in prepare() from the fixed 5 ms.
  int ramp_samples_ = 1;
  /// False until the first block after prepare()/reset(), which lands on its
  /// target instead of gliding to it from unity.
  bool primed_ = false;
  float left_gain_ = 1.0f;
  float right_gain_ = 1.0f;
  float target_left_ = 1.0f;
  float target_right_ = 1.0f;
  float step_left_ = 0.0f;
  float step_right_ = 0.0f;
  int ramp_remaining_ = 0;
};

}  // namespace sonare::mastering::stereo
