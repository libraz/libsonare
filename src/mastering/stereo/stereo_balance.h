#pragma once

/// @file stereo_balance.h
/// @brief Left/right balance and pan style gain processor.

#include <vector>

#include "rt/gain_pair_glide.h"
#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

/// Which curve pair the balance is read through.
enum class StereoBalanceLaw {
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
/// renders exactly as a fixed gain pair. The glide moves the balance position
/// and reads the law at every step, so a constant-power move keeps its power;
/// a law change starts from the gains being applied and decays the difference to
/// the new law over the same 5 ms, so a change that interrupts a change never
/// steps.
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
  /// The target law's gains at the glide's current position.
  rt::GainPairGlide::Pair law_gains() const;

  StereoBalanceConfig config_{};
  bool prepared_ = false;
  /// Gain glide: length in samples, built in prepare() from the fixed 5 ms.
  int ramp_samples_ = 1;
  /// False until the first block after prepare()/reset(), which lands on its
  /// target instead of gliding to it from unity.
  bool primed_ = false;
  /// The pair applied to the audio; law changes only retarget it.
  rt::GainPairGlide gain_glide_;
  /// Balance position glide, and the law being reached.
  float position_ = 0.0f;
  float target_position_ = 0.0f;
  float step_position_ = 0.0f;
  StereoBalanceConfig target_law_{};
  int ramp_remaining_ = 0;
};

}  // namespace sonare::mastering::stereo
