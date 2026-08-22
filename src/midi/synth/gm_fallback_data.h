#pragma once

#include <array>
#include <cstddef>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth::detail {

constexpr DahdsrConfig fallback_env(float attack_ms, float decay_ms, float sustain,
                                    float release_ms) noexcept {
  DahdsrConfig config{};
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

  // GS variation tones (gm_fallback_map.cpp's kGsVariationPatches). A variation
  // hangs under a capital tone at a Bank Select MSB (CC#0) number and differs in
  // character, not family, so each of these derives from the capital's patch and
  // re-voices it. Only variations the model floor can genuinely voice
  // differently get an entry: everything else keeps falling back to its capital,
  // which is the GS rule a real module follows for a variation it does not have.
  NativeSynthPatch piano_wide;            // program 0 variation 8 (two-choir spread)
  NativeSynthPatch piano_dark;            // program 0 variation 16 (mellow lid-down)
  NativeSynthPatch vibraphone_wide;       // program 11 variation 8
  NativeSynthPatch marimba_wide;          // program 12 variation 8
  NativeSynthPatch church_bell;           // program 14 variation 8 (cast bell, not tube)
  NativeSynthPatch carillon;              // program 14 variation 9 (small tuned bell)
  NativeSynthPatch church_organ_flutes;   // program 19 variation 8 (flute registration)
  NativeSynthPatch church_organ_full;     // program 19 variation 16 (full organ + reeds)
  NativeSynthPatch accordion_italian;     // program 21 variation 8 (wider musette)
  NativeSynthPatch ukulele;               // program 24 variation 8
  NativeSynthPatch nylon_guitar_keyoff;   // program 24 variation 16 (with key off)
  NativeSynthPatch twelve_string_guitar;  // program 25 variation 8
  NativeSynthPatch mandolin;              // program 25 variation 16
  NativeSynthPatch violin_slow;           // program 40 variation 8 (slow bow attack)
  NativeSynthPatch e_piano_detuned_1;     // program 4 variation 8 (Detuned EP 1)
  NativeSynthPatch e_piano_velocity_1;    // program 4 variation 16 (velocity mix)
  NativeSynthPatch e_piano_sixties;       // program 4 variation 24 (60's reed EP)
  NativeSynthPatch e_piano_detuned_2;     // program 5 variation 8 (Detuned EP 2)
  NativeSynthPatch e_piano_velocity_2;    // program 5 variation 16 (velocity mix)
  NativeSynthPatch organ_detuned_1;       // program 16 variation 8 (chorused tonewheel)
  NativeSynthPatch organ_sixties;         // program 16 variation 16 (60's registration)
  NativeSynthPatch organ_4;               // program 16 variation 32 (full drawbars)
  NativeSynthPatch organ_detuned_2;       // program 17 variation 8 (chorused, darker)
  NativeSynthPatch organ_5;               // program 17 variation 32 (bright upper drawbars)
};

/// Every ProgramOverrides member, in declaration order, as an X-macro list.
/// One list serves both the post-configure clamp sweep and the development-only
/// per-patch tuning key (`patch_tuning.h`), so the member names are written
/// once; the count assertion below fires if a member is added to the struct
/// without being added here.
#define SONARE_GM_OVERRIDE_PATCHES(X) \
  X(e_piano)                          \
  X(harpsichord)                      \
  X(harpsichord_octave)               \
  X(harpsichord_wide)                 \
  X(harpsichord_keyoff)               \
  X(clav)                             \
  X(celesta)                          \
  X(glockenspiel)                     \
  X(music_box)                        \
  X(vibraphone)                       \
  X(marimba)                          \
  X(xylophone)                        \
  X(tubular_bells)                    \
  X(dulcimer)                         \
  X(nylon_guitar)                     \
  X(electric_guitar)                  \
  X(muted_guitar)                     \
  X(overdriven)                       \
  X(distortion)                       \
  X(bass_acoustic)                    \
  X(bass_fingered)                    \
  X(bass_picked)                      \
  X(bass_fretless)                    \
  X(bass_slap)                        \
  X(bass_pop)                         \
  X(harp)                             \
  X(sitar)                            \
  X(shamisen)                         \
  X(koto)                             \
  X(church_organ)                     \
  X(reed_organ)                       \
  X(harmonica)                        \
  X(bandoneon)                        \
  X(orchestra_hit)                    \
  X(tremolo_strings)                  \
  X(pizzicato)                        \
  X(timpani)                          \
  X(choir_aahs)                       \
  X(voice_oohs)                       \
  X(synth_voice)                      \
  X(tinkle_bell)                      \
  X(agogo)                            \
  X(steel_drums)                      \
  X(woodblock)                        \
  X(taiko)                            \
  X(melodic_tom)                      \
  X(synth_drum)                       \
  X(reverse_cymbal)                   \
  X(violin)                           \
  X(viola)                            \
  X(cello)                            \
  X(contrabass)                       \
  X(trumpet)                          \
  X(trombone)                         \
  X(tuba)                             \
  X(muted_trumpet)                    \
  X(french_horn)                      \
  X(soprano_sax)                      \
  X(alto_sax)                         \
  X(tenor_sax)                        \
  X(baritone_sax)                     \
  X(oboe)                             \
  X(english_horn)                     \
  X(bassoon)                          \
  X(clarinet)                         \
  X(piccolo)                          \
  X(concert_flute)                    \
  X(recorder)                         \
  X(pan_flute)                        \
  X(blown_bottle)                     \
  X(shakuhachi)                       \
  X(tin_whistle)                      \
  X(ocarina)                          \
  X(piano_wide)                       \
  X(piano_dark)                       \
  X(vibraphone_wide)                  \
  X(marimba_wide)                     \
  X(church_bell)                      \
  X(carillon)                         \
  X(church_organ_flutes)              \
  X(church_organ_full)                \
  X(accordion_italian)                \
  X(ukulele)                          \
  X(nylon_guitar_keyoff)              \
  X(twelve_string_guitar)             \
  X(mandolin)                         \
  X(violin_slow)                      \
  X(e_piano_detuned_1)                \
  X(e_piano_velocity_1)               \
  X(e_piano_sixties)                  \
  X(e_piano_detuned_2)                \
  X(e_piano_velocity_2)               \
  X(organ_detuned_1)                  \
  X(organ_sixties)                    \
  X(organ_4)                          \
  X(organ_detuned_2)                  \
  X(organ_5)

/// Number of override patches in ProgramOverrides. The struct is a homogeneous
/// aggregate of NativeSynthPatch members, so the count falls out of the sizes;
/// the asserts below keep that assumption enforced.
inline constexpr std::size_t kProgramOverrideCount =
    sizeof(ProgramOverrides) / sizeof(NativeSynthPatch);

static_assert(sizeof(ProgramOverrides) == kProgramOverrideCount * sizeof(NativeSynthPatch),
              "ProgramOverrides must contain only NativeSynthPatch members");
#define SONARE_GM_OVERRIDE_COUNT_ONE(name) +1
static_assert(0 SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_OVERRIDE_COUNT_ONE) ==
                  static_cast<int>(kProgramOverrideCount),
              "SONARE_GM_OVERRIDE_PATCHES must list every ProgramOverrides member");
#undef SONARE_GM_OVERRIDE_COUNT_ONE
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

const std::array<NativeSynthPatch, 16>& family_patches() noexcept;
const ProgramOverrides& program_overrides() noexcept;
const std::array<NativeSynthPatch, 128>& drum_note_table() noexcept;

}  // namespace sonare::midi::synth::detail
