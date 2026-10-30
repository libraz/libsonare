#pragma once

/// @file space_stage.h
/// @brief Reverb and echo-family space stage.

#include <cstddef>
#include <vector>

#include "editing/voice_changer/streaming_reverb.h"
#include "editing/voice_changer/voice_transform_types.h"
#include "rt/biquad_design.h"
#include "rt/param_smoother.h"

namespace sonare::editing::voice_changer {

/// Longest delay in milliseconds an echo-family variation may use.
constexpr float kMaxSpaceDelayMs = 3000.0f;

/// @brief Delay time in milliseconds a variation asks for.
/// @details Echo and dub echo use `delay_ms`; tempo delay uses
///          `kSecondsPerMinute * 1000 / tempo_bpm * note value`. Reverb has no delay (0).
///          A tempo that is not positive (or NaN) yields infinity.
float space_delay_ms(const ReverbVariationConfig& variation) noexcept;

/// @brief True when the computed delay exceeds kMaxSpaceDelayMs (or is NaN).
/// @details Config validation refuses such a setting; SpaceStage::set_config ignores it.
bool space_delay_exceeds_limit(const ReverbVariationConfig& variation) noexcept;

class SpaceStage {
 public:
  /// @brief Allocates the delay ring. NOT realtime-safe.
  void prepare(double sr);
  /// @brief Clears all delay and filter state.
  void reset();
  /// @brief Applies a configuration; realtime-safe. A delay beyond kMaxSpaceDelayMs is
  ///        ignored and the previous echo setting stays in force. The reverb variation is not
  ///        rendered here (@ref process_sample passes it through).
  void set_config(const StreamingReverbConfig&, const ReverbVariationConfig&);
  float process_sample(float x) noexcept;

 private:
  static constexpr float kGlideMs = 60.0f;
  static constexpr float kDubCornerHz = 2500.0f;
  static constexpr float kMaxMix = 0.45f;

  double sr_ = 0.0;
  ReverbVariation variation_ = ReverbVariation::Reverb;
  std::vector<float> ring_;
  std::size_t write_ = 0;
  rt::ParamSmoother delay_samples_;
  rt::ParamSmoother feedback_;
  rt::ParamSmoother mix_;
  rt::BiquadState dub_lowpass_;
  bool snap_ = true;
};

}  // namespace sonare::editing::voice_changer
