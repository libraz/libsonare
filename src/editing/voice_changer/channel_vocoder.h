#pragma once

/// @file channel_vocoder.h
/// @brief Channel vocoder with an internal, ensemble or external carrier.

#include <array>
#include <vector>

#include "editing/voice_changer/voice_transform_types.h"
#include "rt/biquad_design.h"

namespace sonare::editing::voice_changer {

/// Read access to the designed filter bank for tests.
struct ChannelVocoderTestAccess;

class ChannelVocoder {
 public:
  void prepare(double sr, int max_block, int bands);
  void reset();
  /// `bands` is fixed by prepare() and ignored here.
  void set_config(const VocoderConfig&, float formant_alpha);
  /// `external_carrier` must be non-NULL when the carrier source is external.
  void process(const float* modulator, const float* external_carrier, const VoiceTargets& notes,
               float* out, int n) noexcept;
  int group_delay_samples() const noexcept;

 private:
  friend struct ChannelVocoderTestAccess;

  static constexpr int kMaxBands = 32;
  static constexpr int kStages = 2;
  static constexpr int kOscPerVoice = 3;

  struct Band {
    float centre_hz = 0.0f;
    float low_edge_hz = 0.0f;
    float high_edge_hz = 0.0f;
    rt::BiquadCoeffs coeffs[kStages];
    rt::BiquadState modulator[kStages];
    rt::BiquadState carrier[kStages];
    float envelope = 0.0f;
  };

  void design_bank();
  void update_formant_map();
  void render_carrier(const VoiceTargets& notes, const float* external_carrier, int n) noexcept;

  double sr_ = 0.0;
  int bands_ = 0;
  int group_delay_ = 0;
  float sibilance_corner_hz_ = 0.0f;

  VocoderConfig config_;
  float formant_alpha_ = 1.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;

  std::array<Band, kMaxBands> band_;
  // Formant map: band k's carrier envelope = lerp(env[map_index_[k]], env[map_index_[k] + 1]).
  std::array<int, kMaxBands> map_index_{};
  std::array<float, kMaxBands> map_frac_{};
  std::array<float, kMaxBands> gain_{};

  rt::BiquadState sibilance_hp_;
  std::vector<float> carrier_;

  // Oscillator phases in cycles, per voice slot and detuned copy.
  std::array<std::array<double, kOscPerVoice>, kMaxVoices> phase_{};
};

}  // namespace sonare::editing::voice_changer
