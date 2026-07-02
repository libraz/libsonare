#include "midi/synth/gm_fallback_map.h"

#include <algorithm>
#include <array>

namespace sonare::midi::synth {

namespace {

DahdsrConfig env(float attack_ms, float decay_ms, float sustain, float release_ms) noexcept {
  DahdsrConfig e;
  e.attack_ms = attack_ms;
  e.decay_ms = decay_ms;
  e.sustain = sustain;
  e.release_ms = release_ms;
  return e;
}

/// One GM family (programs family*8 .. family*8+7) -> one subtractive patch.
/// Voiced for "honest sketch" quality (§E coverage tiers): leads/pads/basses
/// are strong, plucked/keys are decent decay-shaped patches, winds/strings
/// are sustained approximations. Later phases retarget families to their
/// dedicated engine modes by editing entries here only.
std::array<NativeSynthPatch, 16> build_family_patches() noexcept {
  std::array<NativeSynthPatch, 16> t{};

  // 0-7 piano: extended waveguide grand (stiff-string dispersion, felt
  // hammer, coupled unison strings, soundboard bank); the e-piano / clavi
  // programs override to FM.
  t[0].mode = SynthEngineMode::kPiano;
  t[0].amp_env = env(0.5f, 0.0f, 1.0f, 800.0f);
  t[0].cutoff_hz = 20000.0f;
  // A harder, shorter hammer contact keeps the upper partials a concert grand
  // actually has; the longer damp lets the damper fall be heard as a ring-down
  // rather than a gate. Velocity felt compression widens the pp<->ff spread so
  // soft strikes stay mellow and hard strikes brighten the way felt does. The
  // amp release sets the treble ring-down (the top strings, whose light dampers
  // barely load the string, are amp-release limited rather than damper limited).
  t[0].piano.brightness = 0.95f;
  t[0].piano.hammer_contact_ms = 0.8f;
  t[0].piano.hammer_dynamics = 0.5f;
  t[0].piano.release_damp_s = 0.85f;
  t[0].stereo_spread = 0.3f;
  t[0].gain = 0.8f;

  // 8-15 chromatic percussion: FM bell (inharmonic 3.5 ratio, long
  // key-rate-scaled decay).
  t[1].mode = SynthEngineMode::kFm;
  t[1].amp_env = env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.algorithm = FmAlgorithm::kStack2;
  t[1].fm.ops[0].ratio = 1.0f;
  t[1].fm.ops[0].level = 1.0f;
  t[1].fm.ops[0].env = env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.ops[0].key_rate_scale = 0.4f;
  t[1].fm.ops[1].ratio = 3.5f;  // inharmonic bell partials
  t[1].fm.ops[1].level = 3.0f;
  t[1].fm.ops[1].env = env(1.0f, 900.0f, 0.0f, 400.0f);
  t[1].fm.ops[1].vel_to_level = 0.6f;
  t[1].fm.ops[1].key_rate_scale = 0.5f;
  t[1].gain = 0.6f;

  // 16-23 organ: additive drawbar partials with key click (method (5));
  // the 88 8000 000 base registration, fast gate envelope.
  t[2].mode = SynthEngineMode::kAdditive;
  t[2].amp_env = env(2.0f, 0.0f, 1.0f, 60.0f);
  t[2].cutoff_hz = 20000.0f;
  t[2].additive.drawbars = {8.0f, 8.0f, 8.0f, 4.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f};
  t[2].additive.key_click = 0.4f;
  t[2].stereo_spread = 0.2f;
  t[2].gain = 0.7f;

  // 24-31 guitar: Karplus-Strong steel-string pluck (method (3)); the string
  // itself decays, so the amp envelope just gates note-off. Program-level
  // overrides voice the nylon/electric/muted/driven variants.
  t[3].mode = SynthEngineMode::kKarplusStrong;
  t[3].amp_env = env(1.0f, 0.0f, 1.0f, 250.0f);
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
  t[4].amp_env = env(3.0f, 350.0f, 0.7f, 150.0f);
  t[4].cutoff_hz = 900.0f;
  t[4].filter_env = env(1.0f, 250.0f, 0.3f, 150.0f);
  t[4].env_to_cutoff_cents = 1500.0f;
  t[4].key_track = 0.3f;
  t[4].vel_to_cutoff_cents = 1200.0f;

  // 40-47 strings: slow detuned saws with drift.
  t[5].waveform = VaWaveform::kSaw;
  t[5].unison = 3;
  t[5].detune_cents = 10.0f;
  t[5].drift_cents = 3.0f;
  t[5].amp_env = env(120.0f, 300.0f, 0.85f, 350.0f);
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
  t[6].amp_env = env(200.0f, 400.0f, 0.8f, 500.0f);
  t[6].cutoff_hz = 3200.0f;
  t[6].lfo_rate_hz = 4.6f;
  t[6].lfo_to_pitch_cents = 4.0f;
  t[6].stereo_spread = 0.6f;

  // 56-63 brass: 3-op FM stack with a feedback operator (the DX brass
  // recipe), index swelling in through the modulator envelope.
  t[7].mode = SynthEngineMode::kFm;
  t[7].amp_env = env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.algorithm = FmAlgorithm::kStack3;
  t[7].fm.ops[0].ratio = 1.0f;
  t[7].fm.ops[0].level = 1.0f;
  t[7].fm.ops[0].env = env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.ops[1].ratio = 1.0f;
  t[7].fm.ops[1].level = 3.2f;
  t[7].fm.ops[1].env = env(80.0f, 300.0f, 0.7f, 200.0f);  // brightness swell
  t[7].fm.ops[1].vel_to_level = 0.5f;
  t[7].fm.ops[2].ratio = 1.0f;
  t[7].fm.ops[2].level = 2.0f;
  t[7].fm.ops[2].feedback = 2.4f;  // feedback op: saw-like brass spectrum
  t[7].fm.ops[2].env = env(80.0f, 400.0f, 0.6f, 200.0f);
  // Section, not soloist: players never sit at exactly one pitch or one seat.
  t[7].drift_cents = 3.0f;
  t[7].lfo_rate_hz = 5.0f;
  t[7].lfo_to_pitch_cents = 4.0f;
  t[7].stereo_spread = 0.4f;

  // 64-71 reed: hollow square, light vibrato.
  t[8].waveform = VaWaveform::kSquare;
  t[8].amp_env = env(30.0f, 150.0f, 0.85f, 180.0f);
  t[8].cutoff_hz = 2800.0f;
  t[8].key_track = 0.4f;
  t[8].lfo_to_pitch_cents = 4.0f;

  // 72-79 pipe / flute: near-pure triangle with vibrato.
  t[9].waveform = VaWaveform::kTriangle;
  t[9].amp_env = env(50.0f, 100.0f, 0.9f, 150.0f);
  t[9].cutoff_hz = 4500.0f;
  t[9].key_track = 0.5f;
  t[9].lfo_to_pitch_cents = 7.0f;

  // 80-87 synth lead: classic 3-osc detuned saw lead through the ladder.
  t[10].waveform = VaWaveform::kSaw;
  t[10].unison = 3;
  t[10].detune_cents = 12.0f;
  t[10].filter_model = SynthFilterModel::kMoogLadder;
  t[10].drive = 0.1f;
  t[10].amp_env = env(5.0f, 200.0f, 0.8f, 150.0f);
  t[10].cutoff_hz = 3500.0f;
  t[10].filter_env = env(1.0f, 350.0f, 0.4f, 150.0f);
  t[10].env_to_cutoff_cents = 1800.0f;
  t[10].vel_to_cutoff_cents = 1200.0f;

  // 88-95 synth pad: 7-osc supersaw, slow envelope, drift.
  t[11].waveform = VaWaveform::kSaw;
  t[11].unison = 7;
  t[11].detune_cents = 18.0f;
  t[11].drift_cents = 5.0f;
  t[11].amp_env = env(400.0f, 600.0f, 0.8f, 800.0f);
  t[11].cutoff_hz = 2800.0f;
  t[11].stereo_spread = 0.6f;

  // 96-103 synth FX: drifting detuned triangles.
  t[12].waveform = VaWaveform::kTriangle;
  t[12].unison = 3;
  t[12].detune_cents = 15.0f;
  t[12].drift_cents = 8.0f;
  t[12].amp_env = env(300.0f, 800.0f, 0.7f, 900.0f);
  t[12].cutoff_hz = 3000.0f;
  t[12].lfo_to_pitch_cents = 10.0f;

  // 104-111 ethnic (plucked): bright short KS pluck (banjo/sitar/koto
  // sketch — near-bridge pick, fast decay).
  t[13].mode = SynthEngineMode::kKarplusStrong;
  t[13].amp_env = env(1.0f, 0.0f, 1.0f, 200.0f);
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
  t[14].amp_env = env(1.0f, 280.0f, 0.0f, 200.0f);
  t[14].cutoff_hz = 4000.0f;
  t[14].filter_env = env(1.0f, 200.0f, 0.0f, 200.0f);
  t[14].env_to_cutoff_cents = 2000.0f;
  t[14].key_track = 0.5f;

  // 120-127 SFX: resonant band-passed noise wash.
  t[15].waveform = VaWaveform::kNoise;
  t[15].filter_output = SynthFilterOutput::kBandpass;
  t[15].amp_env = env(50.0f, 600.0f, 0.5f, 400.0f);
  t[15].cutoff_hz = 2000.0f;
  t[15].resonance_q = 2.0f;
  t[15].gain = 0.7f;

  for (NativeSynthPatch& p : t) p = clamp_synth_patch(p);
  return t;
}

/// Program-level overrides inside a family: the electric pianos and the
/// clavinet are FM instruments (method (2)), and the guitar family, orchestral
/// harp, and harpsichord voice their Karplus-Strong variants (method (3)).
struct ProgramOverrides {
  NativeSynthPatch e_piano;             // programs 4-5 (Electric Piano 1/2)
  NativeSynthPatch harpsichord;         // program 6 bank 0 (Harpsichord 8', KS)
  NativeSynthPatch harpsichord_octave;  // program 6 bank 1 (octave mix, 8'+4')
  NativeSynthPatch harpsichord_wide;    // program 6 bank 2 (wide)
  NativeSynthPatch harpsichord_keyoff;  // program 6 bank 3 (with key off)
  NativeSynthPatch clav;                // program 7 (Clavi, FM)
  NativeSynthPatch glockenspiel;        // program 9 (uniform-bar modal)
  NativeSynthPatch vibraphone;          // program 11 (tuned-bar modal, long)
  NativeSynthPatch marimba;             // program 12 (tuned-bar modal, woody)
  NativeSynthPatch xylophone;           // program 13 (quint-tuned modal, dry)
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
  NativeSynthPatch church_organ;        // program 19 (Church Organ, flue pipe)
  NativeSynthPatch reed_organ;          // programs 20-21 (Reed Organ / Accordion, free reed)
  NativeSynthPatch tremolo_strings;     // program 44 (measured-bow amp tremolo)
  NativeSynthPatch pizzicato;           // program 45 (Pizzicato Strings, KS + corpus)
  NativeSynthPatch timpani;             // program 47 (kettledrum membrane)
  NativeSynthPatch choir_aahs;          // program 52 (open-vowel vocal body)
  NativeSynthPatch voice_oohs;          // program 53 (darker closed vowel)
  NativeSynthPatch synth_voice;         // program 54 (brighter synthetic vowel)

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

ProgramOverrides build_program_overrides() noexcept {
  ProgramOverrides o{};

  // FM e-piano (Rhodes/Wurli sketch): body pair at 1:1 with a velocity-driven
  // index plus a fast-decaying 14:1 "tine" pair — the exponential index
  // fall-off is what reads as an electric piano.
  NativeSynthPatch& ep = o.e_piano;
  ep.mode = SynthEngineMode::kFm;
  ep.amp_env = env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.algorithm = FmAlgorithm::kPair2x2;
  ep.fm.ops[0].ratio = 1.0f;  // body carrier
  ep.fm.ops[0].level = 1.0f;
  ep.fm.ops[0].env = env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.ops[0].key_rate_scale = 0.4f;
  ep.fm.ops[1].ratio = 1.0f;  // body modulator (warmth -> velocity)
  ep.fm.ops[1].level = 0.9f;
  ep.fm.ops[1].env = env(1.0f, 1200.0f, 0.0f, 250.0f);
  ep.fm.ops[1].vel_to_level = 0.7f;
  ep.fm.ops[1].key_rate_scale = 0.5f;
  ep.fm.ops[2].ratio = 1.0f;  // tine carrier (quiet sparkle)
  ep.fm.ops[2].level = 0.3f;
  ep.fm.ops[2].env = env(1.0f, 600.0f, 0.0f, 150.0f);
  ep.fm.ops[2].key_rate_scale = 0.5f;
  ep.fm.ops[3].ratio = 14.0f;  // tine "ping"
  ep.fm.ops[3].level = 1.2f;
  ep.fm.ops[3].env = env(1.0f, 120.0f, 0.0f, 80.0f);
  ep.fm.ops[3].vel_to_level = 0.8f;
  ep.fm.ops[3].key_rate_scale = 0.6f;
  ep.gain = 0.6f;

  // FM clavi / harpsichord: bright high-ratio pluck with a fast index decay.
  NativeSynthPatch& cl = o.clav;
  cl.mode = SynthEngineMode::kFm;
  cl.amp_env = env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.algorithm = FmAlgorithm::kStack2;
  cl.fm.ops[0].ratio = 1.0f;
  cl.fm.ops[0].level = 1.0f;
  cl.fm.ops[0].env = env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.ops[0].key_rate_scale = 0.4f;
  cl.fm.ops[1].ratio = 7.0f;
  cl.fm.ops[1].level = 2.0f;
  cl.fm.ops[1].env = env(1.0f, 150.0f, 0.0f, 100.0f);
  cl.fm.ops[1].vel_to_level = 0.6f;
  cl.fm.ops[1].key_rate_scale = 0.5f;
  cl.gain = 0.6f;

  // Modal mallets: the realism is the mode-ratio data — uniform bar
  // (glockenspiel) 1:2.756:5.404:8.933 vs deep-arch tuned bar (marimba /
  // vibraphone) 1:4:10. All ring as one-shot-ish bars gated by note-off.
  NativeSynthPatch bar{};
  bar.mode = SynthEngineMode::kModal;
  bar.amp_env = env(0.5f, 0.0f, 1.0f, 350.0f);
  bar.cutoff_hz = 20000.0f;
  bar.gain = 0.7f;

  NativeSynthPatch& gl = o.glockenspiel;
  gl = bar;
  gl.modal.num_modes = 4;
  gl.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  gl.modal.modes[1] = {2.756f, 0.7f, 0.6f};
  gl.modal.modes[2] = {5.404f, 0.45f, 0.4f};
  gl.modal.modes[3] = {8.933f, 0.25f, 0.3f};
  gl.modal.decay_s = 3.5f;
  gl.modal.decay_stretch = 0.3f;
  gl.modal.strike_brightness = 0.85f;
  gl.amp_env.release_ms = 600.0f;

  NativeSynthPatch& vb = o.vibraphone;
  vb = bar;
  vb.modal.num_modes = 3;
  vb.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  vb.modal.modes[1] = {4.0f, 0.5f, 0.5f};
  vb.modal.modes[2] = {10.0f, 0.25f, 0.3f};
  vb.modal.decay_s = 5.0f;
  vb.modal.decay_stretch = 0.4f;
  vb.modal.strike_brightness = 0.75f;
  vb.amp_env.release_ms = 700.0f;
  // The motor-driven rotating vanes: the defining vibraphone tremolo.
  vb.lfo2_rate_hz = 4.5f;
  vb.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.35f};

