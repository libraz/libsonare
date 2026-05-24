#pragma once

/// @file tape.h
/// @brief Analog tape saturation modelled with a Jiles-Atherton hysteresis loop.

#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "mastering/common/oversampler.h"
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
  float speed_ips = 15.0f;
  float head_bump_db = 1.5f;
  float bias = 0.0f;
  float gap_loss = 0.2f;
  /// J-A core oversampling: 1 (default, no oversampling), 2, or 4. >1 reduces
  /// aliasing at high drive; intended for offline/whole-buffer rendering (the
  /// offline oversampler is stateless, so values >1 may produce minor
  /// block-edge artifacts in per-block streaming use).
  int oversample_factor = 1;
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
  static void validate_config(const TapeConfig& config);
  static common::JilesAthertonConfig make_ja_config(const TapeConfig& config);
  float process_sample(common::JilesAthertonState& state, float input) const;
  void ensure_state(int num_channels);
  void update_filters(double sample_rate);

  struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
    float process(float x);
    void reset();
  };

  TapeConfig config_{};
  common::JilesAtherton hysteresis_;
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  Biquad head_bump_coeffs_;
  float gap_loss_coeff_ = 0.0f;
  std::vector<common::JilesAthertonState> states_;
  std::vector<Biquad> head_bump_;
  std::vector<float> gap_state_;
  common::Oversampler oversampler_;
};

}  // namespace sonare::mastering::saturation
