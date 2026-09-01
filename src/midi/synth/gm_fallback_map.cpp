#include "midi/synth/gm_fallback_map.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gs_layer.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

using detail::drum_note_table;
using detail::family_patches;
using detail::program_overrides;
using detail::ProgramOverrides;

namespace {

/// Per-program ambience weighting (see `gm_fallback_sends`). Named rather than
/// written inline because this is the table that decides how much room each
/// instrument carries by default, and it is fitted against reference
/// recordings the same way a voice's calibration constants are — an instrument
/// whose reference is always a wet one (a cathedral organ, a concert harp) is
/// only faithful if its ambience is fitted alongside its timbre.
SONARE_TUNABLE(kSendsDrumsRev, 0.6f);
SONARE_TUNABLE(kSendsDrumsCho, 0.6f);
SONARE_TUNABLE(kSendsChurchOrganRev, 2.2f);
SONARE_TUNABLE(kSendsChurchOrganCho, 1.0f);
SONARE_TUNABLE(kSendsHarpRev, 1.6f);
SONARE_TUNABLE(kSendsHarpCho, 1.0f);
SONARE_TUNABLE(kSendsStringEnsembleRev, 1.6f);
SONARE_TUNABLE(kSendsStringEnsembleCho, 3.0f);
SONARE_TUNABLE(kSendsChoirRev, 1.8f);
SONARE_TUNABLE(kSendsChoirCho, 3.5f);
SONARE_TUNABLE(kSendsPianoRev, 1.0f);
SONARE_TUNABLE(kSendsPianoCho, 1.0f);
SONARE_TUNABLE(kSendsChromPercRev, 1.3f);
SONARE_TUNABLE(kSendsChromPercCho, 1.0f);
SONARE_TUNABLE(kSendsOrganRev, 1.1f);
SONARE_TUNABLE(kSendsOrganCho, 1.5f);
SONARE_TUNABLE(kSendsGuitarRev, 0.8f);
SONARE_TUNABLE(kSendsGuitarCho, 1.5f);
SONARE_TUNABLE(kSendsBassRev, 0.4f);
SONARE_TUNABLE(kSendsBassCho, 0.6f);
SONARE_TUNABLE(kSendsSoloStringRev, 1.2f);
SONARE_TUNABLE(kSendsSoloStringCho, 1.0f);
SONARE_TUNABLE(kSendsEnsembleRev, 1.6f);
SONARE_TUNABLE(kSendsEnsembleCho, 3.0f);
SONARE_TUNABLE(kSendsBrassRev, 1.0f);
SONARE_TUNABLE(kSendsBrassCho, 1.2f);
SONARE_TUNABLE(kSendsReedRev, 0.9f);
SONARE_TUNABLE(kSendsReedCho, 1.0f);
SONARE_TUNABLE(kSendsFluteRev, 1.1f);
SONARE_TUNABLE(kSendsFluteCho, 1.0f);
SONARE_TUNABLE(kSendsSynthLeadRev, 0.7f);
SONARE_TUNABLE(kSendsSynthLeadCho, 1.5f);
SONARE_TUNABLE(kSendsSynthPadRev, 1.7f);
SONARE_TUNABLE(kSendsSynthPadCho, 3.0f);
SONARE_TUNABLE(kSendsSynthFxRev, 1.5f);
SONARE_TUNABLE(kSendsSynthFxCho, 2.5f);
SONARE_TUNABLE(kSendsEthnicRev, 0.9f);
SONARE_TUNABLE(kSendsEthnicCho, 1.0f);
SONARE_TUNABLE(kSendsPercussiveRev, 1.2f);
SONARE_TUNABLE(kSendsPercussiveCho, 1.0f);
SONARE_TUNABLE(kSendsSfxRev, 1.4f);
SONARE_TUNABLE(kSendsSfxCho, 1.0f);

/// The default rigs (see `gm_fallback_rig`), tunable for the same reason a send
/// weight is: an electric guitar's reference is always heard through one. The
/// numbers run high because a preset is voiced for a full-scale input and the
/// bank's guitar arrives 12 dB under one; each trim peaks its program against
/// the same voice's direct signal. Programs 29 and 30 share an amplifier at
/// different gain: a brighter preset put 30's 5 kHz band 12.6 dB over its
/// reference against the crunch rig's 3.6, and no drive moved it.
SONARE_TUNABLE(kRigCleanDrive, 0.35f);
SONARE_TUNABLE(kRigCleanLevelDb, 44.1f);
SONARE_TUNABLE(kRigCrunchDrive, 0.55f);
SONARE_TUNABLE(kRigCrunchLevelDb, 24.9f);
SONARE_TUNABLE(kRigLeadDrive, 0.95f);
SONARE_TUNABLE(kRigLeadLevelDb, 21.2f);

/// A GS variation tone the model floor voices with a patch of its own: the
/// capital tone it hangs under, the bank number that selects it, and the patch.
///
/// `variation` is the bank gs_effective_bank resolved, which is one number space
/// carrying two numbering schemes. GS puts the variation number in Bank Select
/// MSB and numbers its variations 8 / 16 / 24 / …; GM2 puts it in Bank Select
/// LSB and renumbers the variations it adopted 1 / 2 / 3 / …. Both addresses for
/// the same tone are listed, so a file written for either standard sounds the
/// same voice. Where the two standards disagree about WHICH tone a number
/// selects, only the GS address is listed and the GM2 one falls back to the
/// capital — a plausible neighbour is worse than the tone the file asked for.
/// `since` is the earliest tone map defining the tone. A map older than that
/// does not reach it, so a file that asks for the SC-55 map hears the capital
/// tone for a variation the SC-55 map never had — the same fallback a module
/// gives it. Every tone below is an SC-55 tone, so the field only bites once
/// later maps' tones are voiced; it is carried now so the gate is exercised
/// rather than added after the fact.
struct GsVariationPatch {
  uint8_t program;
  uint8_t variation;
  GsToneMap since;
  NativeSynthPatch ProgramOverrides::*patch;
};

/// Deliberately a plain array with a deduced bound rather than a counted
/// std::array: a count that falls behind the entries zero-fills the tail, and a
/// zero-filled entry here is a null pointer-to-member that the lookup would
/// dereference. There is no count to fall behind.
constexpr GsVariationPatch kGsVariationPatches[] = {
    // Piano 1w / Piano 1d (GM2: wide / dark).
    {0, 8, GsToneMap::kSc55, &ProgramOverrides::piano_wide},
    {0, 1, GsToneMap::kSc55, &ProgramOverrides::piano_wide},
    {0, 16, GsToneMap::kSc55, &ProgramOverrides::piano_dark},
    {0, 2, GsToneMap::kSc55, &ProgramOverrides::piano_dark},
    // Piano 2w / Piano 3w / HonkyTonk w — one per capital, since the three
    // capitals are voiced apart. Sharing the grand's wide patch would have made
    // each of them duller, quieter or more in tune than its own capital.
    {1, 8, GsToneMap::kSc55, &ProgramOverrides::bright_piano_wide},
    {1, 1, GsToneMap::kSc55, &ProgramOverrides::bright_piano_wide},
    {2, 8, GsToneMap::kSc55, &ProgramOverrides::electric_grand_wide},
    {2, 1, GsToneMap::kSc55, &ProgramOverrides::electric_grand_wide},
    {3, 8, GsToneMap::kSc55, &ProgramOverrides::honky_tonk_wide},
    {3, 1, GsToneMap::kSc55, &ProgramOverrides::honky_tonk_wide},
    // Detuned EP 1/2, E.Piano 1v/2v, 60's E.Piano.
    {4, 8, GsToneMap::kSc55, &ProgramOverrides::e_piano_detuned_1},
    {4, 1, GsToneMap::kSc55, &ProgramOverrides::e_piano_detuned_1},
    {4, 16, GsToneMap::kSc55, &ProgramOverrides::e_piano_velocity_1},
    {4, 2, GsToneMap::kSc55, &ProgramOverrides::e_piano_velocity_1},
    {4, 24, GsToneMap::kSc55, &ProgramOverrides::e_piano_sixties},
    {4, 3, GsToneMap::kSc55, &ProgramOverrides::e_piano_sixties},
    {5, 8, GsToneMap::kSc55, &ProgramOverrides::e_piano_detuned_2},
    {5, 1, GsToneMap::kSc55, &ProgramOverrides::e_piano_detuned_2},
    {5, 16, GsToneMap::kSc55, &ProgramOverrides::e_piano_velocity_2},
    {5, 2, GsToneMap::kSc55, &ProgramOverrides::e_piano_velocity_2},
    // Coupled Hps. / Harpsi.w / Harpsi.o — the harpsichord's three registrations.
    {6, 8, GsToneMap::kSc55, &ProgramOverrides::harpsichord_octave},
    {6, 1, GsToneMap::kSc55, &ProgramOverrides::harpsichord_octave},
    {6, 16, GsToneMap::kSc55, &ProgramOverrides::harpsichord_wide},
    {6, 2, GsToneMap::kSc55, &ProgramOverrides::harpsichord_wide},
    {6, 24, GsToneMap::kSc55, &ProgramOverrides::harpsichord_keyoff},
    {6, 3, GsToneMap::kSc55, &ProgramOverrides::harpsichord_keyoff},
    // Vib.w / Marimba w.
    {11, 8, GsToneMap::kSc55, &ProgramOverrides::vibraphone_wide},
    {11, 1, GsToneMap::kSc55, &ProgramOverrides::vibraphone_wide},
    {12, 8, GsToneMap::kSc55, &ProgramOverrides::marimba_wide},
    {12, 1, GsToneMap::kSc55, &ProgramOverrides::marimba_wide},
    // Church Bell / Carillon — the two cast bells under the tubular bell.
    {14, 8, GsToneMap::kSc55, &ProgramOverrides::church_bell},
    {14, 1, GsToneMap::kSc55, &ProgramOverrides::church_bell},
    {14, 9, GsToneMap::kSc55, &ProgramOverrides::carillon},
    {14, 2, GsToneMap::kSc55, &ProgramOverrides::carillon},
    // Detuned Or.1 / 60's Organ 1 / Organ 4, and Organ 2's own two. GM2 adopted
    // only the first two of each, at its own numbers.
    {16, 8, GsToneMap::kSc55, &ProgramOverrides::organ_detuned_1},
    {16, 1, GsToneMap::kSc55, &ProgramOverrides::organ_detuned_1},
    {16, 16, GsToneMap::kSc55, &ProgramOverrides::organ_sixties},
    {16, 2, GsToneMap::kSc55, &ProgramOverrides::organ_sixties},
    {16, 32, GsToneMap::kSc55, &ProgramOverrides::organ_4},
    {16, 3, GsToneMap::kSc55, &ProgramOverrides::organ_4},
    {17, 8, GsToneMap::kSc55, &ProgramOverrides::organ_detuned_2},
    {17, 1, GsToneMap::kSc55, &ProgramOverrides::organ_detuned_2},
    {17, 32, GsToneMap::kSc55, &ProgramOverrides::organ_5},
    {17, 2, GsToneMap::kSc55, &ProgramOverrides::organ_5},
    // Church Org.2 / Church Org.3 — the flute and full-organ registrations.
    {19, 8, GsToneMap::kSc55, &ProgramOverrides::church_organ_flutes},
    {19, 1, GsToneMap::kSc55, &ProgramOverrides::church_organ_flutes},
    {19, 16, GsToneMap::kSc55, &ProgramOverrides::church_organ_full},
    {19, 2, GsToneMap::kSc55, &ProgramOverrides::church_organ_full},
    // Accordion It. GS only: GM2 gives its own bank LSB 1 to the FRENCH
    // accordion, which is the dry tuning the capital already voices, so
    // adopting the GM2 address here would make the two standards contradict.
    {21, 8, GsToneMap::kSc55, &ProgramOverrides::accordion_italian},
    // Ukulele / Nylon Gt.o.
    {24, 8, GsToneMap::kSc55, &ProgramOverrides::ukulele},
    {24, 1, GsToneMap::kSc55, &ProgramOverrides::ukulele},
    {24, 16, GsToneMap::kSc55, &ProgramOverrides::nylon_guitar_keyoff},
    {24, 2, GsToneMap::kSc55, &ProgramOverrides::nylon_guitar_keyoff},
    // 12-str.Gt / Mandolin.
    {25, 8, GsToneMap::kSc55, &ProgramOverrides::twelve_string_guitar},
    {25, 1, GsToneMap::kSc55, &ProgramOverrides::twelve_string_guitar},
    {25, 16, GsToneMap::kSc55, &ProgramOverrides::mandolin},
    {25, 2, GsToneMap::kSc55, &ProgramOverrides::mandolin},
    // Slow Violin.
    {40, 8, GsToneMap::kSc55, &ProgramOverrides::violin_slow},
    {40, 1, GsToneMap::kSc55, &ProgramOverrides::violin_slow},
};

/// The variation patch for a (bank, program), or nullptr when the bank selects
/// no variation this table voices. A miss is not an error: GS resolves a
/// variation a module does not have to the capital tone, so an unlisted bank
/// falling through to the capital is the specified behaviour, not a gap.
const NativeSynthPatch* gs_variation_patch(uint16_t bank, uint8_t program, GsToneMap map) noexcept {
  if (bank == 0 || bank > 0x7Fu) return nullptr;  // capital tone, or the drum bank
  const auto variation = static_cast<uint8_t>(bank);
  for (const GsVariationPatch& v : kGsVariationPatches) {
    if (v.program != program || v.variation != variation) continue;
    if (!gs_map_reaches(map, v.since)) return nullptr;  // older map -> capital tone
    return &(program_overrides().*(v.patch));
  }
  return nullptr;
}

}  // namespace

