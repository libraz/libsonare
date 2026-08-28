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
  NativeSynthPatch bright_piano;        // program 1 (the grand voiced hard)
  NativeSynthPatch electric_grand;      // program 2 (short strings, piezo, no board)
  NativeSynthPatch honky_tonk;          // program 3 (beating unisons)
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
  NativeSynthPatch steel_guitar;        // program 25 (Acoustic Guitar, steel)
  NativeSynthPatch electric_guitar;     // programs 26-27 (jazz / clean)
  NativeSynthPatch muted_guitar;        // program 28 (palm mute)
  NativeSynthPatch overdriven;          // program 29
  NativeSynthPatch distortion;          // program 30
  NativeSynthPatch guitar_harmonics;    // program 31 (touched node, flageolet)
  NativeSynthPatch bass_acoustic;       // program 32 (Acoustic Bass)
  NativeSynthPatch bass_fingered;       // program 33 (Electric Bass, finger)
  NativeSynthPatch bass_picked;         // program 34 (Electric Bass, pick)
  NativeSynthPatch bass_fretless;       // program 35 (Fretless Bass)
  NativeSynthPatch bass_slap;           // program 36 (Slap Bass 1, thumb)
  NativeSynthPatch bass_pop;            // program 37 (Slap Bass 2, pull/pop)
  NativeSynthPatch harp;                // program 46 (Orchestral Harp)
  NativeSynthPatch sitar;               // program 104 (buzzing jawari bridge)
  NativeSynthPatch banjo;               // program 105 (steel over a membrane head)
  NativeSynthPatch shamisen;            // program 106 (sawari buzzing bridge)
  NativeSynthPatch koto;                // program 107 (bridge-buzz plucked)
  NativeSynthPatch kalimba;             // program 108 (plucked steel tine, modal)
  NativeSynthPatch drawbar_organ;       // program 16 (tonewheel, 88 8402 001)
  NativeSynthPatch percussive_organ;    // program 17 (the same tonewheel plus percussion)
  NativeSynthPatch rock_organ;          // program 18 (fuller registration, driven)
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
  NativeSynthPatch lead_square;         // program 80 (Lead 1, square)
  NativeSynthPatch lead_saw;            // program 81 (Lead 2, sawtooth)
  NativeSynthPatch lead_calliope;       // program 82 (Lead 3, calliope)
  NativeSynthPatch lead_chiff;          // program 83 (Lead 4, chiff)
  NativeSynthPatch lead_charang;        // program 84 (Lead 5, charang)
  NativeSynthPatch lead_voice;          // program 85 (Lead 6, voice)
  NativeSynthPatch lead_fifths;         // program 86 (Lead 7, fifths)
  NativeSynthPatch lead_bass_lead;      // program 87 (Lead 8, bass + lead)
  NativeSynthPatch pad_new_age;         // program 88 (Pad 1, new age)
  NativeSynthPatch pad_warm;            // program 89 (Pad 2, warm)
  NativeSynthPatch pad_polysynth;       // program 90 (Pad 3, polysynth)
  NativeSynthPatch pad_choir;           // program 91 (Pad 4, choir)
  NativeSynthPatch pad_bowed;           // program 92 (Pad 5, bowed)
  NativeSynthPatch pad_metallic;        // program 93 (Pad 6, metallic)
  NativeSynthPatch pad_halo;            // program 94 (Pad 7, halo)
  NativeSynthPatch pad_sweep;           // program 95 (Pad 8, sweep)
  NativeSynthPatch fx_rain;             // program 96 (FX 1, rain)
  NativeSynthPatch fx_soundtrack;       // program 97 (FX 2, soundtrack)
  NativeSynthPatch fx_crystal;          // program 98 (FX 3, crystal)
  NativeSynthPatch fx_atmosphere;       // program 99 (FX 4, atmosphere)
  NativeSynthPatch fx_brightness;       // program 100 (FX 5, brightness)
  NativeSynthPatch fx_goblins;          // program 101 (FX 6, goblins)
  NativeSynthPatch fx_echoes;           // program 102 (FX 7, echoes)
  NativeSynthPatch fx_sci_fi;           // program 103 (FX 8, sci-fi)
  NativeSynthPatch woodblock;           // program 115 (struck wood block)
  NativeSynthPatch taiko;               // program 116 (large struck membrane)
  NativeSynthPatch melodic_tom;         // program 117 (pitched tom membrane)
  NativeSynthPatch synth_drum;          // program 118 (synthetic decaying-sine tom)
  NativeSynthPatch reverse_cymbal;      // program 119 (noise-swell approximation)
  NativeSynthPatch sfx_guitar_fret;     // program 120 (guitar fret noise)
  NativeSynthPatch sfx_breath;          // program 121 (breath noise)
  NativeSynthPatch sfx_seashore;        // program 122 (seashore)
  NativeSynthPatch sfx_bird_tweet;      // program 123 (bird tweet)
  NativeSynthPatch sfx_telephone_ring;  // program 124 (telephone ring)
  NativeSynthPatch sfx_helicopter;      // program 125 (helicopter)
  NativeSynthPatch sfx_applause;        // program 126 (applause)
  NativeSynthPatch sfx_gunshot;         // program 127 (gunshot)

  // Physical-model acoustic families (bowed string / reed / brass / air-jet
  // flute). These mirror the calibration of the like-named entries in the synth
  // preset catalog. The values are duplicated here rather than pulled from
  // find_synth_preset() on purpose: build_presets() itself calls
  // gm_fallback_patch() to voice several of its presets, so having this table
  // depend on the preset catalog would form a static-initialisation cycle.
  NativeSynthPatch violin;      // program 40
  NativeSynthPatch viola;       // program 41
  NativeSynthPatch cello;       // program 42
  NativeSynthPatch contrabass;  // program 43
  // 110 is the violin family again, played the other way; 48-49 are the same
  // string in section, where the players' spread is most of the timbre.
  NativeSynthPatch fiddle;             // program 110
  NativeSynthPatch string_ensemble_1;  // program 48
  NativeSynthPatch string_ensemble_2;  // program 49
  NativeSynthPatch trumpet;            // program 56
  NativeSynthPatch trombone;           // program 57
  NativeSynthPatch tuba;               // program 58
  NativeSynthPatch muted_trumpet;      // program 59
  NativeSynthPatch french_horn;        // program 60
  NativeSynthPatch brass_section;      // program 61 (lip reeds in section)
  NativeSynthPatch soprano_sax;        // program 64
  NativeSynthPatch alto_sax;           // program 65
  NativeSynthPatch tenor_sax;          // program 66
  NativeSynthPatch baritone_sax;       // program 67
  NativeSynthPatch oboe;               // program 68
  NativeSynthPatch english_horn;       // program 69
  NativeSynthPatch bassoon;            // program 70
  NativeSynthPatch clarinet;           // program 71
  // The two double reeds outside the orchestral family, both on the same cone.
  NativeSynthPatch bag_pipe;       // program 109
  NativeSynthPatch shanai;         // program 111
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
  NativeSynthPatch bright_piano_wide;     // program 1 variation 8
  NativeSynthPatch electric_grand_wide;   // program 2 variation 8
  NativeSynthPatch honky_tonk_wide;       // program 3 variation 8
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
  X(bright_piano)                     \
  X(electric_grand)                   \
  X(honky_tonk)                       \
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
  X(steel_guitar)                     \
  X(electric_guitar)                  \
  X(muted_guitar)                     \
  X(overdriven)                       \
  X(distortion)                       \
  X(guitar_harmonics)                 \
  X(bass_acoustic)                    \
  X(bass_fingered)                    \
  X(bass_picked)                      \
  X(bass_fretless)                    \
  X(bass_slap)                        \
  X(bass_pop)                         \
  X(harp)                             \
  X(sitar)                            \
  X(banjo)                            \
  X(shamisen)                         \
  X(koto)                             \
  X(kalimba)                          \
  X(drawbar_organ)                    \
  X(percussive_organ)                 \
  X(rock_organ)                       \
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
  X(lead_square)                      \
  X(lead_saw)                         \
  X(lead_calliope)                    \
  X(lead_chiff)                       \
  X(lead_charang)                     \
  X(lead_voice)                       \
  X(lead_fifths)                      \
  X(lead_bass_lead)                   \
  X(pad_new_age)                      \
  X(pad_warm)                         \
  X(pad_polysynth)                    \
  X(pad_choir)                        \
  X(pad_bowed)                        \
  X(pad_metallic)                     \
  X(pad_halo)                         \
  X(pad_sweep)                        \
  X(fx_rain)                          \
  X(fx_soundtrack)                    \
  X(fx_crystal)                       \
  X(fx_atmosphere)                    \
  X(fx_brightness)                    \
  X(fx_goblins)                       \
  X(fx_echoes)                        \
  X(fx_sci_fi)                        \
  X(woodblock)                        \
  X(taiko)                            \
  X(melodic_tom)                      \
  X(synth_drum)                       \
  X(reverse_cymbal)                   \
  X(sfx_guitar_fret)                  \
  X(sfx_breath)                       \
  X(sfx_seashore)                     \
  X(sfx_bird_tweet)                   \
  X(sfx_telephone_ring)               \
  X(sfx_helicopter)                   \
  X(sfx_applause)                     \
  X(sfx_gunshot)                      \
  X(violin)                           \
  X(viola)                            \
  X(cello)                            \
  X(contrabass)                       \
  X(fiddle)                           \
  X(string_ensemble_1)                \
  X(string_ensemble_2)                \
  X(trumpet)                          \
  X(trombone)                         \
  X(tuba)                             \
  X(muted_trumpet)                    \
  X(french_horn)                      \
  X(brass_section)                    \
  X(soprano_sax)                      \
  X(alto_sax)                         \
  X(tenor_sax)                        \
  X(baritone_sax)                     \
  X(oboe)                             \
  X(english_horn)                     \
  X(bassoon)                          \
  X(clarinet)                         \
  X(bag_pipe)                         \
  X(shanai)                           \
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
  X(bright_piano_wide)                \
  X(electric_grand_wide)              \
  X(honky_tonk_wide)                  \
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

#define SONARE_GM_OVERRIDE_OFFSET_ONE(name) offsetof(ProgramOverrides, name),
/// True when SONARE_GM_OVERRIDE_PATCHES lists the members in declaration order.
///
/// The count assertion above is blind to a reordering, and the list has a
/// positional consumer: the tuning-build program-key recorder pairs the i-th
/// listed name with the i-th patch of the contiguous `program_override_patches`
/// view. A swapped pair there would mislabel two patches in the knob catalogue,
/// and a fitter would write a value back into the wrong voice. Each member's
/// offset must therefore be its list position times the patch size — a
/// constant expression, so a reordering is a compile error and a shipped build
/// carries nothing.
constexpr bool program_override_list_in_declaration_order() noexcept {
  constexpr std::size_t kOffsets[] = {SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_OVERRIDE_OFFSET_ONE)};
  for (std::size_t i = 0; i < kProgramOverrideCount; ++i) {
    if (kOffsets[i] != i * sizeof(NativeSynthPatch)) return false;
  }
  return true;
}
#undef SONARE_GM_OVERRIDE_OFFSET_ONE
static_assert(program_override_list_in_declaration_order(),
              "SONARE_GM_OVERRIDE_PATCHES must list ProgramOverrides members in declaration order");

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