  NativeSynthPatch& mr = o.marimba;
  mr = bar;
  mr.modal.num_modes = 3;
  mr.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  mr.modal.modes[1] = {4.0f, 0.6f, 0.35f};
  mr.modal.modes[2] = {10.0f, 0.35f, 0.2f};
  mr.modal.decay_s = 0.45f;
  mr.modal.decay_stretch = 0.6f;
  mr.modal.strike_brightness = 0.7f;
  mr.amp_env.release_ms = 250.0f;
  mr.body = BodyType::kWoodTube;
  mr.body_mix = 0.4f;

  NativeSynthPatch& xy = o.xylophone;
  xy = bar;
  xy.modal.num_modes = 3;
  xy.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  xy.modal.modes[1] = {3.0f, 0.65f, 0.4f};
  xy.modal.modes[2] = {6.0f, 0.4f, 0.25f};
  xy.modal.decay_s = 0.3f;
  xy.modal.decay_stretch = 0.5f;
  xy.modal.strike_brightness = 0.9f;
  xy.amp_env.release_ms = 200.0f;
  xy.body = BodyType::kWoodTube;
  xy.body_mix = 0.3f;

  // KS guitar variants: all share the family-3 steel string and differ in
  // pick position / loop brightness / decay (the Jaffe-Smith knobs).
  NativeSynthPatch steel{};
  steel.mode = SynthEngineMode::kKarplusStrong;
  steel.amp_env = env(1.0f, 0.0f, 1.0f, 250.0f);
  steel.cutoff_hz = 20000.0f;
  steel.ks.brightness = 0.62f;
  steel.ks.decay_s = 3.5f;
  steel.ks.decay_stretch = 0.6f;
  steel.ks.pick_position = 0.18f;
  steel.ks.exc_brightness = 0.85f;
  steel.ks.vel_to_brightness = 0.6f;
  steel.ks.release_damp_s = 0.08f;
  // Dedicated plucked-string physics: a coupled second polarization (bridge
  // double-decay), a physical pick pluck, steel-string dispersion, a tension
  // attack bend, and the sympathetic open-string halo (the last only rings when
  // the patch drives a standalone NativeSynth — the per-note GM fallback path
  // has no shared bank, and the per-voice gates carry the tone there).
  steel.ks.polarization = 0.3f;
  steel.ks.body_coupling = 0.35f;
  steel.ks.pluck_style = 0.5f;
  steel.ks.nail = 0.62f;  // steel pick / fingernail: bright edge
  steel.ks.tension_mod = 0.35f;
  steel.ks.dispersion = 0.65f;  // steel wound-string inharmonicity
  steel.ks.sympathetic = true;
  steel.body = BodyType::kGuitar;
  steel.body_mix = 0.35f;
  steel.gain = 1.5f;

  // Nylon: soft finger pluck near the middle of the string, dull loop. Keeps the
  // sympathetic halo (classical guitars sing with open-string resonance) but
  // drops dispersion (nylon plain strings are not audibly inharmonic) and softens
  // the pluck to the flesh of a fingertip with a lighter tension bend.
  o.nylon_guitar = steel;
  o.nylon_guitar.ks.brightness = 0.72f;
  o.nylon_guitar.ks.exc_brightness = 0.75f;
  o.nylon_guitar.ks.pick_position = 0.27f;
  o.nylon_guitar.ks.decay_s = 3.0f;
  o.nylon_guitar.ks.dispersion = 0.0f;
  o.nylon_guitar.ks.nail = 0.28f;  // fingertip flesh, rounder
  o.nylon_guitar.ks.tension_mod = 0.2f;
  o.nylon_guitar.body_mix = 0.3f;

  // Electric (jazz/clean) — the `electric-guitar` preset: bright sustaining
  // loop, near-bridge pick, a pickup-ish lowpass instead of the open string.
  o.electric_guitar = steel;
  o.electric_guitar.ks.brightness = 0.8f;
  o.electric_guitar.ks.decay_s = 4.5f;
  o.electric_guitar.ks.pick_position = 0.12f;
  o.electric_guitar.ks.exc_brightness = 0.9f;
  o.electric_guitar.cutoff_hz = 7000.0f;
  o.electric_guitar.body = BodyType::kNone;
  o.electric_guitar.body_mix = 0.0f;
  // Solid body: no sympathetic halo and a lighter plane coupling; the magnetic
  // pickup (position comb + field-gradient nonlinearity) near the bridge is the
  // electric character instead, with only a trace of steel dispersion.
  o.electric_guitar.ks.sympathetic = false;
  o.electric_guitar.ks.polarization = 0.15f;
  o.electric_guitar.ks.body_coupling = 0.2f;
  o.electric_guitar.ks.dispersion = 0.3f;
  o.electric_guitar.ks.pickup_pos = 0.14f;  // near the bridge, bright
  o.electric_guitar.ks.nail = 0.7f;         // pick
  o.electric_guitar.gain = 1.3f;

  // Palm mute: same electric string, choked decay.
  o.muted_guitar = o.electric_guitar;
  o.muted_guitar.ks.decay_s = 0.35f;
  o.muted_guitar.ks.brightness = 0.55f;
  o.muted_guitar.ks.release_damp_s = 0.04f;