const NativeSynthPatch& gm_fallback_patch(uint16_t bank, uint8_t program, GsToneMap map) noexcept {
  // A GS variation tone is the same instrument voiced differently, so most
  // variation banks resolve to their capital tone's patch — the same rule
  // resolve_gs_preset applies to a SoundFont. The ones the model floor can
  // genuinely voice apart have their own patch in kGsVariationPatches.
  if (const NativeSynthPatch* variation =
          gs_variation_patch(bank, static_cast<uint8_t>(program & 0x7Fu), map)) {
    return *variation;
  }
  switch (program & 0x7Fu) {
    // Program 0 is deliberately absent: the concert grand stays on the family
    // patch, which is where the piano's fit lives and what these three derive
    // from.
    case 1:  // Bright Acoustic Piano (the same grand, voiced hard)
      return program_overrides().bright_piano;
    case 2:  // Electric Grand Piano (short strings, piezo pickup, no board)
      return program_overrides().electric_grand;
    case 3:  // Honky-tonk Piano (unisons pulled apart until they beat)
      return program_overrides().honky_tonk;
    case 4:  // Electric Piano 1
    case 5:  // Electric Piano 2
      return program_overrides().e_piano;
    case 6:  // Harpsichord (plucked string, KS physical) — the plain 8'
      return program_overrides().harpsichord;
    case 7:  // Clavi (struck string + pickup — FM stand-in for now)
      return program_overrides().clav;
    case 8:  // Celesta (soft mallet bar, modal)
      return program_overrides().celesta;
    case 9:  // Glockenspiel
      return program_overrides().glockenspiel;
    case 10:  // Music Box (metallic comb tine, modal)
      return program_overrides().music_box;
    case 11:  // Vibraphone
      return program_overrides().vibraphone;
    case 12:  // Marimba
      return program_overrides().marimba;
    case 13:  // Xylophone
      return program_overrides().xylophone;
    case 14:  // Tubular Bells (long-ringing bell, modal)
      return program_overrides().tubular_bells;
    case 15:  // Dulcimer (hammered string, KS)
      return program_overrides().dulcimer;
    case 16:  // Drawbar Organ (tonewheel, base registration)
      return program_overrides().drawbar_organ;
    case 17:  // Percussive Organ (the single-shot on the first key of a phrase)
      return program_overrides().percussive_organ;
    case 18:  // Rock Organ (fuller registration, harder click)
      return program_overrides().rock_organ;
    case 19:  // Church Organ (flue pipe)
      return program_overrides().church_organ;
    case 20:  // Reed Organ (harmonium free reed)
    case 21:  // Accordion (shares the reed-organ free-reed voicing)
      return program_overrides().reed_organ;
    case 22:  // Harmonica (small bright free reed)
      return program_overrides().harmonica;
    case 23:  // Bandoneon (musette-detuned free reed)
      return program_overrides().bandoneon;
    case 24:  // Acoustic Guitar (nylon)
      return program_overrides().nylon_guitar;
    case 25:  // Acoustic Guitar (steel)
      return program_overrides().steel_guitar;
    case 26:  // Electric Guitar (jazz)
    case 27:  // Electric Guitar (clean)
      return program_overrides().electric_guitar;
    case 28:  // Electric Guitar (muted)
      return program_overrides().muted_guitar;
    case 29:  // Overdriven Guitar
      return program_overrides().overdriven;
    case 30:  // Distortion Guitar
      return program_overrides().distortion;
    case 31:  // Guitar Harmonics (a touched node, not the open string)
      return program_overrides().guitar_harmonics;
    case 32:  // Acoustic Bass
      return program_overrides().bass_acoustic;
    case 33:  // Electric Bass (finger)
      return program_overrides().bass_fingered;
    case 34:  // Electric Bass (pick)
      return program_overrides().bass_picked;
    case 35:  // Fretless Bass
      return program_overrides().bass_fretless;
    case 36:  // Slap Bass 1
      return program_overrides().bass_slap;
    case 37:  // Slap Bass 2
      return program_overrides().bass_pop;
    case 44:  // Tremolo Strings
      return program_overrides().tremolo_strings;
    case 45:  // Pizzicato Strings
      return program_overrides().pizzicato;
    case 46:  // Orchestral Harp
      return program_overrides().harp;
    case 47:  // Timpani
      return program_overrides().timpani;
    case 52:  // Choir Aahs
      return program_overrides().choir_aahs;
    case 53:  // Voice Oohs
      return program_overrides().voice_oohs;
    case 54:  // Synth Voice
      return program_overrides().synth_voice;
    case 55:  // Orchestra Hit (bright detuned-saw stab)
      return program_overrides().orchestra_hit;
    // Bowed string family (physical waveguide).
    case 40:  // Violin
      return program_overrides().violin;
    case 41:  // Viola
      return program_overrides().viola;
    case 42:  // Cello
      return program_overrides().cello;
    case 43:  // Contrabass
      return program_overrides().contrabass;
    case 110:  // Fiddle (the violin family, bowed the folk way)
      return program_overrides().fiddle;
    case 48:  // String Ensemble 1
      return program_overrides().string_ensemble_1;
    case 49:  // String Ensemble 2 (slower, warmer)
      return program_overrides().string_ensemble_2;
    // Brass family (physical lip reed). SynthBrass (62-63) stays on the FM
    // family sketch.
    case 56:  // Trumpet
      return program_overrides().trumpet;
    case 57:  // Trombone
      return program_overrides().trombone;
    case 58:  // Tuba
      return program_overrides().tuba;
    case 59:  // Muted Trumpet
      return program_overrides().muted_trumpet;
    case 60:  // French Horn
      return program_overrides().french_horn;
    case 61:  // Brass Section (the same lip reed, in section)
      return program_overrides().brass_section;
    // Reed woodwind family (physical single-reed waveguide).
    case 64:  // Soprano Sax
      return program_overrides().soprano_sax;
    case 65:  // Alto Sax
      return program_overrides().alto_sax;
    case 66:  // Tenor Sax
      return program_overrides().tenor_sax;
    case 67:  // Baritone Sax
      return program_overrides().baritone_sax;
    case 68:  // Oboe
      return program_overrides().oboe;
    case 69:  // English Horn
      return program_overrides().english_horn;
    case 70:  // Bassoon
      return program_overrides().bassoon;
    case 71:  // Clarinet
      return program_overrides().clarinet;
    case 109:  // Bag pipe (chanter double reed, fixed bag pressure)
      return program_overrides().bag_pipe;
    case 111:  // Shanai (double reed into a metal bell)
      return program_overrides().shanai;
    // Air-jet flute family (physical edge-tone waveguide).
    case 72:  // Piccolo
      return program_overrides().piccolo;
    case 73:  // Flute
      return program_overrides().concert_flute;
    case 74:  // Recorder
      return program_overrides().recorder;
    case 75:  // Pan Flute
      return program_overrides().pan_flute;
    case 76:  // Blown Bottle
      return program_overrides().blown_bottle;
    case 77:  // Shakuhachi
      return program_overrides().shakuhachi;
    case 78:  // Whistle
      return program_overrides().tin_whistle;
    case 79:  // Ocarina
      return program_overrides().ocarina;
    // Buzzing-bridge plucked strings (physical waveguide).
    case 104:  // Sitar
      return program_overrides().sitar;
    case 105:  // Banjo (steel strings drained by a membrane head)
      return program_overrides().banjo;
    case 106:  // Shamisen
      return program_overrides().shamisen;
    case 107:  // Koto
      return program_overrides().koto;
    case 108:  // Kalimba (a plucked steel tine is a bar, not a string)
      return program_overrides().kalimba;
    // Synth lead and pad (subtractive, one voice each rather than two families).
    case 80:  // Lead 1 (square)
      return program_overrides().lead_square;
    case 81:  // Lead 2 (sawtooth)
      return program_overrides().lead_saw;
    case 82:  // Lead 3 (calliope)
      return program_overrides().lead_calliope;
    case 83:  // Lead 4 (chiff)
      return program_overrides().lead_chiff;
    case 84:  // Lead 5 (charang)
      return program_overrides().lead_charang;
    case 85:  // Lead 6 (voice)
      return program_overrides().lead_voice;
    case 86:  // Lead 7 (fifths)
      return program_overrides().lead_fifths;
    case 87:  // Lead 8 (bass + lead)
      return program_overrides().lead_bass_lead;
    case 88:  // Pad 1 (new age)
      return program_overrides().pad_new_age;
    case 89:  // Pad 2 (warm)
      return program_overrides().pad_warm;
    case 90:  // Pad 3 (polysynth)
      return program_overrides().pad_polysynth;
    case 91:  // Pad 4 (choir)
      return program_overrides().pad_choir;
    case 92:  // Pad 5 (bowed)
      return program_overrides().pad_bowed;
    case 93:  // Pad 6 (metallic)
      return program_overrides().pad_metallic;
    case 94:  // Pad 7 (halo)
      return program_overrides().pad_halo;
    case 95:  // Pad 8 (sweep)
      return program_overrides().pad_sweep;
    // Synth effects (subtractive, one named gesture per GM program).
    case 96:  // FX 1 (rain)
      return program_overrides().fx_rain;
    case 97:  // FX 2 (soundtrack)
      return program_overrides().fx_soundtrack;
    case 98:  // FX 3 (crystal)
      return program_overrides().fx_crystal;
    case 99:  // FX 4 (atmosphere)
      return program_overrides().fx_atmosphere;
    case 100:  // FX 5 (brightness)
      return program_overrides().fx_brightness;
    case 101:  // FX 6 (goblins)
      return program_overrides().fx_goblins;
    case 102:  // FX 7 (echoes)
      return program_overrides().fx_echoes;
    case 103:  // FX 8 (sci-fi)
      return program_overrides().fx_sci_fi;
    // Pitched percussion family (membrane / struck-idiophone cores, key-tracked).
    case 112:  // Tinkle Bell
      return program_overrides().tinkle_bell;
    case 113:  // Agogo
      return program_overrides().agogo;
    case 114:  // Steel Drums
      return program_overrides().steel_drums;
    case 115:  // Woodblock
      return program_overrides().woodblock;
    case 116:  // Taiko Drum
      return program_overrides().taiko;
    case 117:  // Melodic Tom
      return program_overrides().melodic_tom;
    case 118:  // Synth Drum
      return program_overrides().synth_drum;
    case 119:  // Reverse Cymbal
      return program_overrides().reverse_cymbal;
    // Sound effects (subtractive approximations of GM sample names).
    case 120:  // Guitar Fret Noise
      return program_overrides().sfx_guitar_fret;
    case 121:  // Breath Noise
      return program_overrides().sfx_breath;
    case 122:  // Seashore
      return program_overrides().sfx_seashore;
    case 123:  // Bird Tweet
      return program_overrides().sfx_bird_tweet;
    case 124:  // Telephone Ring
      return program_overrides().sfx_telephone_ring;
    case 125:  // Helicopter
      return program_overrides().sfx_helicopter;
    case 126:  // Applause
      return program_overrides().sfx_applause;
    case 127:  // Gunshot
      return program_overrides().sfx_gunshot;
    default:
      break;
  }
  return family_patches()[static_cast<size_t>((program & 0x7Fu) >> 3)];
}

