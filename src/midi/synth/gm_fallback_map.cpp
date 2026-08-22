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

}  // namespace

const NativeSynthPatch& gm_fallback_patch(uint16_t bank, uint8_t program) noexcept {
  // GS variation banks fall back to their capital tone's family (same rule as
  // resolve_gs_preset: the variation differs in character, not family). The
  // harpsichord is the exception: its GS/GM2 banks select genuine registrations
  // (octave mix / wide / with key off), so program 6 consults the bank.
  switch (program & 0x7Fu) {
    case 4:  // Electric Piano 1
    case 5:  // Electric Piano 2
      return program_overrides().e_piano;
    case 6: {  // Harpsichord (plucked string, KS physical) + registration banks
      const ProgramOverrides& o = program_overrides();
      switch (bank) {
        case 1:  // Harpsichord (octave mix) — 8'+4'
          return o.harpsichord_octave;
        case 2:  // Harpsichord (wide)
          return o.harpsichord_wide;
        case 3:  // Harpsichord (with key off)
          return o.harpsichord_keyoff;
        default:  // bank 0 or unknown variation: the plain 8'
          return o.harpsichord;
      }
    }
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
    case 26:  // Electric Guitar (jazz)
    case 27:  // Electric Guitar (clean)
      return program_overrides().electric_guitar;
    case 28:  // Electric Guitar (muted)
      return program_overrides().muted_guitar;
    case 29:  // Overdriven Guitar
      return program_overrides().overdriven;
    case 30:  // Distortion Guitar
      return program_overrides().distortion;
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
    // Brass family (physical lip reed). Brass Section (61) + SynthBrass (62-63)
    // stay on the FM family sketch.
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
    case 106:  // Shamisen
      return program_overrides().shamisen;
    case 107:  // Koto
      return program_overrides().koto;
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
      const NativeSynthPatch* got = &gm_fallback_patch(0, static_cast<uint8_t>(p));
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
      ::sonare::tuning::note_program_key(p, key);
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

uint8_t gm_fallback_drum_kit(uint8_t program) noexcept {
  const GsDrumKit* kit = gs_drum_kit_entry(program & 0x7Fu);
  return kit != nullptr ? kit->index : 0;  // unknown program -> Standard
}

float apply_gs_drum_kit(PercussionPatchParams& perc, DahdsrConfig& amp, uint8_t kit,
                        uint8_t note) noexcept {
  const bool kick = note == 35 || note == 36;
  const bool snare = note == 38 || note == 40;
  const bool tom = note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50;
  const bool cymbal = note == 49 || note == 51 || note == 52 || note == 53 || note == 55 ||
                      note == 57 || note == 59;
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
    case 8:   // SFX: a set of one-shots in real GS — leave the Standard voicing.
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
