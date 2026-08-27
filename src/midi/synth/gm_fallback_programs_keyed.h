#pragma once

#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

constexpr void configure_keyed_programs(ProgramOverrides& o) noexcept {
  // FM e-piano (Rhodes/Wurli sketch): body pair at 1:1 with a velocity-driven
  // index plus a fast-decaying 14:1 "tine" pair — the exponential index
  // fall-off is what reads as an electric piano.
  NativeSynthPatch& ep = o.e_piano;
  ep.mode = SynthEngineMode::kFm;
  ep.amp_env = fallback_env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.algorithm = FmAlgorithm::kPair2x2;
  ep.fm.ops[0].ratio = 1.0f;  // body carrier
  ep.fm.ops[0].level = 1.0f;
  ep.fm.ops[0].env = fallback_env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.ops[0].key_rate_scale = 0.4f;
  ep.fm.ops[1].ratio = 1.0f;  // body modulator (warmth -> velocity)
  ep.fm.ops[1].level = 0.9f;
  ep.fm.ops[1].env = fallback_env(1.0f, 1200.0f, 0.0f, 250.0f);
  ep.fm.ops[1].vel_to_level = 0.7f;
  ep.fm.ops[1].key_rate_scale = 0.5f;
  ep.fm.ops[2].ratio = 1.0f;  // tine carrier (quiet sparkle)
  ep.fm.ops[2].level = 0.3f;
  ep.fm.ops[2].env = fallback_env(1.0f, 600.0f, 0.0f, 150.0f);
  ep.fm.ops[2].key_rate_scale = 0.5f;
  ep.fm.ops[3].ratio = 14.0f;  // tine "ping"
  ep.fm.ops[3].level = 1.2f;
  ep.fm.ops[3].env = fallback_env(1.0f, 120.0f, 0.0f, 80.0f);
  ep.fm.ops[3].vel_to_level = 0.8f;
  ep.fm.ops[3].key_rate_scale = 0.6f;
  ep.gain = 0.6f;

  // FM clavi / harpsichord: bright high-ratio pluck with a fast index decay.
  NativeSynthPatch& cl = o.clav;
  cl.mode = SynthEngineMode::kFm;
  cl.amp_env = fallback_env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.algorithm = FmAlgorithm::kStack2;
  cl.fm.ops[0].ratio = 1.0f;
  cl.fm.ops[0].level = 1.0f;
  cl.fm.ops[0].env = fallback_env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.ops[0].key_rate_scale = 0.4f;
  cl.fm.ops[1].ratio = 7.0f;
  cl.fm.ops[1].level = 2.0f;
  cl.fm.ops[1].env = fallback_env(1.0f, 150.0f, 0.0f, 100.0f);
  cl.fm.ops[1].vel_to_level = 0.6f;
  cl.fm.ops[1].key_rate_scale = 0.5f;
  cl.gain = 0.6f;

  // Modal mallets: the realism is the mode-ratio data — uniform bar
  // (glockenspiel) 1:2.756:5.404:8.933 vs deep-arch tuned bar (marimba /
  // vibraphone) 1:4:10. All ring as one-shot-ish bars gated by note-off.
  NativeSynthPatch bar{};
  bar.mode = SynthEngineMode::kModal;
  bar.amp_env = fallback_env(0.5f, 0.0f, 1.0f, 350.0f);
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

  // Celesta (GM 8): a soft felt-hammered steel bar over a wooden resonator —
  // the glockenspiel's uniform-bar ratios but rung shorter and softer, with a
  // touch of the resonator box.
  NativeSynthPatch& ce = o.celesta;
  ce = bar;
  ce.modal.num_modes = 4;
  ce.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  ce.modal.modes[1] = {2.756f, 0.55f, 0.6f};
  ce.modal.modes[2] = {5.404f, 0.28f, 0.4f};
  ce.modal.modes[3] = {8.933f, 0.14f, 0.3f};
  ce.modal.decay_s = 1.5f;
  ce.modal.decay_stretch = 0.35f;
  ce.modal.strike_brightness = 0.6f;
  ce.amp_env.release_ms = 500.0f;
  ce.body = BodyType::kWoodTube;
  ce.body_mix = 0.2f;
  ce.gain = 0.6f;

  // Music Box (GM 10): a plucked steel comb tooth — a stiff cantilever bar
  // (clamped-free 1 : 6.27 : 17.5 inharmonic series) with the tine "shimmer".
  // detune_cents is inert on a modal voice (the unison oscillators only run in
  // the subtractive engine), so the twin-tooth beat is voiced by duplicating a
  // few modes a handful of cents apart (mode pairs a few 1/1000 apart beat).
  NativeSynthPatch& mb = o.music_box;
  mb = bar;
  mb.modal.num_modes = 5;
  mb.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  mb.modal.modes[1] = {1.004f, 0.85f, 1.0f};  // twin tooth, ~7 cents -> beat
  mb.modal.modes[2] = {6.267f, 0.45f, 0.7f};
  mb.modal.modes[3] = {6.29f, 0.38f, 0.7f};  // second twin
  mb.modal.modes[4] = {17.5f, 0.16f, 0.45f};
  mb.modal.decay_s = 1.5f;
  mb.modal.decay_stretch = 0.3f;
  mb.modal.strike_brightness = 0.8f;
  mb.amp_env.release_ms = 700.0f;
  mb.gain = 0.55f;

  // Tubular Bells (GM 14): a long-ringing struck bell. The perceived "strike
  // pitch" is a missing fundamental an octave below the hum, approximated by a
  // sub-unity hum mode. A struck bell keeps ringing after the mallet leaves the
  // key, so the note-off damp is loosened (a long release_damp_s) and the amp
  // release is long — a glockenspiel's short damper would choke the bell.
  NativeSynthPatch& tb = o.tubular_bells;
  tb = bar;
  tb.modal.num_modes = 5;
  tb.modal.modes[0] = {0.5f, 0.4f, 1.2f};  // hum (missing-fundamental strike pitch)
  tb.modal.modes[1] = {1.0f, 1.0f, 1.0f};
  tb.modal.modes[2] = {2.76f, 0.7f, 0.9f};
  tb.modal.modes[3] = {5.4f, 0.4f, 0.7f};
  tb.modal.modes[4] = {8.9f, 0.25f, 0.5f};
  tb.modal.decay_s = 9.0f;
  tb.modal.decay_stretch = 0.5f;
  tb.modal.strike_brightness = 0.7f;
  tb.modal.release_damp_s = 8.0f;  // the bell rings on after note-off
  tb.amp_env.sustain = 1.0f;
  tb.amp_env.release_ms = 6000.0f;
  tb.gain = 0.6f;

  // Dulcimer (GM 15): a hammered string — physically a struck (not plucked)
  // steel string, so the Karplus-Strong core with a hard hammer excitation,
  // paired-string beat and a medium ring fits better than a modal bar.
  NativeSynthPatch& du = o.dulcimer;
  du.mode = SynthEngineMode::kKarplusStrong;
  du.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 250.0f);
  du.cutoff_hz = 20000.0f;
  du.ks.brightness = 0.7f;
  du.ks.decay_s = 2.0f;
  du.ks.decay_stretch = 0.5f;
  du.ks.pick_position = 0.2f;
  du.ks.exc_brightness = 0.85f;
  du.ks.vel_to_brightness = 0.6f;
  du.ks.release_damp_s = 0.08f;
  du.ks.nail = 0.7f;          // hard hammer edge
  du.ks.pluck_style = 0.6f;   // deterministic struck excitation
  du.ks.dispersion = 0.3f;    // steel stiffness
  du.ks.polarization = 0.3f;  // course of two/three strings beat
  du.body = BodyType::kGuitar;
  du.body_mix = 0.3f;
  du.gain = 1.3f;

  // KS guitar variants: all share the family-3 steel string and differ in
  // pick position / loop brightness / decay (the Jaffe-Smith knobs).
  NativeSynthPatch steel{};
  steel.mode = SynthEngineMode::kKarplusStrong;
  steel.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 250.0f);
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
  // attack bend, and the sympathetic open-string halo. The halo rings in both
  // hosts: the standalone NativeSynth's own resonator bank, and the GM fallback
  // path where Sf2Player drives a per-part shared sympathetic bank fed by the
  // summed dry signal and folded back centre-panned (see Sf2Player::process).
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
  bass.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 200.0f);
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
  o.harp.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 1200.0f);
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

  // Sitar (GM 104): a plucked string over the curved jawari bridge — the
  // grazing bridge contact keeps spraying energy into the upper partials, so
  // the note shimmers and buzzes for its whole long ring.
  NativeSynthPatch& si = o.sitar;
  si.mode = SynthEngineMode::kPluckedString;
  si.amp_env = fallback_env(0.0f, 0.0f, 1.0f, 200.0f);
  si.cutoff_hz = 20000.0f;
  si.plucked_string.buzz = 0.55f;
  si.plucked_string.brightness = 0.85f;
  si.plucked_string.decay_s = 3.5f;
  si.plucked_string.pick_position = 0.20f;
  si.gain = 0.8f;

  // Shamisen (GM 106): the sawari — only a slight graze against the bare wood
  // at the nut — gives a drier, harder buzz than the sitar, over a shorter
  // ring and a hard bachi strike.
  o.shamisen = o.sitar;
  o.shamisen.plucked_string.buzz = 0.5f;
  o.shamisen.plucked_string.brightness = 0.8f;
  o.shamisen.plucked_string.decay_s = 2.0f;
  o.shamisen.plucked_string.pick_position = 0.24f;
  o.shamisen.gain = 0.8f;

  // Koto (GM 107): long zither strings over movable bridges — mostly a clean
  // plucked ring with just a whisper of bridge buzz.
  o.koto = o.sitar;
  o.koto.plucked_string.buzz = 0.3f;
  o.koto.plucked_string.brightness = 0.8f;
  o.koto.plucked_string.decay_s = 3.0f;
  o.koto.plucked_string.pick_position = 0.22f;
  o.koto.gain = 0.85f;

  // Kalimba (GM 108): a plucked steel tine over a wooden box. A tine is a
  // clamped-free cantilever, so its partials are the 1 : 6.27 : 17.5 series the
  // music box's comb tooth already voices and not a harmonic one — which is why
  // this is a bar rather than a string. The differences from the comb are a
  // longer softer tine plucked with the thumb, a much shorter ring, and a box
  // that is most of what a listener actually hears.
  NativeSynthPatch& ka = o.kalimba;
  ka = bar;
  ka.modal.num_modes = 3;
  ka.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  ka.modal.modes[1] = {6.267f, 0.3f, 0.5f};
  ka.modal.modes[2] = {17.55f, 0.1f, 0.3f};
  ka.modal.decay_s = 1.1f;
  ka.modal.decay_stretch = 0.4f;
  ka.modal.strike_brightness = 0.55f;  // a thumbnail, not a mallet
  ka.modal.release_damp_s = 0.5f;      // the tine rings on after the thumb leaves
  ka.amp_env.release_ms = 400.0f;
  ka.body = BodyType::kWoodTube;
  ka.body_mix = 0.45f;

  // Harpsichord (GM 6): the jack-and-plectrum engine, voiced as a single 8'
  // choir — one string per key at written pitch. Decay and stretch are regressed
  // on the captured reference's sustained slope over eleven notes (the first
  // second is the loop filter, not the string); the damper is plausible rather
  // than measured, since neither reference could settle it.
  // damping_ref_hz / pluck_8a / plectrum_edge move as one lever: both move the
  // partial stack and the centroid, which a fresh quill at the nut left 21 dB and
  // 92 % over both references. Set together they land inside the reference spread.
  // The comb these cut is real and stays: all three references have one, at the
  // fourth or the fifth partial rather than nowhere. Flattening it to reach the
  // reference's fourth partial gives the voice an even harmonic series that reads
  // as synthetic, and the grid loss does not see the difference.
  o.harpsichord.mode = SynthEngineMode::kHarpsichord;
  o.harpsichord.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 250.0f);
  o.harpsichord.cutoff_hz = 20000.0f;
  o.harpsichord.harpsichord.eight_a = true;
  o.harpsichord.harpsichord.decay_s = 11.6f;
  o.harpsichord.harpsichord.decay_stretch = 0.40f;
  o.harpsichord.harpsichord.hf_damping = 0.45f;
  o.harpsichord.harpsichord.damping_ref_hz = 1600.0f;
  o.harpsichord.harpsichord.pluck_8a = 0.25f;
  o.harpsichord.harpsichord.plectrum_edge = 0.3f;  // a Delrin tongue, part worn
  o.harpsichord.harpsichord.velocity_range_db = 5.0f;
  o.harpsichord.harpsichord.rear_segment_mm = 90.0f;  // the undamped halo
  o.harpsichord.harpsichord.rear_coupling = 0.3f;
  // Inheriting the speaking string's t60 rang this fixed-pitch segment for 22 s
  // at F2, putting late 800-6000 Hz residue 41 dB over the reference; shortening
  // it recovers 28.6 of that and drops the recurrence term from 2.75 to 1.86.
  // The tail is flat from 0.4 s to 2.5 s, so the value is the physics: a short
  // length loaded by the bridge does not ring for a second.
  o.harpsichord.harpsichord.rear_decay_s = 0.6f;
  o.harpsichord.harpsichord.pluck_noise = 0.15f;  // the plectrum leaving the string
  o.harpsichord.harpsichord.damper_s = 0.12f;
  // The board's diffuse field, set on the phrase set rather than the note grid:
  // five of its seven takes land inside the references' own span here, against
  // none before. The top octave still wants it 13 dB louder than a single note
  // does, so this is not yet a register-independent level.
  // Where the board gives out. All three real-instrument slots put FF's
  // fundamental 27 to 30 dB under its own strongest partial and its octave-and-a
  // -fifth's on top, so the wall is between them; 75 Hz takes the bottom three
  // notes' picture from 13.13 to 12.80 and their off-partial energy from 2.67 to
  // 1.41, and leaves the middle and top of the compass where they were. The
  // general-MIDI slot has no such wall and cannot adjudicate this.
  o.harpsichord.harpsichord.board_radiating_from_hz = 75.0f;
  // The board's radiation, without which the voice puts the bare bridge force in
  // the room: at FF the references hold partials 3-10 level with the fundamental
  // and the bridge force alone puts them 13 dB under it. Against the baroque
  // slot this lands the centroid at 0.7 of the references' own spread, from 2.5
  // with no board; both 3.5 and 6 dB/oct sit outside it, one either way.
  o.harpsichord.harpsichord.board_tilt_db_oct = 5.0f;
  // What the board radiates that is not a partial. It moves the tone-to-noise
  // and nothing else — the partial stack and the centroid do not shift over the
  // whole 90 dB of its range — and this is where that lands at the references'
  // own spread, from 7.1 times it with the field off.
  o.harpsichord.harpsichord.board_diffuse_db = -34.0f;
  o.harpsichord.body = BodyType::kGuitar;
  o.harpsichord.body_mix = 0.3f;
  o.harpsichord.gain = 0.30f;

  // GS/GM2 harpsichord registration variations (program 6, bank select). Names
  // follow the GM2 melodic variation table (program_map.cpp): bank 1 octave mix,
  // bank 2 wide, bank 3 with key off. Each derives from the bank-0 8' voice, and
  // each draws a stop rather than colouring one.
  // Bank 1 — octave mix: the 4' choir joins the 8', the brightest and fullest
  // registration. Its top octave carries no dampers on a real instrument.
  o.harpsichord_octave = o.harpsichord;
  o.harpsichord_octave.harpsichord.four = true;
  o.harpsichord_octave.harpsichord.undamped_from_note = 84;
  // Bank 2 — wide: both 8' choirs drawn and spread across the stereo field. Two
  // unisons tuned by ear are what makes the chorus; one string cannot.
  o.harpsichord_wide = o.harpsichord;
  o.harpsichord_wide.harpsichord.eight_b = true;
  o.harpsichord_wide.stereo_spread = 0.5f;
  // Bank 3 — with key off: the jack dropping back and the damper landing.
  o.harpsichord_keyoff = o.harpsichord;
  o.harpsichord_keyoff.harpsichord.jack_noise = 0.5f;

  // Drawbar Organ (GM 16): the tonewheel generator at 88 8000 000, the base
  // registration. A patch of its own rather than the organ family's, which is
  // what makes the registration addressable and gives 18 something to differ
  // from; the values are the family's, moved rather than changed.
  NativeSynthPatch& dr = o.drawbar_organ;
  dr.mode = SynthEngineMode::kAdditive;
  dr.amp_env = fallback_env(2.0f, 0.0f, 1.0f, 60.0f);
  dr.cutoff_hz = 20000.0f;
  dr.additive.drawbars = {8.0f, 8.0f, 8.0f, 4.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f};
  dr.additive.key_click = 0.4f;
  dr.stereo_spread = 0.2f;
  dr.gain = 0.7f;

  // Percussive Organ (GM 17): the same generator with the percussion tab down.
  // The registration goes back to the bare first three drawbars because that is
  // what the percussion is played against — it borrows the top wheel, so a
  // drawn upper register would fight it. A reference drawbar organ settles both
  // of the remaining choices: its third harmonic stands 33 dB over its own
  // sustain and is spent in 200 ms while the second never leaves the floor, and
  // the transient's time constant measures 45 ms across two takes. The level
  // stays heard — the reference draws a different registration, so its
  // percussion-against-fundamental does not carry across.
  o.percussive_organ = dr;
  o.percussive_organ.additive.drawbars = {8.0f, 8.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  o.percussive_organ.additive.percussion_harmonic = 3;
  o.percussive_organ.additive.percussion_decay_ms = 45.0f;
  o.percussive_organ.additive.percussion_level = 0.6f;

  // Rock Organ (GM 18): the same generator drawn fuller and struck harder — the
  // upper drawbars the base registration leaves in, and the key click that comes
  // with playing them. What separates a rock organ from a jazz one on the real
  // instrument is also the amplifier it is driven through, and that is NOT set
  // here: any drive over zero steps straight to tanh(x)/tanh(1), which on this
  // registration buys 1.7 dB of level for 0.5 dB of crest — a step, not a
  // gradient, so it is a listening decision rather than a fitted one.
  o.rock_organ = dr;
  o.rock_organ.additive.drawbars = {8.0f, 8.0f, 8.0f, 6.0f, 4.0f, 4.0f, 2.0f, 2.0f, 6.0f};
  o.rock_organ.additive.key_click = 0.6f;
  o.rock_organ.gain = dr.gain * 0.85f;  // nine drawbars sounding, not six

  // Church organ: a principal chorus of self-oscillating jet flue pipes. Each
  // rank locks its pitch and holds a solid, endless tone while keyed (no decay,
  // no breath-noise wander); the amp envelope just gates the wind on and off.
  o.church_organ.mode = SynthEngineMode::kPipeOrgan;
  o.church_organ.amp_env = fallback_env(120.0f, 0.0f, 1.0f, 380.0f);
  o.church_organ.cutoff_hz = 20000.0f;
  o.church_organ.pipe_organ.tone_decay_s = 8.0f;
  o.church_organ.pipe_organ.breath = 0.549198f;
  o.church_organ.pipe_organ.chiff = 0.38f;
  o.church_organ.pipe_organ.release_damp_s = 0.75f;
  // GM Church Organ: a principal chorus (plenum) — 16' stopped sub for gravity
  // under an 8'+4'+2-2/3'+2' open principal chorus, the upperwork brighter.
  // The upperwork (smaller pipes) radiates more brightly into the room than the
  // wide bass ranks: radiation rises rank by rank, the 16' bourdon staying dark.
  // Levels and brightnesses are fitted against a measured chorus rather than
  // chosen: the upperwork carries most of a plenum's sound and voicing it by ear
  // had left the partial stack 11 dB short of the reference and widening to
  // 21 dB by C7. The 2-2/3' quint sits at its clamp maximum, which is the model
  // saying it cannot supply that rank's share any other way.
  o.church_organ.pipe_organ.rank_count = 6;
  o.church_organ.pipe_organ.ranks[0] = {0.5f, /*stopped=*/true, 0.232389f, 0.42f, 0.0f,
                                        0.0f};                                      // 16'
  o.church_organ.pipe_organ.ranks[1] = {1.0f, false, 0.841979f, 1.0f, 0.0f, 0.3f};  // 8' principal
  o.church_organ.pipe_organ.ranks[2] = {2.0f,      false, 0.636014f,
                                        0.978447f, 0.0f,  0.45f};                    // 4' octave
  o.church_organ.pipe_organ.ranks[3] = {3.0f, false, 0.476471f, 1.0f, 0.0f, 0.55f};  // 2-2/3' quint
  o.church_organ.pipe_organ.ranks[4] = {4.0f,      false, 0.712067f,
                                        0.918024f, 0.0f,  0.6f};  // 2' super-octave
  o.church_organ.pipe_organ.ranks[5] = {5.0f,      false, 0.369863f,
                                        0.687749f, 0.0f,  0.6f};  // 1-3/5' tierce
  o.church_organ.pipe_organ.brightness = 0.417552f;
  // Treble regulation: thin the upperwork (4'/quint/2'/tierce) toward the treble
  // so the plenum does not turn shrill above C4, while the bass and mid compass
  // keep the full chorus. Over C4 and up, 0.5 regulated far harder than the
  // sampled references: it left the partial stack 1.4x and the brightness 1.2x
  // their spread, where 0.175 leaves them at 1.0x and 0.6x for the same tuning.
  o.church_organ.pipe_organ.keytrack = 0.175078f;
  // A touch of wind sag so a full chord breathes, and a gentle tremulant — the
  // slow pressure undulation that keeps a sustained chord alive rather than
  // frozen (heard as faint sidebands around every partial).
  o.church_organ.pipe_organ.wind_sag = 0.25f;
  o.church_organ.pipe_organ.tremulant_rate_hz = 4.8f;
  o.church_organ.pipe_organ.tremulant_depth = 0.18f;
  o.church_organ.stereo_spread = 0.55f;
  o.church_organ.gain = 0.45f;

  // Reed Organ (GM 20) + Accordion (GM 21): a true free reed — the metal
  // tongue swings through its slot under steady bellows pressure. Harmonium
  // colour: a mellow plate, soft tongues, and a slow bellows take-up with just
  // a hint of wet-tuned beating.
  o.reed_organ.mode = SynthEngineMode::kFreeReed;
  o.reed_organ.amp_env = fallback_env(30.0f, 0.0f, 1.0f, 120.0f);
  o.reed_organ.cutoff_hz = 20000.0f;
  o.reed_organ.free_reed.brightness = 0.50f;
  o.reed_organ.free_reed.reed_stiffness = 0.40f;
  o.reed_organ.free_reed.detune = 0.12f;
  o.reed_organ.free_reed.breath_pressure = 0.7f;
  o.reed_organ.free_reed.attack_ms = 30.0f;
  o.reed_organ.free_reed.release_ms = 120.0f;
  o.reed_organ.stereo_spread = 0.18f;
  o.reed_organ.gain = 0.42f;

  // Harmonica (GM 22): a small, bright free reed right at the mouth — stiff
  // little tongues speak fast with a buzzy edge, and the player's cupping
  // hands add a gentle vibrato.
  o.harmonica = o.reed_organ;
  o.harmonica.amp_env = fallback_env(12.0f, 0.0f, 1.0f, 90.0f);
  o.harmonica.free_reed.brightness = 0.78f;
  o.harmonica.free_reed.reed_stiffness = 0.65f;
  o.harmonica.free_reed.detune = 0.15f;
  o.harmonica.free_reed.attack_ms = 12.0f;
  o.harmonica.free_reed.release_ms = 90.0f;
  o.harmonica.lfo_rate_hz = 5.6f;
  o.harmonica.lfo_to_pitch_cents = 8.0f;
  o.harmonica.stereo_spread = 0.12f;
  o.harmonica.gain = 0.44f;

  // Bandoneon (GM 23): the tango free reed. The defining trait is the musette
  // voicing — two near-unison tongues a few cents apart beat against each
  // other, producing the characteristic wet shimmer.
  o.bandoneon = o.reed_organ;
  o.bandoneon.amp_env = fallback_env(24.0f, 0.0f, 1.0f, 120.0f);
  o.bandoneon.free_reed.brightness = 0.55f;
  o.bandoneon.free_reed.reed_stiffness = 0.45f;
  o.bandoneon.free_reed.detune = 0.30f;
  o.bandoneon.stereo_spread = 0.22f;
  o.bandoneon.gain = 0.44f;

  // Orchestra Hit (GM 55): a sharp tutti stab. The ensemble family's slow pad
  // is the wrong envelope, so this overrides to a bright detuned-saw chord with
  // a fast attack and a snappy filter decay — a synthetic stab, not a section.
  NativeSynthPatch& oh = o.orchestra_hit;
  oh.waveform = VaWaveform::kSaw;
  oh.unison = 5;
  oh.detune_cents = 16.0f;
  oh.drift_cents = 3.0f;
  oh.amp_env = fallback_env(2.0f, 320.0f, 0.0f, 180.0f);
  oh.cutoff_hz = 5200.0f;
  oh.filter_env = fallback_env(1.0f, 220.0f, 0.0f, 180.0f);
  oh.env_to_cutoff_cents = 2400.0f;
  oh.key_track = 0.4f;
  oh.vel_to_cutoff_cents = 1500.0f;
  oh.stereo_spread = 0.5f;
  oh.gain = 0.7f;

  // Tremolo Strings (GM 44): the string-section pad under a measured-bow
  // amplitude tremolo (LFO2 -> amp) — the section shudders rather than
  // re-attacking per stroke.
  NativeSynthPatch& trem = o.tremolo_strings;
  trem.waveform = VaWaveform::kSaw;
  trem.unison = 4;
  trem.detune_cents = 12.0f;
  trem.drift_cents = 4.0f;
  trem.amp_env = fallback_env(60.0f, 300.0f, 0.85f, 300.0f);
  trem.cutoff_hz = 3800.0f;
  trem.key_track = 0.3f;
  trem.lfo2_rate_hz = 9.0f;
  trem.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.45f};
  trem.stereo_spread = 0.5f;

  // Pizzicato Strings (GM 45): a plucked violin-family string — short KS ring
  // into the violin corpus, mid-string finger pluck.
  NativeSynthPatch& pz = o.pizzicato;
  pz.mode = SynthEngineMode::kKarplusStrong;
  pz.amp_env = fallback_env(1.0f, 0.0f, 1.0f, 200.0f);
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
  tp.amp_env = fallback_env(0.5f, 1800.0f, 0.0f, 500.0f);
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

  // Choir Aahs (GM 52): a glottal source sung through the open /a/ formant
  // bank — the vowel tract is what separates "aah" from a string pad. The
  // section swells in slowly with a singer's own vibrato.
  NativeSynthPatch& ch = o.choir_aahs;
  ch.mode = SynthEngineMode::kVocal;
  ch.amp_env = fallback_env(200.0f, 400.0f, 0.9f, 400.0f);
  ch.cutoff_hz = 20000.0f;
  ch.vocal.vowel = 0;  // /a/
  ch.vocal.brightness = 0.55f;
  ch.vocal.vibrato_depth = 0.30f;
  ch.vocal.breath_noise = 0.12f;
  ch.vocal.attack_ms = 200.0f;
  ch.vocal.release_ms = 400.0f;
  ch.stereo_spread = 0.6f;
  ch.gain = 0.6f;

  // Voice Oohs (GM 53): the same voice with a nearly closed mouth — the /u/
  // vowel, darker and more covered.
  o.voice_oohs = ch;
  o.voice_oohs.vocal.vowel = 4;  // /u/
  o.voice_oohs.vocal.brightness = 0.42f;
  o.voice_oohs.vocal.vibrato_depth = 0.25f;
  o.voice_oohs.gain = 0.65f;

  // Synth Voice (GM 54): a brighter, steadier synthetic vowel — the forward
  // /i/ with a quicker swell and less vibrato wobble.
  o.synth_voice = ch;
  o.synth_voice.vocal.vowel = 2;  // /i/
  o.synth_voice.vocal.brightness = 0.62f;
  o.synth_voice.vocal.vibrato_depth = 0.15f;
  o.synth_voice.vocal.attack_ms = 120.0f;
  o.synth_voice.vocal.release_ms = 300.0f;
  o.synth_voice.amp_env = fallback_env(120.0f, 300.0f, 0.9f, 300.0f);
}

}  // namespace sonare::midi::synth::detail
