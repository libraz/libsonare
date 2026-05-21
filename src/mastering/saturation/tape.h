#pragma once

/// @file tape.h
/// @brief Analog tape saturation modelled with a Jiles-Atherton hysteresis loop.

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

/// @brief Tape saturation configuration.
/// @reference Chowdhury "Real-time Physical Modelling for Analog Tape Machines"
///            DAFx-19 (2019) — algorithmic inspiration only, no code reuse.
struct TapeConfig {
  /// Pre-saturation input drive (-12 dB ... +24 dB recommended).
  float drive_db = 3.0f;
  /// Wet/dry mix and saturation aggressiveness in [0, 1]. Maps to the
  /// anhysteretic shape parameter `a` (small `a` saturates earlier).
  float saturation = 0.5f;
  /// Hysteresis loop width in [0, 1]. Maps to the loss/coercivity parameter
  /// `k` (larger value → wider hysteresis loop → more low-end thickening).
  float hysteresis = 0.2f;
  /// Post-saturation output trim in dB.
  float output_gain_db = 0.0f;
};

class Tape : public common::ProcessorBase {
 public:
  explicit Tape(TapeConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TapeConfig& config);
  const TapeConfig& config() const { return config_; }

 private:
  /// Per-channel Jiles-Atherton state.
  struct JaState {
    float M = 0.0f;       // current magnetization (output)
    float H_prev = 0.0f;  // previous input field
  };

  static void validate_config(const TapeConfig& config);
  static float db_to_linear(float db);
  static float langevin(float x);
  static float langevin_derivative(float x);
  float process_sample(JaState& state, float input) const;
  void ensure_state(int num_channels);

  TapeConfig config_{};
  bool prepared_ = false;
  std::vector<JaState> states_;
};

}  // namespace sonare::mastering::saturation
