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

 private:
  bool in_scale(int note) const noexcept;
  int next_in_scale(int note, int dir) const noexcept;
  int snap(double midi) const noexcept;
  int scale_step(int note, int steps) const noexcept;
  void track_estimate(const F0Estimate& e) noexcept;
  void track_robot(double midi, double t) noexcept;
  void track_auto(double midi, double t) noexcept;

  PitchCorrectionConfig pc_{};
  HarmonyConfig hm_{};
  MidiConfig midi_{};
  float semitones_ = 0.0f;
  float formant_factor_ = 1.0f;
  FormantMode fmode_ = FormantMode::Relative;
  double sr_ = 48000.0;
  double time_s_ = 0.0;

  bool have_f0_ = false;
  double last_f0_hz_ = 0.0;

  bool auto_valid_ = false;
  int auto_note_ = 0;
  bool smooth_valid_ = false;
  double auto_smooth_midi_ = 0.0;
  int pend_note_ = 0;
  bool pend_valid_ = false;
  double pend_since_ = 0.0;

  bool centre_valid_ = false;
  double centre_midi_ = 0.0;
  double centre_t_ = 0.0;
  bool tonic_valid_ = false;
  int tonic_ = 0;

  int held_[kMaxHeldNotes] = {};
  int held_count_ = 0;
  int hold_note_ = -1;
};

}  // namespace sonare::editing::voice_changer
