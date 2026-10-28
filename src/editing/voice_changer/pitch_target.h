#pragma once

/// @file pitch_target.h
/// @brief Turns f0 estimates, scale settings and MIDI notes into per-voice pitch targets.

#include "editing/voice_changer/voice_changer.h"
#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

class PitchTargetController {
 public:
  void prepare(double sr);
  void reset();
  void set_config(const PitchCorrectionConfig&, const HarmonyConfig&, const MidiConfig&,
                  float semitones, float formant_factor, FormantMode mode);
  void apply_note(const NoteEvent&) noexcept;
  void all_notes_off() noexcept;
  VoiceTargets update(const F0Estimate* e, int count, int block_samples) noexcept;
};

}  // namespace sonare::editing::voice_changer