  // Overdriven / distortion: electric string into the gain-compensated tanh
  // drive (the voice-level stage; the track-level `saturation.ampSim` insert
  // supplies the full amp/cab character).
  o.overdriven = o.electric_guitar;
  o.overdriven.drive = 0.45f;
  o.overdriven.cutoff_hz = 4000.0f;
  o.distortion = o.electric_guitar;
  o.distortion.drive = 0.8f;
  o.distortion.cutoff_hz = 3500.0f;
  o.distortion.gain = 1.0f;

  // Electric / acoustic bass (GM 32-35): the KS string voiced for the low
  // register — long, strongly stretched decays (bass strings ring far longer
  // than treble) and a pickup-ish lowpass on the electric members. Fingerstyle
  // plucks nearer the neck (rounder), the pick nearer the bridge (brighter
  // attack); the acoustic upright keeps a large resonating body. Slap/pop
  // (36-37) needs the dedicated bass excitation core and stays on the
  // subtractive family until it lands; synth bass (38-39) is subtractive by
  // design.
  NativeSynthPatch bass{};
  bass.mode = SynthEngineMode::kKarplusStrong;
  bass.amp_env = env(1.0f, 0.0f, 1.0f, 200.0f);
  bass.cutoff_hz = 3500.0f;
  bass.ks.brightness = 0.44f;
  bass.ks.decay_s = 5.5f;
  bass.ks.decay_stretch = 0.75f;
  bass.ks.pick_position = 0.26f;
  bass.ks.exc_brightness = 0.5f;
  bass.ks.vel_to_brightness = 0.6f;
  bass.ks.release_damp_s = 0.08f;
  // Two-polarization beat: a detuned horizontal plane adds the thickness and
  // slow shimmer of a big low string (the sustained members carry it; the
  // percussive slap/pop keep it off).
  bass.ks.polarization = 0.2f;
  bass.body = BodyType::kNone;
  bass.body_mix = 0.0f;
  bass.gain = 1.2f;
  o.bass_fingered = bass;

  // Acoustic upright: darker, softer pluck, a large resonating body.
  o.bass_acoustic = bass;
  o.bass_acoustic.ks.brightness = 0.34f;
  o.bass_acoustic.ks.decay_s = 6.0f;
  o.bass_acoustic.ks.decay_stretch = 0.8f;
  o.bass_acoustic.ks.pick_position = 0.22f;
  o.bass_acoustic.ks.exc_brightness = 0.55f;
  o.bass_acoustic.ks.release_damp_s = 0.1f;
  o.bass_acoustic.cutoff_hz = 4000.0f;
  o.bass_acoustic.body = BodyType::kGuitar;
  o.bass_acoustic.body_mix = 0.45f;
  o.bass_acoustic.gain = 1.2f;

  // Pick: near-bridge, bright attack, shorter ring.
  o.bass_picked = bass;
  o.bass_picked.ks.brightness = 0.58f;
  o.bass_picked.ks.decay_s = 4.5f;
  o.bass_picked.ks.decay_stretch = 0.7f;
  o.bass_picked.ks.pick_position = 0.11f;
  o.bass_picked.ks.exc_brightness = 0.85f;
  o.bass_picked.ks.release_damp_s = 0.06f;
  o.bass_picked.cutoff_hz = 5000.0f;
  o.bass_picked.gain = 1.2f;

  // Fretless: rounder, darker, longer glide-friendly ring.
  o.bass_fretless = bass;
  o.bass_fretless.ks.brightness = 0.4f;
  o.bass_fretless.ks.decay_s = 6.0f;
  o.bass_fretless.ks.decay_stretch = 0.78f;
  o.bass_fretless.ks.pick_position = 0.28f;
  o.bass_fretless.ks.exc_brightness = 0.55f;
  o.bass_fretless.ks.release_damp_s = 0.12f;
  o.bass_fretless.cutoff_hz = 4200.0f;
  o.bass_fretless.gain = 1.2f;

  // Slap Bass 1 (GM 36, thumb): the hard near-bridge attack of the pick voicing
  // driven into the fret-slap limiter — the string knocks the frets, so the
  // over-travel is reflected and buzzes (Rank & Kubin 1997).
  o.bass_slap = o.bass_picked;
  o.bass_slap.ks.brightness = 0.62f;
  o.bass_slap.ks.pick_position = 0.09f;
  o.bass_slap.ks.exc_brightness = 0.92f;
  o.bass_slap.ks.decay_s = 3.8f;
  o.bass_slap.ks.slap = 0.7f;
  o.bass_slap.ks.polarization = 0.0f;  // percussive: the beat would muddy the pop
  o.bass_slap.cutoff_hz = 6000.0f;

  // Slap Bass 2 (GM 37, pull/pop): a sharper, brighter pop with a harder fret
  // slap and a shorter ring.
  o.bass_pop = o.bass_slap;
  o.bass_pop.ks.brightness = 0.68f;
  o.bass_pop.ks.exc_brightness = 0.98f;
  o.bass_pop.ks.decay_s = 3.2f;
  o.bass_pop.ks.slap = 0.85f;
  o.bass_pop.cutoff_hz = 6500.0f;

  // Orchestral harp: long stretched decay, strings keep ringing after
  // note-off (no damper grip), mid-string pluck.
  o.harp = steel;
  o.harp.amp_env = env(1.0f, 0.0f, 1.0f, 1200.0f);
  o.harp.ks.brightness = 0.5f;
  o.harp.ks.decay_s = 5.0f;
  o.harp.ks.decay_stretch = 0.8f;
  o.harp.ks.pick_position = 0.3f;
  o.harp.ks.vel_to_brightness = 0.5f;
  o.harp.ks.release_damp_s = 1.0f;
  // A harp's many open strings ring in sympathy (keep the halo), but the strings
  // are plucked with the flesh of the finger and are not stiff or tension-bent.
  o.harp.ks.nail = 0.2f;
  o.harp.ks.dispersion = 0.0f;
  o.harp.ks.tension_mod = 0.0f;
  o.harp.body_mix = 0.3f;  // large open soundboard, less boxy than the guitar

  // Harpsichord (GM 6): a plucked string like the guitars, but a keyboard
  // instrument voiced by a hard quill/Delrin plectrum near the nut. The defining
  // trait is near velocity-insensitivity (only 3-6 dB across the range), so the
  // velocity->brightness coupling is nearly disabled. The pluck is a sharp
  // deterministic doublet (not a noisy strum), the near-nut pick position combs
  // in bright nasal upper harmonics, and the thin brass/iron strings are barely
  // inharmonic (no steel dispersion, no tension bend). The 8' unison choir beats
  // via the second polarization; the undamped 4'-top / behind-bridge strings ring
  // as the sympathetic halo (inherited from steel; sings in the standalone path).
  o.harpsichord = steel;
  o.harpsichord.amp_env.release_ms = 180.0f;
  o.harpsichord.ks.brightness = 0.72f;
  o.harpsichord.ks.decay_s = 2.4f;  // thin light strings ring shorter than a guitar
  o.harpsichord.ks.decay_stretch = 0.55f;
  o.harpsichord.ks.pick_position = 0.12f;      // near-nut pluck, high combed harmonics
  o.harpsichord.ks.exc_brightness = 0.92f;     // hard plectrum, sharp attack
  o.harpsichord.ks.vel_to_brightness = 0.12f;  // near velocity-insensitive
  o.harpsichord.ks.release_damp_s = 0.05f;     // fast felt damper on note-off
  o.harpsichord.ks.polarization = 0.28f;       // 8' unison beat
  o.harpsichord.ks.body_coupling = 0.3f;
  o.harpsichord.ks.pluck_style = 0.7f;  // deterministic quill doublet
  o.harpsichord.ks.nail = 0.85f;        // hard sharp plectrum edge
  o.harpsichord.ks.dispersion = 0.1f;   // low inharmonicity (thin brass/iron)
  o.harpsichord.ks.tension_mod = 0.0f;  // constant plucking force, no bend
  o.harpsichord.body_mix = 0.3f;

  // GS/GM2 harpsichord registration variations (program 6, bank select). Names
  // follow the GM2 melodic variation table (program_map.cpp): bank 1 octave mix,
  // bank 2 wide, bank 3 with key off. Each derives from the bank-0 8' voice.
  // Bank 1 — octave mix: engage the 4' companion string (the coupled 8'+4'
  // register), the brightest, fullest harpsichord colour.
  o.harpsichord_octave = o.harpsichord;
  o.harpsichord_octave.ks.octave_mix = 0.6f;
  // Bank 2 — wide: two 8' choirs spread across the stereo field with a touch
  // more unison beat, no 4'.
  o.harpsichord_wide = o.harpsichord;
  o.harpsichord_wide.stereo_spread = 0.5f;
  o.harpsichord_wide.ks.polarization = 0.4f;
  // Bank 3 — with key off: the jack-drop / felt-damper thump on note release.
  o.harpsichord_keyoff = o.harpsichord;
  o.harpsichord_keyoff.ks.keyoff_noise = 0.5f;

  // Church organ: a principal chorus of self-oscillating jet flue pipes. Each
  // rank locks its pitch and holds a solid, endless tone while keyed (no decay,
  // no breath-noise wander); the amp envelope just gates the wind on and off.
  o.church_organ.mode = SynthEngineMode::kPipeOrgan;
  o.church_organ.amp_env = env(120.0f, 0.0f, 1.0f, 380.0f);
  o.church_organ.cutoff_hz = 20000.0f;
  o.church_organ.pipe_organ.tone_decay_s = 8.0f;
  o.church_organ.pipe_organ.breath = 0.26f;
  o.church_organ.pipe_organ.chiff = 0.38f;
  o.church_organ.pipe_organ.release_damp_s = 0.75f;
  // GM Church Organ: a principal chorus (plenum) — 16' stopped sub for gravity
  // under an 8'+4'+2-2/3'+2' open principal chorus, the upperwork brighter.
  // The upperwork (smaller pipes) radiates more brightly into the room than the
  // wide bass ranks: radiation rises rank by rank, the 16' bourdon staying dark.
  o.church_organ.pipe_organ.rank_count = 6;
  o.church_organ.pipe_organ.ranks[0] = {0.5f, /*stopped=*/true, 0.4f, 0.16f, 0.0f, 0.0f};  // 16'
  o.church_organ.pipe_organ.ranks[1] = {1.0f, false, 0.74f, 1.0f, 0.0f, 0.2f};    // 8' principal
  o.church_organ.pipe_organ.ranks[2] = {2.0f, false, 0.76f, 1.01f, 0.0f, 0.3f};   // 4' octave
  o.church_organ.pipe_organ.ranks[3] = {3.0f, false, 0.7f, 0.31f, 0.0f, 0.4f};    // 2-2/3' quint
  o.church_organ.pipe_organ.ranks[4] = {4.0f, false, 0.72f, 0.43f, 0.0f, 0.45f};  // 2' super-octave
  o.church_organ.pipe_organ.ranks[5] = {5.0f, false, 0.6f, 0.04f, 0.0f, 0.45f};   // 1-3/5' tierce
  // Treble regulation: thin the upperwork (4'/quint/2'/tierce) toward the treble
  // so the plenum does not turn shrill above C4, while the bass and mid compass
  // keep the full chorus.
  o.church_organ.pipe_organ.keytrack = 0.5f;
  // A touch of wind sag so a full chord breathes, and a gentle tremulant — the
  // slow pressure undulation that keeps a sustained chord alive rather than
  // frozen (heard as faint sidebands around every partial).
  o.church_organ.pipe_organ.wind_sag = 0.25f;
  o.church_organ.pipe_organ.tremulant_rate_hz = 4.8f;
  o.church_organ.pipe_organ.tremulant_depth = 0.18f;
  o.church_organ.stereo_spread = 0.2f;
  o.church_organ.gain = 0.45f;

