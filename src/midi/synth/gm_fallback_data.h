#pragma once

#include <array>
#include <cstddef>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth::detail {

inline DahdsrConfig fallback_env(float attack_ms, float decay_ms, float sustain,
                                 float release_ms) noexcept {
  DahdsrConfig config;
  config.attack_ms = attack_ms;
  config.decay_ms = decay_ms;
  config.sustain = sustain;
  config.release_ms = release_ms;
  return config;
}

struct ProgramOverrides {
  NativeSynthPatch e_piano;             // programs 4-5 (Electric Piano 1/2)
  NativeSynthPatch harpsichord;         // program 6 bank 0 (Harpsichord 8', KS)
  NativeSynthPatch harpsichord_octave;  // program 6 bank 1 (octave mix, 8'+4')
  NativeSynthPatch harpsichord_wide;    // program 6 bank 2 (wide)
  NativeSynthPatch harpsichord_keyoff;  // program 6 bank 3 (with key off)
  NativeSynthPatch clav;                // program 7 (Clavi, FM)
  NativeSynthPatch celesta;             // program 8 (soft mallet bar, modal)
  NativeSynthPatch glockenspiel;        // program 9 (uniform-bar modal)
  NativeSynthPatch music_box;           // program 10 (metallic comb tine, modal)
  NativeSynthPatch vibraphone;          // program 11 (tuned-bar modal, long)
  NativeSynthPatch marimba;             // program 12 (tuned-bar modal, woody)
  NativeSynthPatch xylophone;           // program 13 (quint-tuned modal, dry)
  NativeSynthPatch tubular_bells;       // program 14 (long-ringing bell, modal)
  NativeSynthPatch dulcimer;            // program 15 (hammered string, KS)
  NativeSynthPatch nylon_guitar;        // program 24
  NativeSynthPatch electric_guitar;     // programs 26-27 (jazz / clean)
  NativeSynthPatch muted_guitar;        // program 28 (palm mute)
  NativeSynthPatch overdriven;          // program 29
  NativeSynthPatch distortion;          // program 30
  NativeSynthPatch bass_acoustic;       // program 32 (Acoustic Bass)
  NativeSynthPatch bass_fingered;       // program 33 (Electric Bass, finger)
  NativeSynthPatch bass_picked;         // program 34 (Electric Bass, pick)
  NativeSynthPatch bass_fretless;       // program 35 (Fretless Bass)
  NativeSynthPatch bass_slap;           // program 36 (Slap Bass 1, thumb)
  NativeSynthPatch bass_pop;            // program 37 (Slap Bass 2, pull/pop)
  NativeSynthPatch harp;                // program 46 (Orchestral Harp)
  NativeSynthPatch sitar;               // program 104 (buzzing jawari bridge)
  NativeSynthPatch shamisen;            // program 106 (sawari buzzing bridge)
  NativeSynthPatch koto;                // program 107 (bridge-buzz plucked)
  NativeSynthPatch church_organ;        // program 19 (Church Organ, flue pipe)
  NativeSynthPatch reed_organ;          // programs 20-21 (Reed Organ / Accordion, free reed)
  NativeSynthPatch harmonica;           // program 22 (free reed, bright hand vibrato)
  NativeSynthPatch bandoneon;           // program 23 (musette-detuned free reed)
  NativeSynthPatch orchestra_hit;       // program 55 (bright detuned-saw stab)
  NativeSynthPatch tremolo_strings;     // program 44 (measured-bow amp tremolo)
  NativeSynthPatch pizzicato;           // program 45 (Pizzicato Strings, KS + corpus)
  NativeSynthPatch timpani;             // program 47 (kettledrum membrane)
  NativeSynthPatch choir_aahs;          // program 52 (open-vowel vocal body)
  NativeSynthPatch voice_oohs;          // program 53 (darker closed vowel)
  NativeSynthPatch synth_voice;         // program 54 (brighter synthetic vowel)
  NativeSynthPatch tinkle_bell;         // program 112 (high metal chime, percussion)
  NativeSynthPatch agogo;               // program 113 (two-tone metal bell)
  NativeSynthPatch steel_drums;         // program 114 (tuned steel pan)
  NativeSynthPatch woodblock;           // program 115 (struck wood block)
  NativeSynthPatch taiko;               // program 116 (large struck membrane)
  NativeSynthPatch melodic_tom;         // program 117 (pitched tom membrane)
  NativeSynthPatch synth_drum;          // program 118 (synthetic decaying-sine tom)
  NativeSynthPatch reverse_cymbal;      // program 119 (noise-swell approximation)

