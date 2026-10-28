#pragma once

/// @file channel_vocoder.h
/// @brief Channel vocoder with an internal, ensemble or external carrier.

#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

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
};

}  // namespace sonare::editing::voice_changer