bool is_dedicated_model_engine(SynthEngineMode mode) noexcept {
  // Exhaustive over SynthEngineMode with no default: adding a new engine
  // triggers a -Wswitch warning so it must be classified here explicitly.
  switch (mode) {
    case SynthEngineMode::kKarplusStrong:
    case SynthEngineMode::kModal:
    case SynthEngineMode::kPercussion:
    case SynthEngineMode::kPiano:
    case SynthEngineMode::kPipeOrgan:
    case SynthEngineMode::kBowedString:
    case SynthEngineMode::kReed:
    case SynthEngineMode::kBrass:
    case SynthEngineMode::kFlute:
    case SynthEngineMode::kPluckedString:
    case SynthEngineMode::kFreeReed:
    case SynthEngineMode::kHarpsichord:
      return true;
    case SynthEngineMode::kSubtractive:
    case SynthEngineMode::kFm:
    case SynthEngineMode::kAdditive:
    case SynthEngineMode::kVocal:
      return false;
  }
  return false;
}

bool gm_program_has_dedicated_model(uint16_t bank, uint8_t program) noexcept {
  return is_dedicated_model_engine(gm_fallback_patch(bank, program).mode);
}

const NativeSynthPatch& gm_fallback_drum_patch(uint8_t note) noexcept {
  return drum_note_table()[note & 0x7Fu];
}

