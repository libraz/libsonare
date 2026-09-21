#pragma once

#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/patch_sections.h"
#include "midi/synth/patch_tuning.h"
#include "util/tunable.h"

namespace sonare::midi::synth::detail {

/// Grand-piano family patch voicing. Named rather than written inline because
/// these are the knobs the voicematch fitter sweeps against a reference
/// rendering; every other family's values stay inline until one needs fitting.
SONARE_TUNABLE(kPianoBrightness, 0.81459f);
/// Unison spread. Read together with kUnisonRadSpread in piano_voice.cpp: the
/// two set how deeply the fundamental beats, and a deep beat on a treble note
/// is the one thing a tuned piano never does. Measured over a sustained C5
/// against three concert grands, whose fundamentals wobble 5.2 to 11.6 dB, the
/// pair had this voice at 21.1.
SONARE_TUNABLE(kPianoDetuneCents, 1.0f);
SONARE_TUNABLE(kPianoDecayFastS, 1.35f);
/// Aftersound t60 at A4 and how it stretches into the bass, overriding the
/// PianoPatchParams defaults for this family only. The struct's own values
/// describe a piano in general; these are what a concert grand measured
/// against a captured reference wants, and the difference between the two is
/// not something a default should be carrying for every caller.
///
/// Both are measured where the reference is still well clear of its own floor,
/// which for this corpus is not a detail: the samples carry a recorded rumble
/// (see the capture definition's `_floor`), and a decay fitted over a whole
/// eight-second gate flattens onto it and reports a slower fall than the
/// instrument has. Windows below are chosen to end while the reference is
/// thirty decibels or more above it.
///
/// The stretch is a RATIO and not a rate, because the string model already
/// keytracks on its own and this term multiplies whatever that does. Bass
/// decay time against midrange, from the broadband envelope over 0.5-3.0 s:
/// the three instruments give 1.51x, this voice gives 1.58x with the term at
/// zero and 3.51x at the 0.6 it used to carry. There is a real trend and the
/// string model already has it; the extra term was doubling it.
///
/// The slow t60 is the late fall, fitted over 3-7 s on the bottom two octaves
/// alone -- the only rows whose reference is still above -57 dBFS that late.
/// The instruments fall 2.32 dB/s there. This voice fell 7.03 at the 9.6 it
/// used to carry, and falls 2.82 here, against 1.5 dB/s of disagreement between
/// the three instruments. Thirty lands closer still on those rows and is not
/// taken: read across the whole keyboard the same voice then falls 0.89 dB/s
/// slower than the reference on average, where this value holds the signed
/// error to 0.57 and puts the dimension inside the spread the three instruments
/// themselves span.
///
/// Neither figure covers the top of the keyboard, where this voice is still
/// 1.6 to 2.6 times too fast above F#5 and stops entirely by two seconds at
/// C7 while the reference is still sounding at five.
SONARE_TUNABLE(kPianoDecaySlowS, 26.0f);
SONARE_TUNABLE(kPianoDecayStretch, 0.0f);
SONARE_TUNABLE(kPianoSoundboard, 0.35f);
/// Felt contact time at A4 and mezzo-forte. It buys two things and costs two,
/// and no value inside the literature's 1-4 ms band lands all four, so this is
/// the point where the four cross rather than an optimum for any of them.
///
/// Longer takes the h6-h12 partials back inside what a felt hammer can produce
/// at C4 -- they had been 6 dB harder than the instrument's, which is the band
/// a plectrum lives in -- and puts the tenor's sustained fundamental back
/// toward the reference. Shorter keeps the 2-8 kHz sustain tonal and keeps the
/// bass unison beat from shallowing. Measured across the value: C4's hardness
/// needs 1.35 or more, the mid and treble tonality needs 1.35 or less, and the
/// tenor's fundamental share does not arrive by 1.8 and wants a mechanism this
/// knob is not.
SONARE_TUNABLE(kPianoHammerContactMs, 1.35f);
SONARE_TUNABLE(kPianoHammerDynamics, 0.5f);
/// Damper t60 at note-off, at the loud end of the velocity range; the voice
/// lengthens it for a softer blow, because felt damps a quiet string weakly
/// (see kDamperVelSlope in piano_voice.cpp). Fitted against the concert grand's
/// own reference with that scaling in place, so the two move together and this
/// one alone is not the whole damper; the three voiced variants scale it down
/// from here, because the recordings they are aimed at damp far harder.
///
/// It also sets the far end of the half-pedal interpolation, which is what
/// stops it growing without limit: the pedal grades between the free string and
/// this, and a gentle enough full damp leaves nothing to grade against.
SONARE_TUNABLE(kPianoReleaseDampS, 1.0f);
/// Amp-envelope release (ms) for the piano family. This is the ceiling on how
/// long ANY released note may ring, so it bounds the damper ring-down as well
/// as the treble's, and the bound is close enough to bind: dropping it to 1500
/// shortens the measured release by 148 ms with every other knob held. It is
/// what carries the top of the keyboard, whose light dampers barely load the
/// string, so the two are fitted as a pair rather than independently.
SONARE_TUNABLE(kPianoAmpReleaseMs, 2500.0f);

/// The patches that are not reached through a program number: family 0, which
/// is where the concert grand's fit lives and which program 0 resolves to, plus
/// three bases that program builders copy and re-voice (steel-string pluck,
/// synth bass, FM brass).
SONARE_TUNED_CONSTEXPR std::array<NativeSynthPatch, 16> build_family_patches() noexcept {
  std::array<NativeSynthPatch, 16> t{};

  // Grand piano (GM family 0): extended waveguide (stiff-string dispersion,
  // felt hammer, coupled unison strings, soundboard bank). Program 0 resolves
  // here, and the three other acoustic pianos start from it.
  t[0].mode = SynthEngineMode::kPiano;
  t[0].amp_env = fallback_env(6.0f, 0.0f, 1.0f, kPianoAmpReleaseMs);
  t[0].cutoff_hz = 20000.0f;
  // A harder, shorter hammer contact keeps the upper partials a concert grand
  // actually has; the longer damp lets the damper fall be heard as a ring-down
  // rather than a gate. Velocity felt compression widens the pp<->ff spread so
  // soft strikes stay mellow and hard strikes brighten the way felt does. The
  // amp release sets the treble ring-down (the top strings, whose light dampers
  // barely load the string, are amp-release limited rather than damper limited).
  t[0].piano.brightness = kPianoBrightness;
  t[0].piano.detune_cents = kPianoDetuneCents;
  t[0].piano.decay_fast_s = kPianoDecayFastS;
  t[0].piano.decay_slow_s = kPianoDecaySlowS;
  t[0].piano.decay_stretch = kPianoDecayStretch;
  t[0].piano.soundboard = kPianoSoundboard;
  t[0].piano.hammer_contact_ms = kPianoHammerContactMs;
  t[0].piano.hammer_dynamics = kPianoHammerDynamics;
  t[0].piano.release_damp_s = kPianoReleaseDampS;
  t[0].stereo_spread = 0.3f;
  // Levelled against a captured concert grand, which is also where the
  // harpsichord's 0.30 comes from -- the two voices in this bank whose output
  // level answers to a measurement rather than to whatever the physics
  // happened to produce. A physical model has no output level of its own: the
  // string, the hammer and the board are each calibrated against something, and
  // the product of the three is a number nobody chose. At 0.8 this one struck
  // 8.5 dB above the harpsichord, 5.7 to 10.0 dB above three captured grands
  // across the phrase takes, and 11.4 dB above the median peak of the bank's
  // own twelve programs at one note and one velocity -- three measurements that
  // do not share a method and agree on the sign and nearly on the size. At 0.30
  // it sits between the violin and the alto sax, which is where a grand belongs
  // among them.
  t[0].gain = 0.3f;

  // Steel-string pluck base (GM family 3): Karplus-Strong, the string itself
  // decaying so the amp envelope only gates note-off. Read by
  // `twelve_string_guitar` and `mandolin`, which start from it and re-voice the
  // course pairs; no program resolves to it.
  t[3].mode = SynthEngineMode::kKarplusStrong;
  t[3].amp_env = fallback_env(1.0f, 0.0f, 1.0f, 250.0f);
  t[3].cutoff_hz = 20000.0f;
  t[3].ks.brightness = 0.62f;
  t[3].ks.decay_s = 3.5f;
  t[3].ks.decay_stretch = 0.6f;
  t[3].ks.pick_position = 0.18f;
  t[3].ks.exc_brightness = 0.85f;
  t[3].ks.vel_to_brightness = 0.6f;
  t[3].ks.release_damp_s = 0.08f;
  // Dedicated plucked-string physics (kept in step with the `steel`
  // program-override base): coupled polarization, physical pick, steel
  // dispersion, tension bend, sympathetic halo.
  t[3].ks.polarization = 0.3f;
  t[3].ks.body_coupling = 0.35f;
  t[3].ks.pluck_style = 0.5f;
  t[3].ks.nail = 0.62f;
  t[3].ks.tension_mod = 0.35f;
  t[3].ks.dispersion = 0.65f;
  t[3].ks.sympathetic = true;
  t[3].body = BodyType::kGuitar;
  t[3].body_mix = 0.35f;
  t[3].gain = 1.5f;

  // Bass base (GM family 4): single dark saw through the transistor ladder,
  // punchy filter envelope and a touch of drive. Read by the two synth basses,
  // which start from it; no program resolves to it.
  t[4].waveform = VaWaveform::kSaw;
  t[4].filter_model = SynthFilterModel::kMoogLadder;
  t[4].drive = 0.15f;
  t[4].amp_env = fallback_env(3.0f, 350.0f, 0.7f, 150.0f);
  t[4].cutoff_hz = 900.0f;
  t[4].filter_env = fallback_env(1.0f, 250.0f, 0.3f, 150.0f);
  t[4].env_to_cutoff_cents = 1500.0f;
  t[4].key_track = 0.3f;
  t[4].vel_to_cutoff_cents = 1200.0f;

  // Brass base (GM family 7): 3-op FM stack with a feedback operator (the DX
  // brass recipe), index swelling in through the modulator envelope. Read by
  // the two synth brasses, which start from it; no program resolves to it.
  t[7].mode = SynthEngineMode::kFm;
  t[7].amp_env = fallback_env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.algorithm = FmAlgorithm::kStack3;
  t[7].fm.ops[0].ratio = 1.0f;
  t[7].fm.ops[0].level = 1.0f;
  t[7].fm.ops[0].env = fallback_env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.ops[1].ratio = 1.0f;
  t[7].fm.ops[1].level = 3.2f;
  t[7].fm.ops[1].env = fallback_env(80.0f, 300.0f, 0.7f, 200.0f);  // brightness swell
  t[7].fm.ops[1].vel_to_level = 0.5f;
  t[7].fm.ops[2].ratio = 1.0f;
  t[7].fm.ops[2].level = 2.0f;
  // Feedback op: saw-like brass spectrum, and 2.4 is well past where that stops
  // being what it does. Swept alone against the GM 62 reference the value costs
  // 15 dB of tone-to-noise by 1.0 and only 1.3 dB more by 2.4, while the
  // centroid goes from 26% over the reference to 760% over it.
  t[7].fm.ops[2].feedback = 2.4f;
  t[7].fm.ops[2].env = fallback_env(80.0f, 400.0f, 0.6f, 200.0f);
  // Section, not soloist: players never sit at exactly one pitch or one seat.
  t[7].drift_cents = 3.0f;
  t[7].lfo_rate_hz = 5.0f;
  t[7].lfo_to_pitch_cents = 4.0f;
  t[7].stereo_spread = 0.4f;

  for (size_t i : kLiveBases) t[i] = clamp_synth_patch(t[i]);

    // Development-only voicing override, keyed `famN` by GM family
    // (`SONARE_TUNING_OVERRIDES=fam0.piano.brightness=0.83`). Only the populated
    // entries are offered: a key for an entry nothing reads would report a knob
    // no render can consult. See gm_fallback_programs.cpp for why this is
    // compiled out rather than gated.
#if defined(SONARE_TUNING) && SONARE_TUNING
  for (size_t i : kLiveBases) {
    const char key[5] = {'f', 'a', 'm', static_cast<char>('0' + i), 0};
    apply_patch_tuning(t[i], key);
  }
#endif
  return t;
}

/// The family table as it is stored. Separate from `build_family_patches()`
/// because the program builders start from that one and go on to set a section
/// of their own; only what is kept blanks its unvoiced sections.
SONARE_TUNED_CONSTEXPR std::array<NativeSynthPatch, 16> build_family_table() noexcept {
  std::array<NativeSynthPatch, 16> t = build_family_patches();
  for (size_t i : kLiveBases) t[i] = strip_unvoiced_sections(t[i]);
  return t;
}

}  // namespace sonare::midi::synth::detail
