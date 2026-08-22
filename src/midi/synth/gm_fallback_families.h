#pragma once

#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/patch_tuning.h"
#include "util/tunable.h"

namespace sonare::midi::synth::detail {

/// Grand-piano family patch voicing. Named rather than written inline because
/// these are the knobs the voicematch fitter sweeps against a reference
/// rendering; every other family's values stay inline until one needs fitting.
SONARE_TUNABLE(kPianoBrightness, 0.81459f);
SONARE_TUNABLE(kPianoDetuneCents, 1.0f);
SONARE_TUNABLE(kPianoDecayFastS, 1.35f);
SONARE_TUNABLE(kPianoSoundboard, 0.35f);
SONARE_TUNABLE(kPianoHammerContactMs, 1.1f);
SONARE_TUNABLE(kPianoHammerDynamics, 0.5f);
/// Damper t60 at note-off. Held well short of what the measured release alone
/// asks for, because this value is also the far end of the half-pedal
/// interpolation: a gentler full damp gives the pedal less to grade against,
/// and past about 1.2 s a half pedal stops being distinguishable from a lifted
/// one. Closing the remaining gap needs the two endpoints separated, not a
/// larger number here.
SONARE_TUNABLE(kPianoReleaseDampS, 1.0f);
/// Amp-envelope release (ms) for the piano family. This is the ceiling on how
/// long ANY released note may ring, so it bounds the damper ring-down as well
/// as the treble's -- raising release_damp_s alone moves the measured release
/// barely at all while this sits under it, which is why the two are fitted as
/// a pair. Longer than the reference strictly wants: past here the measured
/// release keeps improving, but only against a reference whose own numbers are
/// running into the capture gate, and every millisecond costs clarity on
/// repeated notes.
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
  t[0].piano.soundboard = kPianoSoundboard;
  t[0].piano.hammer_contact_ms = kPianoHammerContactMs;
  t[0].piano.hammer_dynamics = kPianoHammerDynamics;
  t[0].piano.release_damp_s = kPianoReleaseDampS;
  t[0].stereo_spread = 0.3f;
  t[0].gain = 0.8f;

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
  // the 88 8000 000 base registration, fast gate envelope.
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