  // Physical-model acoustic families (bowed string / reed / brass / air-jet
  // flute). These mirror the calibration of the like-named entries in the synth
  // preset catalog. The values are duplicated here rather than pulled from
  // find_synth_preset() on purpose: build_presets() itself calls
  // gm_fallback_patch() to voice several of its presets, so having this table
  // depend on the preset catalog would form a static-initialisation cycle.
  NativeSynthPatch violin;         // program 40
  NativeSynthPatch viola;          // program 41
  NativeSynthPatch cello;          // program 42
  NativeSynthPatch contrabass;     // program 43
  NativeSynthPatch trumpet;        // program 56
  NativeSynthPatch trombone;       // program 57
  NativeSynthPatch tuba;           // program 58
  NativeSynthPatch muted_trumpet;  // program 59
  NativeSynthPatch french_horn;    // program 60
  NativeSynthPatch soprano_sax;    // program 64
  NativeSynthPatch alto_sax;       // program 65
  NativeSynthPatch tenor_sax;      // program 66
  NativeSynthPatch baritone_sax;   // program 67
  NativeSynthPatch oboe;           // program 68
  NativeSynthPatch english_horn;   // program 69
  NativeSynthPatch bassoon;        // program 70
  NativeSynthPatch clarinet;       // program 71
  NativeSynthPatch piccolo;        // program 72
  NativeSynthPatch concert_flute;  // program 73
  NativeSynthPatch recorder;       // program 74
  NativeSynthPatch pan_flute;      // program 75
  NativeSynthPatch blown_bottle;   // program 76
  NativeSynthPatch shakuhachi;     // program 77
  NativeSynthPatch tin_whistle;    // program 78
  NativeSynthPatch ocarina;        // program 79
};

/// Number of override patches in ProgramOverrides. The struct is a homogeneous
/// aggregate of NativeSynthPatch members, so the count falls out of the sizes;
/// the asserts below keep that assumption enforced.
inline constexpr std::size_t kProgramOverrideCount =
    sizeof(ProgramOverrides) / sizeof(NativeSynthPatch);

static_assert(sizeof(ProgramOverrides) == kProgramOverrideCount * sizeof(NativeSynthPatch),
              "ProgramOverrides must contain only NativeSynthPatch members");
static_assert(alignof(ProgramOverrides) == alignof(NativeSynthPatch),
              "ProgramOverrides must contain only NativeSynthPatch members");

/// Contiguous view over every patch in a ProgramOverrides table. Whole-table
/// sweeps (e.g. the maximum release/decay bound in gm_fallback_max_release_ms)
/// iterate this view so a newly added override member is picked up
/// automatically instead of depending on a hand-maintained member list.
inline const NativeSynthPatch* program_override_patches(
    const ProgramOverrides& overrides) noexcept {
  return reinterpret_cast<const NativeSynthPatch*>(&overrides);
}

void configure_keyed_programs(ProgramOverrides& overrides) noexcept;
void configure_percussion_programs(ProgramOverrides& overrides) noexcept;
void configure_physical_programs(ProgramOverrides& overrides) noexcept;

const std::array<NativeSynthPatch, 16>& family_patches() noexcept;
const ProgramOverrides& program_overrides() noexcept;
const std::array<NativeSynthPatch, 128>& drum_note_table() noexcept;

}  // namespace sonare::midi::synth::detail
