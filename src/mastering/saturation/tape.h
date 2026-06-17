#pragma once

/// @file tape.h
/// @brief Analog tape saturation modelled with a Jiles-Atherton hysteresis loop.

#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "rt/biquad_design.h"
#include "rt/oversampler.h"
#include "rt/processor_base.h"

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

/// @brief Whether a tape config would impart audible coloration.
/// @details Tape is a color stage, so a config that supplies neither drive nor
/// saturation is a no-op. Parsers use this to decide whether to auto-engage the
/// stage when the caller did not pass an explicit `enabled` flag, so a
/// zero-drive/zero-saturation tape stays bypassed instead of adding grit.
inline bool tape_engages_color(const TapeConfig& config) {
  return config.drive_db > 0.0f || config.saturation > 0.0f;
}

class Tape : public rt::ProcessorBase {
 public:
  explicit Tape(TapeConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TapeConfig& config);
  const TapeConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset):
  //   0 = drive_db (read per sample)
  //   1 = saturation (clamped to [0, 1]; updates J-A anhysteretic shape)
  //   2 = hysteresis (clamped to [0, 1]; updates J-A coercivity)
  //   3 = output_gain_db (read per sample)
  //   4 = speed_ips (clamped to > 0; recomputes head-bump/gap filters in place)
  //   5 = head_bump_db (clamped to >= 0; recomputes head-bump filter in place)
  //   6 = bias (read per sample)
  //   7 = gap_loss (clamped to [0, 1]; read per sample)
  // J-A config updates only touch coefficients; the per-channel magnetization
  // state in states_ and the biquad delay state are preserved. oversample_factor
  // is a discrete mode and is not exposed.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=driveDb, 1=saturation, 2=hysteresis, 3=outputGainDb,
  //   4=speedIps, 5=headBumpDb, 6=bias, 7=gapLoss
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const TapeConfig& config);
  static common::JilesAthertonConfig make_ja_config(const TapeConfig& config);
  float process_sample(common::JilesAthertonState& state, float input) const;
  void ensure_state(int num_channels);
  void update_filters(double sample_rate);

  using Biquad = rt::BiquadState;

  TapeConfig config_{};
  common::JilesAtherton hysteresis_;
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  Biquad head_bump_coeffs_;
  float gap_loss_coeff_ = 0.0f;
  std::vector<common::JilesAthertonState> states_;
  std::vector<Biquad> head_bump_;
  std::vector<float> gap_state_;
  sonare::rt::Oversampler oversampler_;
  // Preallocated oversampling scratch (sized max_block_size_*oversample_factor
  // in prepare()) so the oversampled process() path never allocates on the
  // audio thread. up_scratch_ holds the upsampled J-A input/output; down_scratch_
  // holds the decimated base-rate result.
  std::vector<float> up_scratch_;
  std::vector<float> down_scratch_;
};

}  // namespace sonare::mastering::saturation
