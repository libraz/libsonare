#pragma once

/// @file character_stage.h
/// @brief Megaphone, radio, BBD chorus and strobe colouration stage.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "editing/voice_changer/voice_transform_types.h"
#include "rt/adaa.h"
#include "rt/biquad_design.h"
#include "rt/nonlinearities.h"

namespace sonare::editing::voice_changer {

/// Mono colouration stage. Every fixed corner is held in hertz and its coefficients are built for
/// the prepared sample rate, clamped to kMaxDesignFrequencyRatio of it.
class CharacterStage {
 public:
  /// Sets the sample rate and allocates the chorus delay line; not realtime-safe.
  void prepare(double sr);
  /// Clears every filter, delay and modulation state.
  void reset();
  /// Applies a configuration without allocating.
  void set_config(const CharacterConfig&);
  /// Processes one sample.
  float process_sample(float x) noexcept;

 private:
  void apply_config() noexcept;
  float corner_hz(float hz) const noexcept;
  void clear_state() noexcept;

  double sr_ = 0.0;
  CharacterConfig config_{};

  // Megaphone.
  rt::BiquadState band_;
  rt::Adaa1<rt::TanhNonlinearity> drive_;
  float drive_gain_ = 1.0f;
  float drive_trim_ = 1.0f;

  // Radio.
  rt::BiquadState radio_hp_[2];
  rt::BiquadState radio_lp_[2];
  float hold_step_ = 1.0f;
  float hold_phase_ = 1.0f;
  float held_ = 0.0f;
  float noise_peak_ = 0.0f;
  std::uint32_t noise_state_ = 0x9E3779B9u;

  // BBD chorus.
  std::vector<float> delay_;
  std::size_t write_index_ = 0;
  double lfo_phase_ = 0.0;
  double lfo_step_ = 0.0;
  float center_delay_samples_ = 0.0f;
  float depth_delay_samples_ = 0.0f;
  float chorus_mix_ = 0.0f;
  rt::BiquadState wet_lp_;

  // Strobo.
  double gate_phase_ = 0.0;
  double gate_step_ = 0.0;
  float gate_ = 1.0f;
  float gate_slew_ = 0.0f;
  float gate_depth_ = 0.0f;
};

}  // namespace sonare::editing::voice_changer
