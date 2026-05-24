#include "analysis/voice_changer/voice_changer.h"

#include <cmath>

#include "effects/pitch_shift.h"
#include "util/exception.h"

namespace sonare::analysis::voice_changer {

VoiceChanger::VoiceChanger(VoiceChangerConfig config) : config_(config) {}

Audio VoiceChanger::process(const Audio& audio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  PitchShiftConfig pitch_config;
  pitch_config.backend = config_.backend;
  Audio shifted = std::abs(config_.pitch_semitones) > 1.0e-6f
                      ? pitch_shift(audio, config_.pitch_semitones, pitch_config)
                      : audio;

  FormantWarpConfig formant_config;
  formant_config.factor = config_.formant_factor;
  FormantWarp warp(formant_config);
  return warp.process(shifted);
}

}  // namespace sonare::analysis::voice_changer
