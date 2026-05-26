#pragma once

/// @file voice_changer.h
/// @brief Offline voice changer facade combining pitch and formant controls.

#include "core/audio.h"
#include "editing/voice_changer/formant_warp.h"
#include "effects/time_stretch.h"

namespace sonare::editing::voice_changer {

struct VoiceChangerConfig {
  float pitch_semitones = 0.0f;
  float formant_factor = 1.0f;
  StretchBackend backend = StretchBackend::NativeSpectral;
};

class VoiceChanger {
 public:
  explicit VoiceChanger(VoiceChangerConfig config = {});

  Audio process(const Audio& audio) const;
  const VoiceChangerConfig& config() const noexcept { return config_; }

 private:
  VoiceChangerConfig config_{};
};

}  // namespace sonare::editing::voice_changer
