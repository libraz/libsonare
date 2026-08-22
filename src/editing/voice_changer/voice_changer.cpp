#include "editing/voice_changer/voice_changer.h"

#include <cmath>

#include "effects/pitch_shift.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {

VoiceChanger::VoiceChanger(VoiceChangerConfig config) : config_(config) {}

Audio VoiceChanger::process(const Audio& audio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  // The epsilon test below answers "is this near zero", and a NaN answers false
  // to every comparison -- so an unguarded NaN skipped the pitch branch
  // entirely and returned the input unchanged while reporting success. The
  // formant sibling is already checked inside FormantWarp; check this one here,
  // in the core, so all four surfaces and both CLIs inherit it rather than each
  // growing its own copy. +/-Inf needs no case here: it enters the branch and
  // pitch_shift rejects it.
  SONARE_CHECK(std::isfinite(config_.pitch_semitones), ErrorCode::InvalidParameter);

  PitchShiftConfig pitch_config;
  pitch_config.backend = config_.backend;
  Audio shifted = std::abs(config_.pitch_semitones) > 1.0e-6f
                      ? pitch_shift(audio, config_.pitch_semitones, pitch_config)
                      : audio;

  if (std::abs(config_.formant_factor - 1.0f) < 1.0e-6f) return shifted;

  FormantWarpConfig formant_config;
  formant_config.factor = config_.formant_factor;
  FormantWarp warp(formant_config);
  return warp.process(shifted);
}

}  // namespace sonare::editing::voice_changer