#if defined(SONARE_TUNING) && SONARE_TUNING
namespace {

/// Which patch key voices each melodic program, resolved by comparing the
/// address `gm_fallback_patch` hands back against the two tables it draws
/// from. Recorded for the `SONARE_TUNING_DUMP` catalogue so a fitter can list
/// a program's knobs without re-implementing the switch above — that switch is
/// the one place the mapping exists, and a parse of it would go stale.
///
/// Deliberately not run from the table builders: they are what
/// `gm_fallback_patch` initialises, so calling it from inside one would
/// re-enter a static initialisation already in progress.
struct ProgramKeyRecorder {
  ProgramKeyRecorder() {
    static const char* const kNames[] = {
#define SONARE_GM_NAME_ONE(name) #name,
        SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_NAME_ONE)
#undef SONARE_GM_NAME_ONE
    };
    const NativeSynthPatch* base = detail::program_override_patches(program_overrides());
    const std::array<NativeSynthPatch, 16>& fams = family_patches();
    for (int p = 0; p < 128; ++p) {
      const NativeSynthPatch* const capital = &gm_fallback_patch(0, static_cast<uint8_t>(p));
      // Every GS variation bank, not only the capital tone: a variation is its
      // own patch with its own knobs, and a fitter handed the capital's knob
      // list cannot address the one the render actually selected. A bank whose
      // variation is undefined resolves back to the capital and is skipped, so
      // what the catalogue lists is exactly the set that can be addressed.
      for (int bank = 0; bank < 128; ++bank) {
        const NativeSynthPatch* got =
            &gm_fallback_patch(static_cast<uint16_t>(bank), static_cast<uint8_t>(p));
        if (bank != 0 && got == capital) continue;
        const char* key = nullptr;
        for (std::size_t i = 0; i < detail::kProgramOverrideCount; ++i) {
          if (got == base + i) {
            key = kNames[i];
            break;
          }
        }
        static char fam_key[8];
        if (key == nullptr) {
          for (std::size_t i = 0; i < fams.size(); ++i) {
            if (got == &fams[i]) {
              std::snprintf(fam_key, sizeof(fam_key), "fam%zu", i);
              key = fam_key;
              break;
            }
          }
        }
        ::sonare::tuning::note_program_key(p, bank, key);
      }
      // The rig belongs to the program rather than to the patch it resolves to:
      // three programs share the electric guitar's patch and are heard through
      // two different amplifiers, which is the whole point of the separation.
      ::sonare::tuning::note_rig(p, gm_fallback_rig(0, static_cast<uint8_t>(p)).preset);
    }
  }
};

}  // namespace
#endif