  // Reed Organ (GM 20) + Accordion (GM 21): a lingual reed stop — the jet is
  // driven hard and asymmetrically so the pipe buzzes with a bright, brassy
  // spectrum (harmonium / regal colour). An 8' reed under a 4' reed octave, both
  // open and very bright. Stands in for the free-reed group until a dedicated
  // model.
  o.reed_organ.mode = SynthEngineMode::kPipeOrgan;
  o.reed_organ.amp_env = env(14.0f, 0.0f, 1.0f, 110.0f);
  o.reed_organ.cutoff_hz = 20000.0f;
  o.reed_organ.pipe_organ.tone_decay_s = 6.0f;
  o.reed_organ.pipe_organ.breath = 0.35f;
  o.reed_organ.pipe_organ.chiff = 0.3f;
  o.reed_organ.pipe_organ.rank_count = 2;
  o.reed_organ.pipe_organ.ranks[0] = {1.0f, /*stopped=*/false, 0.8f, 1.0f, 0.85f, 0.25f};  // 8'
  o.reed_organ.pipe_organ.ranks[1] = {2.0f, false, 0.82f, 0.55f, 0.7f, 0.3f};              // 4'
  o.reed_organ.pipe_organ.wind_sag = 0.2f;
  o.reed_organ.stereo_spread = 0.18f;
  o.reed_organ.gain = 0.42f;

  // Tremolo Strings (GM 44): the string-section pad under a measured-bow
  // amplitude tremolo (LFO2 -> amp) — the section shudders rather than
  // re-attacking per stroke.
  NativeSynthPatch& trem = o.tremolo_strings;
  trem.waveform = VaWaveform::kSaw;
  trem.unison = 4;
  trem.detune_cents = 12.0f;
  trem.drift_cents = 4.0f;
  trem.amp_env = env(60.0f, 300.0f, 0.85f, 300.0f);
  trem.cutoff_hz = 3800.0f;
  trem.key_track = 0.3f;
  trem.lfo2_rate_hz = 9.0f;
  trem.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.45f};
  trem.stereo_spread = 0.5f;

  // Pizzicato Strings (GM 45): a plucked violin-family string — short KS ring
  // into the violin corpus, mid-string finger pluck.
  NativeSynthPatch& pz = o.pizzicato;
  pz.mode = SynthEngineMode::kKarplusStrong;
  pz.amp_env = env(1.0f, 0.0f, 1.0f, 200.0f);
  pz.cutoff_hz = 20000.0f;
  pz.ks.brightness = 0.48f;
  pz.ks.decay_s = 0.7f;
  pz.ks.decay_stretch = 0.55f;
  pz.ks.pick_position = 0.33f;
  pz.ks.exc_brightness = 0.6f;
  pz.ks.vel_to_brightness = 0.5f;
  pz.ks.release_damp_s = 0.06f;
  pz.ks.nail = 0.3f;  // finger flesh
  pz.ks.pluck_style = 0.4f;
  pz.body = BodyType::kViolin;
  pz.body_mix = 0.4f;
  pz.stereo_spread = 0.3f;
  pz.gain = 1.3f;

  // Timpani (GM 47): a note-tracked kettledrum — full membrane mode set, long
  // pitched ring under a soft mallet thud, no snare/noise wash.
  NativeSynthPatch& tp = o.timpani;
  tp.mode = SynthEngineMode::kPercussion;
  tp.amp_env = env(0.5f, 1800.0f, 0.0f, 500.0f);
  tp.cutoff_hz = 20000.0f;
  tp.percussion.num_modes = 5;
  tp.percussion.mode_decay_s = 1.1f;
  tp.percussion.tone_gain = 1.0f;
  tp.percussion.pitch_drop = 0.15f;
  tp.percussion.pitch_drop_ms = 40.0f;
  tp.percussion.noise_gain = 0.25f;
  tp.percussion.noise_decay_ms = 25.0f;
  tp.percussion.noise_cutoff_hz = 600.0f;
  tp.percussion.noise_output = SynthFilterOutput::kLowpass;
  // Struck the timpani way: about a quarter in from the rim, so the pitched
  // ring modes dominate over the centre thump.
  tp.percussion.strike_r = 0.6f;
  tp.stereo_spread = 0.15f;
  tp.gain = 1.2f;

  // Choir Aahs (GM 52): detuned voices through the open-vowel tract formants
  // — the vocal body is what separates "aah" from a string pad.
  NativeSynthPatch& ch = o.choir_aahs;
  ch.waveform = VaWaveform::kSaw;
  ch.unison = 4;
  ch.detune_cents = 9.0f;
  ch.drift_cents = 4.0f;
  ch.amp_env = env(260.0f, 400.0f, 0.85f, 550.0f);
  ch.cutoff_hz = 2400.0f;
  ch.lfo_rate_hz = 4.6f;
  ch.lfo_to_pitch_cents = 5.0f;
  ch.body = BodyType::kVocal;
  ch.body_mix = 0.55f;
  ch.stereo_spread = 0.6f;
  ch.gain = 0.6f;

  // Voice Oohs (GM 53): the same choir with a nearly closed mouth — darker
  // low-pass, more tract, less shimmer.
  o.voice_oohs = ch;
  o.voice_oohs.cutoff_hz = 1300.0f;
  o.voice_oohs.detune_cents = 7.0f;
  o.voice_oohs.body_mix = 0.6f;
  o.voice_oohs.gain = 0.65f;

  // Synth Voice (GM 54): brighter, steadier synthetic vowel.
  o.synth_voice = ch;
  o.synth_voice.cutoff_hz = 3200.0f;
  o.synth_voice.amp_env = env(120.0f, 300.0f, 0.9f, 400.0f);
  o.synth_voice.drift_cents = 2.0f;

  // Bowed string (GM 40-43): one friction-excited waveguide voiced across the
  // violin family. The engine tunes to the played note, so the members differ
  // by timbre (larger = darker, slower-speaking, more corpus) — mirrors the
  // violin/viola/cello/contrabass presets.
  auto bowed = [](float bow_position, float bow_force, float brightness, float damping,
                  float attack_ms, float release_ms, float body_mix, float gain) {
    NativeSynthPatch p{};
    p.mode = SynthEngineMode::kBowedString;
    p.amp_env.attack_ms = 20.0f;
    p.amp_env.sustain = 1.0f;
    p.amp_env.release_ms = release_ms;
    p.cutoff_hz = 20000.0f;
    p.bowed_string.bow_position = bow_position;
    p.bowed_string.bow_force = bow_force;
    p.bowed_string.brightness = brightness;
    p.bowed_string.damping = damping;
    p.bowed_string.attack_ms = attack_ms;
    p.bowed_string.release_ms = release_ms;
    p.bowed_string.rosin = 0.1f;
    // Bowed-string physics gates: bristle memory warms the static friction
    // table, the detuned second plane thickens the sustain, and the open
    // strings halo the bridge output.
    p.bowed_string.elasto_plastic = true;
    p.bowed_string.stribeck = 0.7f;
    p.bowed_string.polarization = 0.15f;
    p.bowed_string.sympathetic = 0.08f;
    p.drift_cents = 2.0f;
    p.stereo_spread = 0.1f;
    p.body = BodyType::kViolin;
    p.body_mix = body_mix;
    p.gain = gain;
    return p;
  };
  o.violin = bowed(0.12f, 0.55f, 0.47f, 0.32f, 45.0f, 110.0f, 0.28f, 0.3f);
  o.violin.cutoff_hz = 6000.0f;
  o.violin.lfo_rate_hz = 5.3f;
  o.violin.lfo_to_pitch_cents = 9.0f;
  o.viola = bowed(0.13f, 0.55f, 0.42f, 0.34f, 55.0f, 120.0f, 0.34f, 0.3f);
  o.viola.lfo_rate_hz = 5.1f;
  o.viola.lfo_to_pitch_cents = 8.0f;
  o.cello = bowed(0.14f, 0.60f, 0.44f, 0.38f, 70.0f, 140.0f, 0.40f, 0.28f);
  o.cello.lfo_rate_hz = 4.8f;
  o.cello.lfo_to_pitch_cents = 7.0f;
  o.contrabass = bowed(0.15f, 0.62f, 0.36f, 0.44f, 90.0f, 160.0f, 0.46f, 0.32f);
  o.contrabass.lfo_rate_hz = 4.4f;
  o.contrabass.lfo_to_pitch_cents = 5.0f;

