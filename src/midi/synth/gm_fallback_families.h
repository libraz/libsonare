#pragma once

#include "midi/synth/gm_fallback_data.h"
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
/// (see kDamperVelSlope in piano_voice.cpp). Fitted here against the reference
/// with that scaling in place, so the two move together and this one alone is
/// not the whole damper.
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

/// One GM family (programs family*8 .. family*8+7) -> one subtractive patch.
/// Voiced for "honest sketch" quality (§E coverage tiers): leads/pads/basses
/// are strong, plucked/keys are decent decay-shaped patches, winds/strings
/// are sustained approximations. Later phases retarget families to their
/// dedicated engine modes by editing entries here only.
SONARE_TUNED_CONSTEXPR std::array<NativeSynthPatch, 16> build_family_patches() noexcept {
  std::array<NativeSynthPatch, 16> t{};

  // 0-7 piano: extended waveguide grand (stiff-string dispersion, felt
  // hammer, coupled unison strings, soundboard bank); the e-piano / clavi
  // programs override to FM.
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

  // 8-15 chromatic percussion: FM bell (inharmonic 3.5 ratio, long
  // key-rate-scaled decay). Every program in this family (8 celesta, 9
  // glockenspiel, 10 music box, 11 vibraphone, 12 marimba, 13 xylophone, 14
  // tubular bells, 15 dulcimer) now resolves to a dedicated modal/KS override,
  // so this entry is retained only to hold the family index (t[2] is the organ
  // family) — no program falls through to it.
  t[1].mode = SynthEngineMode::kFm;
  t[1].amp_env = fallback_env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.algorithm = FmAlgorithm::kStack2;
  t[1].fm.ops[0].ratio = 1.0f;
  t[1].fm.ops[0].level = 1.0f;
  t[1].fm.ops[0].env = fallback_env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.ops[0].key_rate_scale = 0.4f;
  t[1].fm.ops[1].ratio = 3.5f;  // inharmonic bell partials
  t[1].fm.ops[1].level = 3.0f;
  t[1].fm.ops[1].env = fallback_env(1.0f, 900.0f, 0.0f, 400.0f);
  t[1].fm.ops[1].vel_to_level = 0.6f;
  t[1].fm.ops[1].key_rate_scale = 0.5f;
  t[1].gain = 0.6f;

  // 16-23 organ: additive drawbar partials with key click (method (5));
  // the 88 8402 001 base registration, fast gate envelope.
  t[2].mode = SynthEngineMode::kAdditive;
  t[2].amp_env = fallback_env(2.0f, 0.0f, 1.0f, 60.0f);
  t[2].cutoff_hz = 20000.0f;
  t[2].additive.drawbars = {8.0f, 8.0f, 8.0f, 4.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f};
  t[2].additive.key_click = 0.4f;
  t[2].stereo_spread = 0.2f;
  t[2].gain = 0.7f;

  // 24-31 guitar: Karplus-Strong steel-string pluck (method (3)); the string
  // itself decays, so the amp envelope just gates note-off. Program-level
  // overrides voice the nylon/electric/muted/driven variants.
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
  // Dedicated plucked-string physics for the steel-string family default (kept
  // in step with the `steel` program-override base): coupled polarization,
  // physical pick, steel dispersion, tension bend, sympathetic halo.
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

  // 32-39 bass: single dark saw through the transistor ladder, punchy
  // filter envelope and a touch of drive.
  t[4].waveform = VaWaveform::kSaw;
  t[4].filter_model = SynthFilterModel::kMoogLadder;
  t[4].drive = 0.15f;
  t[4].amp_env = fallback_env(3.0f, 350.0f, 0.7f, 150.0f);
  t[4].cutoff_hz = 900.0f;
  t[4].filter_env = fallback_env(1.0f, 250.0f, 0.3f, 150.0f);
  t[4].env_to_cutoff_cents = 1500.0f;
  t[4].key_track = 0.3f;
  t[4].vel_to_cutoff_cents = 1200.0f;

  // 40-47 strings: slow detuned saws with drift.
  t[5].waveform = VaWaveform::kSaw;
  t[5].unison = 3;
  t[5].detune_cents = 10.0f;
  t[5].drift_cents = 3.0f;
  t[5].amp_env = fallback_env(120.0f, 300.0f, 0.85f, 350.0f);
  t[5].cutoff_hz = 4000.0f;
  t[5].key_track = 0.3f;
  t[5].lfo_to_pitch_cents = 5.0f;
  t[5].stereo_spread = 0.45f;

  // 48-55 ensemble / choir: wide slow supersaw pad with a gentle section
  // vibrato (a whole section never sits dead still).
  t[6].waveform = VaWaveform::kSaw;
  t[6].unison = 5;
  t[6].detune_cents = 14.0f;
  t[6].drift_cents = 4.0f;
  t[6].amp_env = fallback_env(200.0f, 400.0f, 0.8f, 500.0f);
  t[6].cutoff_hz = 3200.0f;
  t[6].lfo_rate_hz = 4.6f;
  t[6].lfo_to_pitch_cents = 4.0f;
  t[6].stereo_spread = 0.6f;

  // 56-63 brass: 3-op FM stack with a feedback operator (the DX brass
  // recipe), index swelling in through the modulator envelope.
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
  t[7].fm.ops[2].feedback = 2.4f;  // feedback op: saw-like brass spectrum
  t[7].fm.ops[2].env = fallback_env(80.0f, 400.0f, 0.6f, 200.0f);
  // Section, not soloist: players never sit at exactly one pitch or one seat.
  t[7].drift_cents = 3.0f;
  t[7].lfo_rate_hz = 5.0f;
  t[7].lfo_to_pitch_cents = 4.0f;
  t[7].stereo_spread = 0.4f;

  // 64-71 reed: hollow square, light vibrato.
  t[8].waveform = VaWaveform::kSquare;
  t[8].amp_env = fallback_env(30.0f, 150.0f, 0.85f, 180.0f);
  t[8].cutoff_hz = 2800.0f;
  t[8].key_track = 0.4f;
  t[8].lfo_to_pitch_cents = 4.0f;

  // 72-79 pipe / flute: near-pure triangle with vibrato.
  t[9].waveform = VaWaveform::kTriangle;
  t[9].amp_env = fallback_env(50.0f, 100.0f, 0.9f, 150.0f);
  t[9].cutoff_hz = 4500.0f;
  t[9].key_track = 0.5f;
  t[9].lfo_to_pitch_cents = 7.0f;

  // 80-87 synth lead: classic 3-osc detuned saw lead through the ladder.
  t[10].waveform = VaWaveform::kSaw;
  t[10].unison = 3;
  t[10].detune_cents = 12.0f;
  t[10].filter_model = SynthFilterModel::kMoogLadder;
  t[10].drive = 0.1f;
  t[10].amp_env = fallback_env(5.0f, 200.0f, 0.8f, 150.0f);
  t[10].cutoff_hz = 3500.0f;
  t[10].filter_env = fallback_env(1.0f, 350.0f, 0.4f, 150.0f);
  t[10].env_to_cutoff_cents = 1800.0f;
  t[10].vel_to_cutoff_cents = 1200.0f;

  // 88-95 synth pad: 7-osc supersaw, slow envelope, drift.
  t[11].waveform = VaWaveform::kSaw;
  t[11].unison = 7;
  t[11].detune_cents = 18.0f;
  t[11].drift_cents = 5.0f;
  t[11].amp_env = fallback_env(400.0f, 600.0f, 0.8f, 800.0f);
  t[11].cutoff_hz = 2800.0f;
  t[11].stereo_spread = 0.6f;

  // 96-103 synth FX: drifting detuned triangles.
  t[12].waveform = VaWaveform::kTriangle;
  t[12].unison = 3;
  t[12].detune_cents = 15.0f;
  t[12].drift_cents = 8.0f;
  t[12].amp_env = fallback_env(300.0f, 800.0f, 0.7f, 900.0f);
  t[12].cutoff_hz = 3000.0f;
  t[12].lfo_to_pitch_cents = 10.0f;

  // 104-111 ethnic (plucked): bright short KS pluck (banjo/sitar/koto
  // sketch — near-bridge pick, fast decay).
  t[13].mode = SynthEngineMode::kKarplusStrong;
  t[13].amp_env = fallback_env(1.0f, 0.0f, 1.0f, 200.0f);
  t[13].cutoff_hz = 20000.0f;
  t[13].ks.brightness = 0.75f;
  t[13].ks.decay_s = 1.6f;
  t[13].ks.decay_stretch = 0.4f;
  t[13].ks.pick_position = 0.09f;
  t[13].ks.exc_brightness = 0.95f;
  t[13].ks.vel_to_brightness = 0.6f;
  t[13].ks.release_damp_s = 0.05f;
  t[13].gain = 0.8f;

  // 112-119 percussive: short bright strike.
  t[14].waveform = VaWaveform::kTriangle;
  t[14].amp_env = fallback_env(1.0f, 280.0f, 0.0f, 200.0f);
  t[14].cutoff_hz = 4000.0f;
  t[14].filter_env = fallback_env(1.0f, 200.0f, 0.0f, 200.0f);
  t[14].env_to_cutoff_cents = 2000.0f;
  t[14].key_track = 0.5f;

  // 120-127 SFX: resonant band-passed noise wash.
  t[15].waveform = VaWaveform::kNoise;
  t[15].filter_output = SynthFilterOutput::kBandpass;
  t[15].amp_env = fallback_env(50.0f, 600.0f, 0.5f, 400.0f);
  t[15].cutoff_hz = 2000.0f;
  t[15].resonance_q = 2.0f;
  t[15].gain = 0.7f;

  for (NativeSynthPatch& p : t) p = clamp_synth_patch(p);

    // Development-only per-family voicing override, keyed `fam0`..`fam15` by
    // GM family (`SONARE_TUNING_OVERRIDES=fam0.piano.brightness=0.83`). A
    // family patch serves the eight programs of its family that no program
    // override displaces. See gm_fallback_programs.cpp for why this is
    // compiled out rather than gated.
#if defined(SONARE_TUNING) && SONARE_TUNING
  for (int i = 0; i < 16; ++i) {
    char key[8] = {'f', 'a', 'm', 0, 0, 0};
    if (i < 10) {
      key[3] = static_cast<char>('0' + i);
    } else {
      key[3] = '1';
      key[4] = static_cast<char>('0' + i - 10);
    }
    apply_patch_tuning(t[static_cast<size_t>(i)], key);
  }
#endif
  return t;
}

}  // namespace sonare::midi::synth::detail