GmFallbackSends gm_fallback_sends(uint16_t bank, uint8_t program) noexcept {
#if defined(SONARE_TUNING) && SONARE_TUNING
  // Any render reaches here through refresh_channel_mod, and by then both
  // fallback tables are fully built, which is what the address comparison
  // needs. Runs once per process.
  static const ProgramKeyRecorder kRecorder;
  (void)kRecorder;
#endif
  if (bank == 128) return {kSendsDrumsRev, kSendsDrumsCho};  // tighter than the melodics
  switch (program & 0x7Fu) {
    case 19:  // Church Organ lives in a cathedral, not a booth
      return {kSendsChurchOrganRev, kSendsChurchOrganCho};
    case 46:  // Orchestral Harp: concert-hall halo
      return {kSendsHarpRev, kSendsHarpCho};
    case 48:  // String Ensembles: hall + section shimmer
    case 49:
    case 50:
    case 51:
      return {kSendsStringEnsembleRev, kSendsStringEnsembleCho};
    case 52:  // Choir Aahs / Voice Oohs / Synth Voice
    case 53:
    case 54:
      return {kSendsChoirRev, kSendsChoirCho};
    default:
      break;
  }
  switch ((program & 0x7Fu) >> 3) {
    case 0:
      return {kSendsPianoRev, kSendsPianoCho};  // pianos: lid-open room
    case 1:
      return {kSendsChromPercRev, kSendsChromPercCho};  // chromatic percussion rings in air
    case 2:
      return {kSendsOrganRev, kSendsOrganCho};  // organs
    case 3:
      return {kSendsGuitarRev, kSendsGuitarCho};  // guitars
    case 4:
      return {kSendsBassRev, kSendsBassCho};  // basses stay tight
    case 5:
      return {kSendsSoloStringRev, kSendsSoloStringCho};  // solo strings
    case 6:
      return {kSendsEnsembleRev, kSendsEnsembleCho};  // ensembles (non-override programs)
    case 7:
      return {kSendsBrassRev, kSendsBrassCho};  // brass
    case 8:
      return {kSendsReedRev, kSendsReedCho};  // reeds
    case 9:
      return {kSendsFluteRev, kSendsFluteCho};  // flutes
    case 10:
      return {kSendsSynthLeadRev, kSendsSynthLeadCho};  // synth leads
    case 11:
      return {kSendsSynthPadRev, kSendsSynthPadCho};  // synth pads bathe in the wash
    case 12:
      return {kSendsSynthFxRev, kSendsSynthFxCho};  // synth FX
    case 13:
      return {kSendsEthnicRev, kSendsEthnicCho};  // ethnic plucked
    case 14:
      return {kSendsPercussiveRev, kSendsPercussiveCho};  // percussive
    default:
      return {kSendsSfxRev, kSendsSfxCho};  // SFX
  }
}