  // Reed woodwind (GM 64-71): one single-reed waveguide voiced across the
  // single- and double-reed winds. The clarinet is the only cylinder
  // (odd-harmonic); the saxes and double reeds are conical (full series) —
  // mirrors the reed presets.
  auto reed = [](bool conical, float reed_stiffness, float reed_opening, float brightness,
                 float damping, float attack_ms, float release_ms, float breath, float body_mix,
                 float gain) {
    NativeSynthPatch p{};
    p.mode = SynthEngineMode::kReed;
    p.amp_env.attack_ms = 15.0f;
    p.amp_env.sustain = 1.0f;
    p.amp_env.release_ms = release_ms;
    p.cutoff_hz = 20000.0f;
    p.reed.conical = conical;
    p.reed.reed_stiffness = reed_stiffness;
    p.reed.reed_opening = reed_opening;
    p.reed.brightness = brightness;
    p.reed.damping = damping;
    p.reed.attack_ms = attack_ms;
    p.reed.release_ms = release_ms;
    p.reed.breath_pressure = breath;
    // Reed physics gates: the conical throat bloom restores the fundamental
    // the pure cone loses (inert on the cylindrical clarinet). The dynamic
    // mass-spring reed stays off — its formant bias overshoots the GM
    // reference timbre by >1 kHz.
    p.reed.cone_growth = conical ? 0.15f : 0.0f;
    p.drift_cents = 1.5f;
    p.stereo_spread = 0.08f;
    p.body = BodyType::kWoodTube;
    p.body_mix = body_mix;
    p.gain = gain;
    return p;
  };
  o.soprano_sax = reed(true, 0.55f, 0.55f, 0.64f, 0.32f, 16.0f, 80.0f, 0.78f, 0.30f, 0.55f);
  o.soprano_sax.cutoff_hz = 5200.0f;
  o.soprano_sax.lfo_rate_hz = 5.4f;
  o.soprano_sax.lfo_to_pitch_cents = 6.0f;
  o.soprano_sax.reed.growl = 0.15f;
  o.soprano_sax.reed.chiff = 0.55f;
  o.alto_sax = reed(true, 0.55f, 0.55f, 0.62f, 0.34f, 16.0f, 90.0f, 0.78f, 0.32f, 0.55f);
  o.alto_sax.cutoff_hz = 4500.0f;
  o.alto_sax.lfo_rate_hz = 5.2f;
  o.alto_sax.lfo_to_pitch_cents = 6.0f;
  o.alto_sax.reed.growl = 0.15f;
  o.alto_sax.reed.chiff = 0.6f;
  o.alto_sax.reed.reed_opening = 0.62f;
  o.alto_sax.reed.breath_noise = 0.3f;
  o.tenor_sax = reed(true, 0.60f, 0.50f, 0.56f, 0.36f, 20.0f, 100.0f, 0.78f, 0.36f, 0.58f);
  o.tenor_sax.cutoff_hz = 4000.0f;
  o.tenor_sax.lfo_rate_hz = 5.0f;
  o.tenor_sax.lfo_to_pitch_cents = 5.0f;
  o.tenor_sax.reed.growl = 0.18f;
  o.tenor_sax.reed.chiff = 0.6f;
  o.tenor_sax.reed.breath_noise = 0.3f;
  o.baritone_sax = reed(true, 0.60f, 0.50f, 0.5f, 0.40f, 26.0f, 120.0f, 0.78f, 0.40f, 0.58f);
  o.baritone_sax.cutoff_hz = 3800.0f;
  o.baritone_sax.lfo_rate_hz = 4.8f;
  o.baritone_sax.lfo_to_pitch_cents = 4.0f;
  o.baritone_sax.reed.growl = 0.18f;
  o.baritone_sax.reed.chiff = 0.6f;
  o.oboe = reed(true, 0.80f, 0.35f, 0.74f, 0.30f, 18.0f, 70.0f, 0.62f, 0.30f, 0.6f);
  o.oboe.cutoff_hz = 5200.0f;
  o.oboe.lfo_rate_hz = 5.5f;
  o.oboe.lfo_to_pitch_cents = 5.0f;
  o.english_horn = reed(true, 0.70f, 0.40f, 0.64f, 0.34f, 24.0f, 90.0f, 0.64f, 0.34f, 0.6f);
  o.english_horn.cutoff_hz = 4600.0f;
  o.english_horn.lfo_rate_hz = 5.2f;
  o.english_horn.lfo_to_pitch_cents = 5.0f;
  o.bassoon = reed(true, 0.65f, 0.45f, 0.5f, 0.40f, 30.0f, 120.0f, 0.68f, 0.40f, 0.62f);
  o.bassoon.cutoff_hz = 3800.0f;
  o.bassoon.lfo_rate_hz = 4.8f;
  o.bassoon.lfo_to_pitch_cents = 4.0f;
  o.clarinet = reed(false, 0.40f, 0.50f, 0.54f, 0.30f, 25.0f, 90.0f, 0.72f, 0.25f, 0.6f);
  o.clarinet.cutoff_hz = 4800.0f;
  o.clarinet.lfo_rate_hz = 5.0f;
  o.clarinet.lfo_to_pitch_cents = 2.5f;

  // Brass / lip reed (GM 56-60): one lip-reed waveguide voiced across the
  // trumpets, horns and low brass. Small-bore bells (trumpet family) get the
  // radiation formant; large-bore / mellow brass stays on the round linear
  // tone — mirrors the brass presets. (Brass Section 61 + SynthBrass 62-63
  // stay FM by design.)
  auto brass = [](bool conical, float lip_tension, float lip_damping, float brightness,
                  float damping, float attack_ms, float release_ms, float breath, float bell_mix,
                  float gain) {
    NativeSynthPatch p{};
    p.mode = SynthEngineMode::kBrass;
    p.amp_env.attack_ms = 12.0f;
    p.amp_env.sustain = 1.0f;
    p.amp_env.release_ms = release_ms;
    p.cutoff_hz = 20000.0f;
    p.brass.conical = conical;
    p.brass.lip_tension = lip_tension;
    p.brass.lip_damping = lip_damping;
    p.brass.brightness = brightness;
    p.brass.damping = damping;
    p.brass.attack_ms = attack_ms;
    p.brass.release_ms = release_ms;
    p.brass.breath_pressure = breath;
    p.brass.vel_to_breath = 0.5f;
    // Brass physics gates: the linear waveguide is deliberately dark — the
    // cuivré shock shaper is what manufactures the bright blare of real
    // brass; the 2-DOF lip livens the attack buzz.
    p.brass.dynamic_lip = 0.25f;
    p.drift_cents = 1.5f;
    p.stereo_spread = 0.08f;
    if (bell_mix > 0.0f) {
      p.body = BodyType::kBrassBell;
      p.body_mix = bell_mix;
    }
    p.gain = gain;
    return p;
  };
  o.trumpet = brass(false, 0.55f, 0.30f, 0.75f, 0.28f, 12.0f, 80.0f, 0.88f, 0.50f, 0.90f);
  o.trumpet.cutoff_hz = 6500.0f;
  o.trumpet.brass.brassiness = 0.55f;
  o.trumpet.brass.cuivre_dynamics = 0.7f;
  o.trumpet.lfo_rate_hz = 5.5f;
  o.trumpet.lfo_to_pitch_cents = 4.0f;
  o.trombone = brass(false, 0.48f, 0.45f, 0.85f, 0.32f, 26.0f, 100.0f, 0.85f, 0.0f, 0.92f);
  o.trombone.cutoff_hz = 3800.0f;
  o.trombone.brass.brassiness = 0.85f;
  o.trombone.brass.cuivre_dynamics = 0.7f;
  o.trombone.lfo_rate_hz = 5.0f;
  o.trombone.lfo_to_pitch_cents = 3.0f;
  o.tuba = brass(true, 0.42f, 0.70f, 0.38f, 0.42f, 40.0f, 140.0f, 0.88f, 0.0f, 0.92f);
  o.tuba.cutoff_hz = 3200.0f;
  o.tuba.brass.brassiness = 0.25f;
  o.tuba.brass.cuivre_dynamics = 0.5f;
  o.tuba.lfo_to_pitch_cents = 1.5f;
  // The muted trumpet plays through the real mute model instead of the old
  // dimmed-brightness fake.
  o.muted_trumpet = brass(false, 0.58f, 0.35f, 0.62f, 0.30f, 16.0f, 75.0f, 0.80f, 0.0f, 0.82f);
  o.muted_trumpet.brass.brassiness = 0.4f;
  o.muted_trumpet.brass.cuivre_dynamics = 0.5f;
  o.muted_trumpet.brass.mute = 0.65f;
  o.muted_trumpet.lfo_rate_hz = 5.5f;
  o.muted_trumpet.lfo_to_pitch_cents = 4.0f;
  o.french_horn = brass(true, 0.50f, 0.55f, 0.48f, 0.34f, 30.0f, 110.0f, 0.82f, 0.0f, 0.88f);
  o.french_horn.cutoff_hz = 3600.0f;
  o.french_horn.brass.brassiness = 0.3f;
  o.french_horn.brass.cuivre_dynamics = 0.6f;
  o.french_horn.lfo_to_pitch_cents = 1.5f;

  // Air-jet flute (GM 72-79): one edge-tone waveguide voiced across the
  // open-pipe flutes and their breathier relatives — mirrors the flute presets.
  auto flute = [](float jet_ratio, float brightness, float damping, float breath_noise, float chiff,
                  float vibrato_depth, float breath, float gain) {
    NativeSynthPatch p{};
    p.mode = SynthEngineMode::kFlute;
    p.amp_env.attack_ms = 8.0f;
    p.amp_env.sustain = 1.0f;
    p.amp_env.release_ms = 120.0f;
    p.cutoff_hz = 20000.0f;
    p.flute.jet_ratio = jet_ratio;
    p.flute.brightness = brightness;
    p.flute.damping = damping;
    p.flute.breath_noise = breath_noise;
    p.flute.chiff = chiff;
    p.flute.vibrato_depth = vibrato_depth;
    p.flute.vibrato_rate_hz = 5.0f;
    p.flute.breath_pressure = breath;
    p.flute.vel_to_breath = 0.5f;
    // Flute physics gates: turbulence lets the breath grow and brighten with
    // flow instead of sitting at a fixed hiss.
    p.flute.jet_turbulence = 0.3f;
    p.drift_cents = 1.5f;
    p.stereo_spread = 0.08f;
    p.gain = gain;
    return p;
  };
  o.piccolo = flute(0.50f, 0.75f, 0.25f, 0.18f, 0.40f, 0.10f, 0.62f, 0.95f);
  o.piccolo.amp_env.attack_ms = 40.0f;
  o.piccolo.flute.overblow = 0.3f;
  o.concert_flute = flute(0.50f, 0.55f, 0.30f, 0.35f, 0.2f, 0.15f, 0.60f, 0.85f);
  o.concert_flute.cutoff_hz = 5000.0f;
  o.concert_flute.amp_env.attack_ms = 90.0f;
  o.concert_flute.flute.overblow = 0.35f;
  o.recorder = flute(0.50f, 0.50f, 0.35f, 0.14f, 0.55f, 0.05f, 0.55f, 0.85f);
  o.recorder.body = BodyType::kWoodTube;
  o.recorder.body_mix = 0.15f;
  o.pan_flute = flute(0.52f, 0.42f, 0.40f, 0.40f, 0.30f, 0.08f, 0.55f, 0.85f);
  o.pan_flute.flute.vortex = 0.35f;
  o.pan_flute.body = BodyType::kWoodTube;
  o.pan_flute.body_mix = 0.15f;
  o.blown_bottle = flute(0.50f, 0.35f, 0.50f, 0.35f, 0.25f, 0.0f, 0.55f, 0.85f);
  o.shakuhachi = flute(0.52f, 0.48f, 0.35f, 0.55f, 0.30f, 0.20f, 0.58f, 0.85f);
  o.shakuhachi.flute.vortex = 0.5f;
  o.shakuhachi.body = BodyType::kWoodTube;
  o.shakuhachi.body_mix = 0.2f;
  o.tin_whistle = flute(0.48f, 0.70f, 0.28f, 0.10f, 0.45f, 0.04f, 0.62f, 0.80f);
  o.tin_whistle.flute.overblow = 0.25f;
  o.ocarina = flute(0.50f, 0.40f, 0.55f, 0.15f, 0.30f, 0.06f, 0.55f, 0.85f);