GmFallbackRig gm_rig_binding(uint8_t id) noexcept {
  switch (id) {
    case 1:
      return {1, "cleanCombo", kRigCleanDrive, kRigCleanLevelDb};
    case 2:
      return {2, "classicCrunch", kRigCrunchDrive, kRigCrunchLevelDb};
    case 3:
      return {3, "classicCrunch", kRigLeadDrive, kRigLeadLevelDb};
    default:
      return {};
  }
}

GmFallbackRig gm_fallback_rig(uint16_t bank, uint8_t program) noexcept {
  if (bank == kDrumBank) return {};
  // The six GM electric guitars, and only those. What binds a rig is that the
  // instrument is never heard without one: a module's samples of these six have
  // an amplifier and a cabinet recorded into them, and the model's voice stops
  // at the pickup. Everything else that could take a rig is left unbound for
  // now — an electric piano and a drawbar organ each want a component that is
  // not an amplifier (a suitcase preamp, a rotary speaker), and a module's
  // electric bass is close enough to a direct signal that binding one would be
  // a preference rather than a repair.
  switch (program & 0x7Fu) {
    case 26:  // Jazz Guitar: hollow body into a clean American combo
    case 27:  // Clean Guitar
    case 28:  // Muted Guitar
    case 31:  // Guitar Harmonics: chimes, so the amp stays under breakup
      return gm_rig_binding(1);
    case 29:  // Overdriven Guitar: an amp just into its power stage
      return gm_rig_binding(2);
    case 30:  // Distortion Guitar: a cascaded preamp, scooped
      return gm_rig_binding(3);
    default:
      return {};
  }
}

uint8_t gm_fallback_drum_kit(uint8_t program, GsToneMap map) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program & 0x7Fu, map);
  return kit != nullptr ? kit->index : 0;  // no kit in this map -> Standard
}