  o.e_piano = clamp_synth_patch(o.e_piano);
  o.harpsichord = clamp_synth_patch(o.harpsichord);
  o.harpsichord_octave = clamp_synth_patch(o.harpsichord_octave);
  o.harpsichord_wide = clamp_synth_patch(o.harpsichord_wide);
  o.harpsichord_keyoff = clamp_synth_patch(o.harpsichord_keyoff);
  o.clav = clamp_synth_patch(o.clav);
  o.glockenspiel = clamp_synth_patch(o.glockenspiel);
  o.vibraphone = clamp_synth_patch(o.vibraphone);
  o.marimba = clamp_synth_patch(o.marimba);
  o.xylophone = clamp_synth_patch(o.xylophone);
  o.nylon_guitar = clamp_synth_patch(o.nylon_guitar);
  o.electric_guitar = clamp_synth_patch(o.electric_guitar);
  o.muted_guitar = clamp_synth_patch(o.muted_guitar);
  o.overdriven = clamp_synth_patch(o.overdriven);
  o.distortion = clamp_synth_patch(o.distortion);
  o.bass_acoustic = clamp_synth_patch(o.bass_acoustic);
  o.bass_fingered = clamp_synth_patch(o.bass_fingered);
  o.bass_picked = clamp_synth_patch(o.bass_picked);
  o.bass_fretless = clamp_synth_patch(o.bass_fretless);
  o.bass_slap = clamp_synth_patch(o.bass_slap);
  o.bass_pop = clamp_synth_patch(o.bass_pop);
  o.harp = clamp_synth_patch(o.harp);
  o.church_organ = clamp_synth_patch(o.church_organ);
  o.reed_organ = clamp_synth_patch(o.reed_organ);
  o.tremolo_strings = clamp_synth_patch(o.tremolo_strings);
  o.pizzicato = clamp_synth_patch(o.pizzicato);
  o.timpani = clamp_synth_patch(o.timpani);
  o.choir_aahs = clamp_synth_patch(o.choir_aahs);
  o.voice_oohs = clamp_synth_patch(o.voice_oohs);
  o.synth_voice = clamp_synth_patch(o.synth_voice);
  o.violin = clamp_synth_patch(o.violin);
  o.viola = clamp_synth_patch(o.viola);
  o.cello = clamp_synth_patch(o.cello);
  o.contrabass = clamp_synth_patch(o.contrabass);
  o.trumpet = clamp_synth_patch(o.trumpet);
  o.trombone = clamp_synth_patch(o.trombone);
  o.tuba = clamp_synth_patch(o.tuba);
  o.muted_trumpet = clamp_synth_patch(o.muted_trumpet);
  o.french_horn = clamp_synth_patch(o.french_horn);
  o.soprano_sax = clamp_synth_patch(o.soprano_sax);
  o.alto_sax = clamp_synth_patch(o.alto_sax);
  o.tenor_sax = clamp_synth_patch(o.tenor_sax);
  o.baritone_sax = clamp_synth_patch(o.baritone_sax);
  o.oboe = clamp_synth_patch(o.oboe);
  o.english_horn = clamp_synth_patch(o.english_horn);
  o.bassoon = clamp_synth_patch(o.bassoon);
  o.clarinet = clamp_synth_patch(o.clarinet);
  o.piccolo = clamp_synth_patch(o.piccolo);
  o.concert_flute = clamp_synth_patch(o.concert_flute);
  o.recorder = clamp_synth_patch(o.recorder);
  o.pan_flute = clamp_synth_patch(o.pan_flute);
  o.blown_bottle = clamp_synth_patch(o.blown_bottle);
  o.shakuhachi = clamp_synth_patch(o.shakuhachi);
  o.tin_whistle = clamp_synth_patch(o.tin_whistle);
  o.ocarina = clamp_synth_patch(o.ocarina);
  return o;
}

const ProgramOverrides& program_overrides() noexcept {
  static const ProgramOverrides kTable = build_program_overrides();
  return kTable;
}

/// GM drum-note categories -> one-shot patches. Pitched pieces (kick / toms)
/// play at the struck key's frequency; wires / hats / cymbals are filtered
/// seeded noise.
struct DrumPatches {
  NativeSynthPatch kick;
  NativeSynthPatch snare;
  NativeSynthPatch closed_hat;
  NativeSynthPatch open_hat;
  NativeSynthPatch tom;
  NativeSynthPatch cymbal;
  NativeSynthPatch percussion;
};

DrumPatches build_drum_patches() noexcept {
  DrumPatches d{};

  // Common kit-piece scaffolding: membrane-modal + noise voices (method
  // (6)), one-shot, wrapper filter bypassed (the percussion core owns its
  // own noise band).
  NativeSynthPatch piece{};
  piece.mode = SynthEngineMode::kPercussion;
  piece.one_shot = true;
  piece.cutoff_hz = 20000.0f;

  // Kick: membrane fundamental + first ring mode at the struck key
  // (~61/65 Hz) with the tension-release pitch drop, plus a low beater thud.
  d.kick = piece;
  d.kick.amp_env = env(0.5f, 220.0f, 0.0f, 60.0f);
  d.kick.percussion.num_modes = 2;
  d.kick.percussion.mode_decay_s = 0.22f;
  d.kick.percussion.pitch_drop = 1.5f;
  d.kick.percussion.pitch_drop_ms = 45.0f;
  d.kick.percussion.noise_gain = 0.35f;
  d.kick.percussion.noise_decay_ms = 20.0f;
  d.kick.percussion.noise_cutoff_hz = 900.0f;
  d.kick.percussion.noise_output = SynthFilterOutput::kLowpass;
  // Beater lands near the membrane centre: the m == 0 thump dominates and the
  // single ring mode is held back.
  d.kick.percussion.strike_r = 0.12f;
  // A low shell mode extends the boom under the beater thud.
  d.kick.percussion.shell_mix = 0.18f;
  d.kick.percussion.shell_num_modes = 1;
  d.kick.percussion.shell_freq_hz = {80.0f, 0.0f, 0.0f, 0.0f};
  d.kick.percussion.shell_t60_s = {0.14f, 0.0f, 0.0f, 0.0f};
  d.kick.percussion.shell_weight = {1.0f, 0.0f, 0.0f, 0.0f};
  d.kick.gain = 1.1f;

  // Snare: fixed 185 Hz shell (Rayleigh modes) + the wire crack band.
  d.snare = piece;
  d.snare.amp_env = env(0.5f, 250.0f, 0.0f, 80.0f);
  d.snare.percussion.num_modes = 5;
  d.snare.percussion.base_freq_hz = 185.0f;
  d.snare.percussion.mode_decay_s = 0.12f;
  d.snare.percussion.tone_gain = 0.7f;
  d.snare.percussion.pitch_drop = 0.4f;
  d.snare.percussion.pitch_drop_ms = 25.0f;
  d.snare.percussion.noise_gain = 1.1f;
  d.snare.percussion.noise_decay_ms = 160.0f;
  d.snare.percussion.noise_cutoff_hz = 1800.0f;
  d.snare.percussion.noise_q = 0.9f;
  // Struck off-centre so the m >= 1 shell modes voice the pitched body under
  // the wire crack.
  d.snare.percussion.strike_r = 0.55f;
  // Woody shell body under the snare crack.
  d.snare.percussion.shell_mix = 0.2f;
  d.snare.percussion.shell_num_modes = 2;
  d.snare.percussion.shell_freq_hz = {330.0f, 480.0f, 0.0f, 0.0f};
  d.snare.percussion.shell_t60_s = {0.08f, 0.05f, 0.0f, 0.0f};
  d.snare.percussion.shell_weight = {1.0f, 0.6f, 0.0f, 0.0f};
  // Wires rattle against the bottom head while the shell rings -- a
  // velocity-dependent buzz over the wire crack.
  d.snare.percussion.wire_buzz = 0.9f;
  d.snare.percussion.wire_threshold = 0.08f;
  d.snare.percussion.wire_cutoff_hz = 4500.0f;
  d.snare.gain = 0.8f;

  // Hi-hats: high-passed noise shimmer, closed short / open ringing.
  d.closed_hat = piece;
  d.closed_hat.amp_env = env(0.5f, 90.0f, 0.0f, 40.0f);
  d.closed_hat.percussion.noise_gain = 1.0f;
  d.closed_hat.percussion.noise_decay_ms = 35.0f;
  d.closed_hat.percussion.noise_cutoff_hz = 7500.0f;
  d.closed_hat.percussion.noise_output = SynthFilterOutput::kHighpass;
  d.closed_hat.gain = 0.5f;
  d.open_hat = d.closed_hat;
  d.open_hat.amp_env = env(0.5f, 550.0f, 0.0f, 150.0f);
  d.open_hat.percussion.noise_decay_ms = 350.0f;

  // Toms: note-tracked membrane (full Rayleigh set) with a pitch drop.
  d.tom = piece;
  d.tom.amp_env = env(0.5f, 400.0f, 0.0f, 120.0f);
  d.tom.percussion.num_modes = 5;
  d.tom.percussion.mode_decay_s = 0.3f;
  d.tom.percussion.pitch_drop = 0.6f;
  d.tom.percussion.pitch_drop_ms = 55.0f;
  d.tom.percussion.noise_gain = 0.25f;
  d.tom.percussion.noise_decay_ms = 30.0f;
  d.tom.percussion.noise_cutoff_hz = 1500.0f;
  // Off-centre head strike: the full Rayleigh set voices the tom's pitch.
  d.tom.percussion.strike_r = 0.6f;
  // Note-tracked shell (0 Hz = track the struck key) plus an upper body mode
  // so one tom patch voices every tom size.
  d.tom.percussion.shell_mix = 0.25f;
  d.tom.percussion.shell_num_modes = 2;
  d.tom.percussion.shell_freq_hz = {0.0f, 330.0f, 0.0f, 0.0f};
  d.tom.percussion.shell_t60_s = {0.12f, 0.06f, 0.0f, 0.0f};
  d.tom.percussion.shell_weight = {1.0f, 0.4f, 0.0f, 0.0f};
  d.tom.gain = 1.0f;

  // Cymbals: long high-passed noise + a sparse inharmonic ring-mode bell.
  d.cymbal = piece;
  d.cymbal.amp_env = env(0.5f, 1400.0f, 0.0f, 400.0f);
  d.cymbal.percussion.num_modes = 4;
  d.cymbal.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 2.15f, 0.0f, 0.0f};
  d.cymbal.percussion.base_freq_hz = 3600.0f;
  d.cymbal.percussion.mode_decay_s = 1.1f;
  d.cymbal.percussion.tone_gain = 0.25f;
  d.cymbal.percussion.noise_gain = 0.9f;
  d.cymbal.percussion.noise_decay_ms = 900.0f;
  d.cymbal.percussion.noise_cutoff_hz = 5500.0f;
  d.cymbal.percussion.noise_output = SynthFilterOutput::kHighpass;
  // Nonlinear shimmer: the inharmonic modes pump a high wash that swells after
  // the crash and rides the long ring -- the cymbal "bloom" a static bank
  // lacks.
  d.cymbal.percussion.shimmer = 6.0f;
  d.cymbal.percussion.shimmer_attack_ms = 60.0f;
  d.cymbal.percussion.shimmer_cutoff_hz = 9000.0f;
  d.cymbal.gain = 0.5f;

  // Everything else (claps, shakers, latin percussion): short noise burst
  // with a faint note-tracked knock.
  d.percussion = piece;
  d.percussion.amp_env = env(0.5f, 200.0f, 0.0f, 80.0f);
  d.percussion.percussion.num_modes = 1;
  d.percussion.percussion.mode_decay_s = 0.08f;
  d.percussion.percussion.tone_gain = 0.4f;
  d.percussion.percussion.noise_gain = 0.9f;
  d.percussion.percussion.noise_decay_ms = 110.0f;
  d.percussion.percussion.noise_cutoff_hz = 2500.0f;
  d.percussion.percussion.noise_q = 1.5f;
  d.percussion.gain = 0.7f;

  d.kick = clamp_synth_patch(d.kick);
  d.snare = clamp_synth_patch(d.snare);
  d.closed_hat = clamp_synth_patch(d.closed_hat);
  d.open_hat = clamp_synth_patch(d.open_hat);
  d.tom = clamp_synth_patch(d.tom);
  d.cymbal = clamp_synth_patch(d.cymbal);
  d.percussion = clamp_synth_patch(d.percussion);
  return d;
}

const std::array<NativeSynthPatch, 16>& family_patches() noexcept {
  static const std::array<NativeSynthPatch, 16> kTable = build_family_patches();
  return kTable;
}

const DrumPatches& drum_patches() noexcept {
  static const DrumPatches kTable = build_drum_patches();
  return kTable;
}

// Per-note GM/GS drum map (keys 27..87): each key is a distinct instrument
// built from a mechanism archetype (fixed-pitch membrane / struck wood / struck
// metal / whistle / noise) plus a fixed tuning, on top of the shared kit
// archetypes above. Unmapped keys fall back to the generic short burst so every
// drum key stays audible. The stochastic (PhISEM) shakers and scrapers
// (maracas, cabasa, guiro, cuica, tambourine, vibraslap) are a separate
// archetype not yet built — they resolve to the generic burst here until it
// lands.
std::array<NativeSynthPatch, 128> build_drum_note_table() noexcept {
  const DrumPatches& d = drum_patches();
  std::array<NativeSynthPatch, 128> t{};

  NativeSynthPatch piece{};
  piece.mode = SynthEngineMode::kPercussion;
  piece.one_shot = true;
  piece.cutoff_hz = 20000.0f;

  // Fixed-pitch membrane (conga/bongo/timbale/surdo): unlike the key-tracked
  // toms, GM pins one head frequency per key.
  auto make_membrane = [&](float base_hz, float decay_s, float drop, float shell_hz, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(0.5f, decay_s * 1000.0f + 120.0f, 0.0f, 40.0f);
    p.percussion.num_modes = 5;
    p.percussion.base_freq_hz = base_hz;
    p.percussion.mode_decay_s = decay_s;
    p.percussion.pitch_drop = drop;
    p.percussion.pitch_drop_ms = 30.0f;
    p.percussion.tone_gain = 0.8f;
    p.percussion.noise_gain = 0.2f;
    p.percussion.noise_decay_ms = 18.0f;
    p.percussion.noise_cutoff_hz = 2000.0f;
    p.percussion.strike_r = 0.55f;
    if (shell_hz > 0.0f) {
      p.percussion.shell_mix = 0.2f;
      p.percussion.shell_num_modes = 1;
      p.percussion.shell_freq_hz = {shell_hz, 0.0f, 0.0f, 0.0f};
      p.percussion.shell_t60_s = {0.06f, 0.0f, 0.0f, 0.0f};
      p.percussion.shell_weight = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    p.gain = gain;
    return p;
  };

  // Struck wooden idiophone (claves/woodblock/side stick/clicks): one or two
  // high-Q wood resonances at a fixed pitch plus a short stick click.
  auto make_wood = [&](float base_hz, float ratio2, float decay_s, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(0.3f, decay_s * 1000.0f + 40.0f, 0.0f, 20.0f);
    p.percussion.num_modes = ratio2 > 0.0f ? 2 : 1;
    p.percussion.mode_ratios = {1.0f, ratio2, 0.0f, 0.0f, 0.0f, 0.0f};
    p.percussion.base_freq_hz = base_hz;
    p.percussion.mode_decay_s = decay_s;
    p.percussion.tone_gain = 0.9f;
    p.percussion.noise_gain = 0.3f;
    p.percussion.noise_decay_ms = 4.0f;
    p.percussion.noise_cutoff_hz = base_hz * 2.0f;
    p.percussion.noise_output = SynthFilterOutput::kBandpass;
    p.gain = gain;
    return p;
  };

  // Struck metal idiophone (cowbell/agogo/triangle/bells): sparse inharmonic
  // high-Q modes with a longer ring and only a trace of strike noise.
  auto make_metal = [&](float base_hz, std::array<float, kMaxPercussionModes> ratios, int nmodes,
                        float decay_s, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(0.3f, decay_s * 1000.0f + 60.0f, 0.0f, 30.0f);
    p.percussion.num_modes = nmodes;
    p.percussion.mode_ratios = ratios;
    p.percussion.base_freq_hz = base_hz;
    p.percussion.mode_decay_s = decay_s;
    p.percussion.tone_gain = 0.5f;
    p.percussion.noise_gain = 0.15f;
    p.percussion.noise_decay_ms = 8.0f;
    p.percussion.noise_cutoff_hz = base_hz * 3.0f;
    p.percussion.noise_output = SynthFilterOutput::kBandpass;
    p.gain = gain;
    return p;
  };

  // Whistle (Phase-1 approximation): a strong resonant tone with breath noise.
  // Superseded by the flue-pipe core once that lands.
  auto make_whistle = [&](float base_hz, float decay_s, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(3.0f, decay_s * 1000.0f + 40.0f, 0.0f, 25.0f);
    p.percussion.num_modes = 1;
    p.percussion.mode_ratios = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    p.percussion.base_freq_hz = base_hz;
    p.percussion.mode_decay_s = decay_s;
    p.percussion.tone_gain = 0.8f;
    p.percussion.noise_gain = 0.4f;
    p.percussion.noise_decay_ms = decay_s * 1000.0f;
    p.percussion.noise_cutoff_hz = base_hz;
    p.percussion.noise_q = 4.0f;
    p.percussion.noise_output = SynthFilterOutput::kBandpass;
    p.gain = gain;
    return p;
  };

  // Shaker (PhISEM): a burst of stochastic bead collisions rung through a gourd
  // resonance — maracas, cabasa, shaker, tambourine, vibraslap.
  auto make_shaker = [&](float beans, float energy_ms, float res_hz, float res_q, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(0.5f, energy_ms + 200.0f, 0.0f, 40.0f);
    p.percussion.phisem_beans = beans;
    p.percussion.phisem_energy_ms = energy_ms;
    p.percussion.phisem_sound_ms = 3.0f;
    p.percussion.phisem_res_hz = res_hz;
    p.percussion.phisem_res_q = res_q;
    p.gain = gain;
    return p;
  };

  // Scraper (PhISEM): quasi-periodic ridge collisions — guiro (ratchet) and
  // cuica (with a resonance pitch glide).
  auto make_scrape = [&](float beans, float energy_ms, float scrape_hz, float res_hz, float res_q,
                         float glide, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = env(0.5f, energy_ms + 200.0f, 0.0f, 40.0f);
    p.percussion.phisem_beans = beans;
    p.percussion.phisem_energy_ms = energy_ms;
    p.percussion.phisem_sound_ms = 4.0f;
    p.percussion.phisem_scrape_hz = scrape_hz;
    p.percussion.phisem_res_hz = res_hz;
    p.percussion.phisem_res_q = res_q;
    p.percussion.phisem_pitch_glide = glide;
    p.gain = gain;
    return p;
  };

  // Hand clap: a dense band-passed noise burst.
  NativeSynthPatch clap = piece;
  clap.amp_env = env(0.5f, 120.0f, 0.0f, 40.0f);
  clap.percussion.noise_gain = 1.0f;
  clap.percussion.noise_decay_ms = 90.0f;
  clap.percussion.noise_cutoff_hz = 1300.0f;
  clap.percussion.noise_q = 1.2f;
  clap.percussion.noise_output = SynthFilterOutput::kBandpass;
  clap.gain = 0.7f;

  // Default every key to the generic short burst (keeps unmapped keys audible;
  // also the current home of the not-yet-built stochastic shakers/scrapers).
  for (auto& p : t) p = d.percussion;

  // --- existing kit archetypes (unchanged voicings) ---
  t[35] = d.kick;
  t[36] = d.kick;
  t[38] = d.snare;
  t[40] = d.snare;
  t[42] = d.closed_hat;  // closed
  t[44] = d.closed_hat;  // pedal
  t[46] = d.open_hat;
  t[41] = t[43] = t[45] = t[47] = t[48] = t[50] = d.tom;
  t[49] = t[52] = t[55] = t[57] = d.cymbal;  // crash 1 / china / splash / crash 2
  t[51] = t[59] = d.cymbal;                  // ride 1 / ride 2

  // Hi-hats share mute group 1; the open hat gets a snappy choke fade (release
  // is unused by one-shot voices in normal play, so this stays bit-identical
  // there — it only governs how fast a closed/pedal strike cuts the open hat).
  t[42].percussion.exclusive_class = 1;
  t[44].percussion.exclusive_class = 1;
  t[46].percussion.exclusive_class = 1;
  t[46].amp_env.release_ms = 40.0f;

  // --- wooden idiophones + clicks ---
  t[31] = make_wood(1000.0f, 0.0f, 0.03f, 0.6f);   // Sticks
  t[32] = make_wood(1000.0f, 0.0f, 0.02f, 0.5f);   // Square Click
  t[33] = make_wood(1200.0f, 0.0f, 0.02f, 0.5f);   // Metronome Click
  t[37] = make_wood(820.0f, 0.0f, 0.05f, 0.7f);    // Side Stick
  t[75] = make_wood(2500.0f, 0.0f, 0.025f, 0.6f);  // Claves (2500 Hz, ~25 ms)
  t[76] = make_wood(1200.0f, 0.0f, 0.06f, 0.6f);   // Hi Wood Block
  t[77] = make_wood(800.0f, 0.0f, 0.07f, 0.6f);    // Low Wood Block
  t[85] = make_wood(1800.0f, 0.0f, 0.02f, 0.5f);   // Castanets

  // --- metal idiophones + bells ---
  t[34] =
      make_metal(1500.0f, {1.0f, 2.8f, 5.4f, 0.0f, 0.0f, 0.0f}, 3, 0.3f, 0.4f);  // Metronome Bell
  t[53] = make_metal(1200.0f, {1.0f, 1.5f, 2.6f, 0.0f, 0.0f, 0.0f}, 3, 0.6f, 0.4f);  // Ride Bell
  t[56] = make_metal(587.0f, {1.0f, 1.44f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.25f,
                     0.5f);  // Cowbell (587/845 Hz)
  t[67] = make_metal(1200.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.25f, 0.45f);  // High Agogo
  t[68] = make_metal(900.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.30f, 0.45f);   // Low Agogo
  t[83] =
      make_metal(2500.0f, {1.0f, 1.7f, 2.4f, 0.0f, 0.0f, 0.0f}, 3, 0.40f, 0.35f);  // Jingle Bell
  t[84] = make_metal(3000.0f, {1.0f, 1.6f, 2.3f, 3.1f, 0.0f, 0.0f}, 4, 1.50f, 0.30f);  // Belltree

  // Triangle: high inharmonic modes; mute short, open long (mute group 3).
  const std::array<float, kMaxPercussionModes> triangle_ratios = {1.0f,  2.76f, 5.40f,
                                                                  8.90f, 0.0f,  0.0f};
  t[80] = make_metal(5000.0f, triangle_ratios, 4, 0.15f, 0.35f);  // Mute Triangle
  t[81] = make_metal(5000.0f, triangle_ratios, 4, 1.20f, 0.35f);  // Open Triangle
  t[80].percussion.exclusive_class = 3;
  t[81].percussion.exclusive_class = 3;

  // --- fixed-pitch membranes (congas/bongos/timbales/surdo) ---
  t[60] = make_membrane(260.0f, 0.18f, 0.30f, 0.0f, 0.70f);    // Hi Bongo
  t[61] = make_membrane(180.0f, 0.20f, 0.30f, 0.0f, 0.70f);    // Low Bongo
  t[62] = make_membrane(220.0f, 0.08f, 0.20f, 0.0f, 0.70f);    // Mute Hi Conga
  t[63] = make_membrane(200.0f, 0.25f, 0.30f, 0.0f, 0.70f);    // Open Hi Conga
  t[64] = make_membrane(130.0f, 0.30f, 0.35f, 0.0f, 0.75f);    // Low Conga
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 0.70f);  // High Timbale
  t[66] = make_membrane(200.0f, 0.26f, 0.20f, 550.0f, 0.70f);  // Low Timbale
  t[86] = make_membrane(95.0f, 0.12f, 0.40f, 0.0f, 0.80f);     // Mute Surdo
  t[87] = make_membrane(80.0f, 0.40f, 0.50f, 0.0f, 0.85f);     // Open Surdo
  t[86].percussion.exclusive_class = 6;
  t[87].percussion.exclusive_class = 6;

  // --- whistles (mute group 4) + hand clap ---
  t[71] = make_whistle(1400.0f, 0.12f, 0.5f);  // Short Whistle
  t[72] = make_whistle(1400.0f, 0.50f, 0.5f);  // Long Whistle
  t[71].percussion.exclusive_class = 4;
  t[72].percussion.exclusive_class = 4;
  t[39] = clap;  // Hand Clap

  // --- PhISEM shakers + scrapers ---
  t[54] = make_shaker(32.0f, 120.0f, 2500.0f, 2.0f, 0.5f);   // Tambourine
  t[58] = make_shaker(24.0f, 400.0f, 2500.0f, 3.0f, 0.45f);  // Vibraslap
  t[69] = make_shaker(24.0f, 90.0f, 4000.0f, 1.0f, 0.5f);    // Cabasa
  t[70] = make_shaker(20.0f, 90.0f, 3200.0f, 1.5f, 0.5f);    // Maracas
  t[82] = make_shaker(28.0f, 110.0f, 6000.0f, 1.0f, 0.5f);   // Shaker
  // Guiro (mute group 5): ratchet ridge train.
  t[73] = make_scrape(8.0f, 120.0f, 150.0f, 2500.0f, 3.0f, 0.0f, 0.5f);  // Short Guiro
  t[74] = make_scrape(8.0f, 500.0f, 120.0f, 2500.0f, 3.0f, 0.0f, 0.5f);  // Long Guiro
  t[73].percussion.exclusive_class = 5;
  t[74].percussion.exclusive_class = 5;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 0.55f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);   // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  t[79].percussion.exclusive_class = 2;

  for (auto& p : t) p = clamp_synth_patch(p);
  return t;
}