float apply_gs_drum_kit(PercussionPatchParams& perc, DahdsrConfig& amp, uint8_t kit,
                        uint8_t note) noexcept {
  const bool kick = note == 35 || note == 36;
  const bool snare = note == 38 || note == 40;
  const bool tom = note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50;
  const bool cymbal = note == 49 || note == 51 || note == 52 || note == 53 || note == 55 ||
                      note == 57 || note == 59;
  const bool hat = note == 42 || note == 44 || note == 46;
  const bool membrane = kick || snare || tom;
  float gain = 1.0f;
  switch (kit) {
    case 1:  // Room: more shell body and a longer, ambient tail.
      perc.shell_mix = std::min(perc.shell_mix + 0.12f, 1.0f);
      for (float& t60 : perc.shell_t60_s) t60 *= 1.6f;
      perc.mode_decay_s *= 1.15f;
      perc.noise_decay_ms *= 1.25f;
      amp.decay_ms *= 1.25f;
      break;
    case 2:  // Power (Rock): bigger, lower, longer shells.
      if (membrane) {
        perc.base_freq_hz *= 0.86f;
        perc.mode_decay_s *= 1.4f;
        amp.decay_ms *= 1.4f;
        gain = 1.2f;
      }
      break;
    case 3:  // Electronic: sine-ify the membranes and dry them out.
      if (membrane) {
        if (perc.num_modes > 1) perc.num_modes = 1;
        perc.base_freq_hz *= 0.92f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.5f);
        perc.noise_gain *= 0.5f;
      }
      break;
    case 4:  // TR-808: the iconic decaying-sine recipes.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.82f;
        perc.pitch_drop = 1.0f;
        perc.pitch_drop_ms = 60.0f;
        perc.mode_decay_s *= 2.5f;
        amp.decay_ms *= 2.5f;
        perc.noise_gain *= 0.3f;
        gain = 1.2f;
      } else if (snare) {
        perc.num_modes = 1;  // single body tone under the noise
        perc.noise_gain *= 1.2f;
        perc.mode_decay_s *= 0.8f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.noise_gain *= 0.4f;
      } else if (cymbal) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.2f, 18000.0f);
      }
      break;
    case 5:  // Jazz: tighter, higher, softer.
      if (membrane) {
        perc.base_freq_hz *= 1.06f;
        perc.mode_decay_s *= 0.8f;
        amp.decay_ms *= 0.8f;
        gain = 0.9f;
      }
      break;
    case 6:  // Brush: the snare becomes a sustained swish, not a crack.
      if (snare) {
        perc.noise_decay_ms *= 2.5f;
        amp.decay_ms *= 2.0f;
        perc.noise_cutoff_hz *= 0.8f;
        perc.wire_buzz *= 0.4f;
        gain = 0.85f;
      } else if (membrane) {
        gain = 0.9f;
      }
      break;
    case 7:  // Orchestra: concert bass drum / timpani rings, longer cymbals.
      if (membrane) {
        perc.mode_decay_s *= 2.0f;
        amp.decay_ms *= 2.0f;
      } else if (cymbal) {
        perc.mode_decay_s *= 1.5f;
        amp.decay_ms *= 1.5f;
      }
      break;
    case 9:  // CM-64/32L: the LA-synth era kit — short, thin, bright samples.
      if (membrane) {
        if (perc.num_modes > 2) perc.num_modes = 2;
        perc.base_freq_hz *= 1.04f;
        perc.mode_decay_s *= 0.55f;
        amp.decay_ms *= 0.55f;
        perc.noise_gain *= 0.8f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.7f;
      }
      gain = 0.95f;
      break;
    case 10:  // Standard 2: the alternate standard — a drier, tighter room.
      if (membrane) {
        perc.base_freq_hz *= 0.97f;
        for (float& t60 : perc.shell_t60_s) t60 *= 0.9f;
        perc.mode_decay_s *= 0.92f;
      }
      if (snare) perc.wire_buzz *= 1.1f;
      break;
    case 11:  // Dance: the drum-machine kit of a club record — sine kick, clap-lit
              // snare, tight hats.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.85f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.8f);
        perc.mode_decay_s *= 1.8f;
        amp.decay_ms *= 1.8f;
        perc.noise_gain *= 0.35f;
        gain = 1.15f;
      } else if (snare) {
        perc.noise_gain *= 1.35f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.15f, 18000.0f);
        perc.mode_decay_s *= 0.7f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.5f);
      } else if (hat) {
        perc.noise_decay_ms *= 0.7f;
      }
      break;
    case 12:  // Ethnic: hand drums. Struck near the rim rather than the centre, so
              // the higher modes speak and the drum is pitched, over a thin shell
              // that rings far less than a kit drum's.
      if (membrane) {
        perc.strike_r = std::max(perc.strike_r, 0.6f);
        perc.base_freq_hz *= 1.1f;
        perc.mode_decay_s *= 0.7f;
        amp.decay_ms *= 0.7f;
        perc.noise_gain *= 0.5f;
        perc.shell_mix *= 0.5f;
      }
      gain = 0.95f;
      break;
    case 13:  // Kick & Snare: a bank of alternate kicks and snares over an
              // otherwise Standard kit, so only those two move.
      if (kick) {
        perc.base_freq_hz *= 0.9f;
        perc.mode_decay_s *= 1.25f;
        amp.decay_ms *= 1.25f;
        gain = 1.1f;
      } else if (snare) {
        perc.wire_buzz *= 1.25f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.1f, 18000.0f);
      }
      break;
    case 15:  // Standard 3: the set whose pieces vary strike to strike. The
              // variation itself needs a per-strike seed this hook is not given
              // (it runs once on the resolved patch, before the voice is seeded),
              // so what is voiced here is the rest of the set's character: struck
              // off-centre and left more open than Standard.
      if (membrane) {
        perc.strike_r = std::max(perc.strike_r, 0.45f);
        perc.shell_mix = std::min(perc.shell_mix + 0.08f, 1.0f);
      }
      break;
    case 16:  // Hip Hop: low, short and squashed — the sampled, heavily processed
              // kit rather than a room.
      if (kick) {
        perc.base_freq_hz *= 0.8f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.mode_decay_s *= 1.5f;
        amp.decay_ms *= 1.5f;
        perc.noise_gain *= 0.5f;
        gain = 1.25f;
      } else if (snare) {
        perc.noise_cutoff_hz *= 0.8f;
        perc.noise_decay_ms *= 0.8f;
        perc.wire_buzz *= 0.7f;
        gain = 1.1f;
      } else if (tom) {
        perc.base_freq_hz *= 0.85f;
      }
      break;
    case 17:  // Jungle: a breakbeat chopped short — everything cut off early and
              // pushed bright.
      if (membrane) {
        perc.base_freq_hz *= 1.05f;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
      }
      if (snare) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.25f, 18000.0f);
        perc.noise_decay_ms *= 0.6f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.5f;
      }
      break;
    case 18:  // Techno: Electronic taken further — pure synthetic membranes and a
              // hard, bright top end.
      if (membrane) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.6f);
        perc.noise_gain *= 0.35f;
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.3f, 18000.0f);
      }
      gain = 1.1f;
      break;
    case 19:  // CR-78: the 1978 preset-rhythm box. Barely any tone at all — short
              // filtered noise ticks over tiny sines, and a snare that is a noise
              // burst with no wire under it.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.95f;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
        perc.noise_gain *= 0.2f;
      } else if (snare) {
        perc.num_modes = 1;
        perc.noise_decay_ms *= 0.35f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.4f, 18000.0f);
        perc.wire_buzz = 0.0f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.mode_decay_s *= 0.5f;
        amp.decay_ms *= 0.5f;
      } else if (cymbal || hat) {
        perc.noise_decay_ms *= 0.3f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.5f, 18000.0f);
      }
      gain = 0.85f;
      break;
    case 20:  // TR-606: thin and tinny — the smallest of the analog boxes.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.9f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.7f);
        perc.mode_decay_s *= 1.1f;
        perc.noise_gain *= 0.25f;
      } else if (snare) {
        perc.num_modes = 1;
        perc.noise_gain *= 1.3f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.3f, 18000.0f);
        perc.noise_decay_ms *= 0.55f;
        perc.wire_buzz *= 0.4f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.95f;
        perc.mode_decay_s *= 0.7f;
        amp.decay_ms *= 0.7f;
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.45f, 18000.0f);
        perc.noise_decay_ms *= 0.6f;
      }
      gain = 0.9f;
      break;
    case 21:  // TR-707: sampled, not analog — crisp, dry and short, with none of
              // the analog boxes' decay tails and little room around it.
      if (membrane) {
        perc.mode_decay_s *= 0.65f;
        amp.decay_ms *= 0.65f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.15f, 18000.0f);
        perc.shell_mix *= 0.6f;
      }
      gain = 1.05f;
      break;
    case 22:  // TR-909: the long decaying-sine kick with a click on top, a snare
              // that is mostly noise, and bright metallic cymbals.
      if (kick) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.86f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.9f);
        perc.pitch_drop_ms = 45.0f;
        perc.mode_decay_s *= 2.2f;
        amp.decay_ms *= 2.2f;
        perc.noise_gain *= 0.5f;  // the attack click, not a skin
        gain = 1.2f;
      } else if (snare) {
        perc.noise_gain *= 1.45f;
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.2f, 18000.0f);
        perc.mode_decay_s *= 0.75f;
      } else if (tom) {
        perc.num_modes = 1;
        perc.base_freq_hz *= 0.92f;
        perc.pitch_drop = std::max(perc.pitch_drop, 0.55f);
      } else if (cymbal || hat) {
        perc.noise_cutoff_hz = std::min(perc.noise_cutoff_hz * 1.35f, 18000.0f);
        perc.mode_decay_s *= 1.2f;
      }
      break;
    case 23:  // Asia: gongs, taiko and temple blocks — big, low and long-ringing.
      if (membrane) {
        perc.base_freq_hz *= 0.82f;
        perc.mode_decay_s *= 2.2f;
        amp.decay_ms *= 2.2f;
        for (float& t60 : perc.shell_t60_s) t60 *= 1.5f;
      } else if (cymbal) {
        perc.mode_decay_s *= 2.5f;
        amp.decay_ms *= 2.5f;
        perc.noise_decay_ms *= 1.6f;
      }
      gain = 1.05f;
      break;
    case 8:   // SFX: a set of one-shots in real GS — leave the Standard voicing.
    case 14:  // Rhythm FX: likewise one-shots.
    case 24:  // Cymbal & Claps: a bank of metal and hands over an unchanged kit.
    case 25:  // Rhythm FX 2: likewise one-shots.
    default:  // Standard.
      break;
  }
  return gain;
}

float gm_fallback_max_release_ms() noexcept {
  static const float kMax = [] {
    float max_ms = 0.0f;
    for (const NativeSynthPatch& p : family_patches()) {
      // Zero-sustain (percussive/one-shot) patches ring through their decay
      // after note-off, so the decay bounds the tail too.
      max_ms = std::max(max_ms, std::max(p.amp_env.release_ms, p.amp_env.decay_ms));
    }
    // Per-note drum kit: some GS instruments (open triangle, belltree) ring far
    // longer than the base kit pieces, so bound the tail over the whole table.
    for (const NativeSynthPatch& p : drum_note_table()) {
      max_ms = std::max(max_ms, std::max(p.amp_env.release_ms, p.amp_env.decay_ms));
    }
    // Program overrides: sweep the whole table through its contiguous view so
    // a newly added override patch is bounded without touching this function.
    const NativeSynthPatch* overrides = detail::program_override_patches(program_overrides());
    for (std::size_t i = 0; i < detail::kProgramOverrideCount; ++i) {
      max_ms = std::max(max_ms,
                        std::max(overrides[i].amp_env.release_ms, overrides[i].amp_env.decay_ms));
    }
    return max_ms;
  }();
  return kMax;
}

}  // namespace sonare::midi::synth