const std::array<NativeSynthPatch, 128>& drum_note_table() noexcept {
  static const std::array<NativeSynthPatch, 128> kTable = build_drum_note_table();
  return kTable;
}

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
    case 9:  // Glockenspiel
      return program_overrides().glockenspiel;
    case 11:  // Vibraphone
      return program_overrides().vibraphone;
    case 12:  // Marimba
      return program_overrides().marimba;
    case 13:  // Xylophone
      return program_overrides().xylophone;
    case 19:  // Church Organ (flue pipe)
      return program_overrides().church_organ;
    case 20:  // Reed Organ (lingual reed pipe / harmonium)
    case 21:  // Accordion (free reed — shares the reed-organ voicing until a
              // dedicated free-reed model lands)
      return program_overrides().reed_organ;
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
    default:
      break;
  }
  return family_patches()[static_cast<size_t>((program & 0x7Fu) >> 3)];
}

const NativeSynthPatch& gm_fallback_drum_patch(uint8_t note) noexcept {
  return drum_note_table()[note & 0x7Fu];
}

GmFallbackSends gm_fallback_sends(uint16_t bank, uint8_t program) noexcept {
  if (bank == 128) return {0.6f, 0.6f};  // drums: tighter than the melodics
  switch (program & 0x7Fu) {
    case 19:  // Church Organ lives in a cathedral, not a booth
      return {2.2f, 1.0f};
    case 46:  // Orchestral Harp: concert-hall halo
      return {1.6f, 1.0f};
    case 48:  // String Ensembles: hall + section shimmer
    case 49:
    case 50:
    case 51:
      return {1.6f, 3.0f};
    case 52:  // Choir Aahs / Voice Oohs / Synth Voice
    case 53:
    case 54:
      return {1.8f, 3.5f};
    default:
      break;
  }
  switch ((program & 0x7Fu) >> 3) {
    case 0:
      return {1.0f, 1.0f};  // pianos: lid-open room
    case 1:
      return {1.3f, 1.0f};  // chromatic percussion rings in air
    case 2:
      return {1.1f, 1.5f};  // organs
    case 3:
      return {0.8f, 1.5f};  // guitars
    case 4:
      return {0.4f, 0.6f};  // basses stay tight
    case 5:
      return {1.2f, 1.0f};  // solo strings
    case 6:
      return {1.6f, 3.0f};  // ensembles (non-override programs)
    case 7:
      return {1.0f, 1.2f};  // brass
    case 8:
      return {0.9f, 1.0f};  // reeds
    case 9:
      return {1.1f, 1.0f};  // flutes
    case 10:
      return {0.7f, 1.5f};  // synth leads
    case 11:
      return {1.7f, 3.0f};  // synth pads bathe in the wash
    case 12:
      return {1.5f, 2.5f};  // synth FX
    case 13:
      return {0.9f, 1.0f};  // ethnic plucked
    case 14:
      return {1.2f, 1.0f};  // percussive
    default:
      return {1.4f, 1.0f};  // SFX
  }
}

uint8_t gm_fallback_drum_kit(uint8_t program) noexcept {
  switch (program & 0x7Fu) {
    case 8:
      return 1;  // Room
    case 16:
      return 2;  // Power
    case 24:
      return 3;  // Electronic
    case 25:
      return 4;  // TR-808
    case 32:
      return 5;  // Jazz
    case 40:
      return 6;  // Brush
    case 48:
      return 7;  // Orchestra
    case 56:
      return 8;  // SFX
    default:
      return 0;  // Standard
  }
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
    const ProgramOverrides& o = program_overrides();
    for (const NativeSynthPatch* p :
         {&o.e_piano, &o.clav, &o.glockenspiel, &o.vibraphone, &o.marimba, &o.xylophone,
          &o.nylon_guitar, &o.electric_guitar, &o.muted_guitar, &o.overdriven, &o.distortion,
          &o.harp, &o.church_organ, &o.reed_organ}) {
      max_ms = std::max(max_ms, std::max(p->amp_env.release_ms, p->amp_env.decay_ms));
    }
    return max_ms;
  }();
  return kMax;
}

}  // namespace sonare::midi::synth
