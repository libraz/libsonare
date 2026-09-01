#pragma once

#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gm_fallback_families.h"

namespace sonare::midi::synth::detail {

/// GM 80-103, the synth leads, pads and effects. Twenty-four programs that shared
/// patches: subtractive is the right engine for all of them, so what was
/// missing was not a model but a voice each. Each one is voiced from what its
/// GM name names — the waveform for `square` and `sawtooth`, the attack
/// transient for `chiff`, the resonant formant bank for `voice` and `choir`,
/// the modulation for `sweep`, and noise/filter motion for the effects — rather
/// than by taste, because the names are the only specification these programs
/// have. The effects have no sample or delay line in this engine, so the patches
/// use the closest oscillator, envelope and filter gesture available.
///
/// The lead/pad gains are levelled rather than voiced: a synthetic program has
/// no physical output level, and with each patch's filter and stack chosen
/// independently the block spanned 9.6 dB before they were set. Measured on a
/// held C4 at velocity 100 they now sit inside 2.2 dB, leads a little over pads.
/// That says nothing about the rest of the bank, which spans 22 dB on the same
/// note and is its own question.
///
/// Two of them cannot be finished here. `fifths` and `bass + lead` are layers
/// at a fixed interval, and the oscillator section has no interval: unison
/// positions are a symmetric spread with an anti-phase-lock jitter sized as a
/// tenth of it, so at a fifth's width the jitter alone is 70 cents out. Both
/// are voiced as far as the engine reaches and named for what they are missing.
constexpr void configure_synth_programs(ProgramOverrides& o) noexcept {
  // The shared lead: a detuned saw stack through the ladder, fast enough to
  // play a line with, its filter envelope carrying the attack.
  NativeSynthPatch lead{};
  lead.waveform = VaWaveform::kSaw;
  lead.unison = 3;
  lead.detune_cents = 12.0f;
  lead.filter_model = SynthFilterModel::kMoogLadder;
  lead.drive = 0.1f;
  lead.amp_env = fallback_env(5.0f, 200.0f, 0.8f, 150.0f);
  lead.cutoff_hz = 3500.0f;
  lead.filter_env = fallback_env(1.0f, 350.0f, 0.4f, 150.0f);
  lead.env_to_cutoff_cents = 1800.0f;
  lead.vel_to_cutoff_cents = 1200.0f;
  lead.gain = 0.5f;

  // Square (GM 80): the name is the oscillator. A hollow square needs no stack
  // to be heard, so the unison drops to two and the mod wheel carries the
  // vibrato a lead player reaches for rather than it running all the time.
  o.lead_square = lead;
  o.lead_square.waveform = VaWaveform::kSquare;
  o.lead_square.unison = 2;
  o.lead_square.detune_cents = 8.0f;
  o.lead_square.cutoff_hz = 4200.0f;
  o.lead_square.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kPitchCents, 35.0f};
  o.lead_square.gain = 0.60f;

  // Sawtooth (GM 81): the shared lead itself, opened up — the saw is what the
  // stack was written around.
  o.lead_saw = lead;
  o.lead_saw.drive = 0.2f;
  o.lead_saw.cutoff_hz = 4000.0f;
  o.lead_saw.gain = 0.61f;

  // Calliope (GM 82): a steam whistle rank. One pure triangle in a tube, no
  // stack and no drive, with the deep vibrato a whistle organ has from its
  // wind supply rather than from a player.
  o.lead_calliope = lead;
  o.lead_calliope.waveform = VaWaveform::kTriangle;
  o.lead_calliope.unison = 1;
  o.lead_calliope.detune_cents = 0.0f;
  o.lead_calliope.drive = 0.0f;
  o.lead_calliope.cutoff_hz = 2600.0f;
  o.lead_calliope.env_to_cutoff_cents = 600.0f;
  o.lead_calliope.amp_env = fallback_env(35.0f, 200.0f, 0.9f, 180.0f);
  o.lead_calliope.lfo_rate_hz = 6.0f;
  o.lead_calliope.lfo_to_pitch_cents = 22.0f;
  o.lead_calliope.body = BodyType::kWoodTube;
  o.lead_calliope.body_mix = 0.25f;
  o.lead_calliope.gain = 0.59f;

  // Chiff (GM 83): the attack transient is the whole program. A wide filter
  // envelope that collapses in 60 ms puts the breath edge on the onset and
  // leaves the held note dull, which is what "chiff" means on a flue pipe. The
  // saw stays because a filter can only take away: on a triangle there is
  // nothing above the third partial for the envelope to open onto, and the
  // chiff is silent.
  o.lead_chiff = lead;
  o.lead_chiff.unison = 2;
  o.lead_chiff.detune_cents = 6.0f;
  o.lead_chiff.cutoff_hz = 1800.0f;
  o.lead_chiff.filter_env = fallback_env(1.0f, 70.0f, 0.12f, 120.0f);
  // The edge has to still be open when the amplitude arrives: the filter is
  // through its attack in 1 ms and the amp envelope is not, so without the hold
  // the brightest moment is spent before anything is loud enough to hear it.
  o.lead_chiff.filter_env.hold_ms = 10.0f;
  o.lead_chiff.env_to_cutoff_cents = 3900.0f;
  o.lead_chiff.resonance_q = 1.8f;
  o.lead_chiff.gain = 0.62f;

  // Charang (GM 84): the hard, guitar-edged lead. The diode ladder saturates
  // asymmetrically, so the drive is where the character sits and the cutoff
  // comes down under it.
  o.lead_charang = lead;
  o.lead_charang.unison = 2;
  o.lead_charang.detune_cents = 6.0f;
  o.lead_charang.filter_model = SynthFilterModel::kDiodeLadder;
  o.lead_charang.drive = 0.55f;
  o.lead_charang.resonance_q = 2.0f;
  o.lead_charang.cutoff_hz = 2600.0f;
  o.lead_charang.gain = 0.96f;

  // Voice (GM 85): a sung lead. The vocal formant bank is the model; the
  // oscillator behind it only has to be rich enough to feed the formants.
  o.lead_voice = lead;
  o.lead_voice.unison = 2;
  o.lead_voice.detune_cents = 9.0f;
  o.lead_voice.cutoff_hz = 2200.0f;
  o.lead_voice.amp_env = fallback_env(45.0f, 250.0f, 0.85f, 220.0f);
  o.lead_voice.lfo_rate_hz = 5.2f;
  o.lead_voice.lfo_to_pitch_cents = 14.0f;
  o.lead_voice.body = BodyType::kVocal;
  o.lead_voice.body_mix = 0.6f;
  o.lead_voice.gain = 0.61f;

  // Fifths (GM 86): the interval it is named for is not reachable — see the
  // file header. Voiced instead as the hollow, hard-detuned stack the program
  // is used for, so it is at least not a second sawtooth lead.
  o.lead_fifths = lead;
  o.lead_fifths.waveform = VaWaveform::kSquare;
  o.lead_fifths.unison = 5;
  o.lead_fifths.detune_cents = 28.0f;
  o.lead_fifths.cutoff_hz = 3000.0f;
  o.lead_fifths.resonance_q = 1.6f;
  o.lead_fifths.stereo_spread = 0.35f;
  o.lead_fifths.gain = 0.96f;

  // Bass + lead (GM 87): likewise a layer, and likewise not reachable. What is
  // reachable is the weight — a low cutoff with the drive up puts the energy
  // under the line instead of over it.
  o.lead_bass_lead = lead;
  o.lead_bass_lead.unison = 4;
  o.lead_bass_lead.detune_cents = 16.0f;
  o.lead_bass_lead.cutoff_hz = 1600.0f;
  o.lead_bass_lead.drive = 0.35f;
  o.lead_bass_lead.env_to_cutoff_cents = 2400.0f;
  o.lead_bass_lead.key_track = 0.4f;
  o.lead_bass_lead.gain = 0.63f;

  // The shared pad: a supersaw that arrives slowly, drifts, and spreads.
  NativeSynthPatch pad{};
  pad.waveform = VaWaveform::kSaw;
  pad.unison = 7;
  pad.detune_cents = 18.0f;
  pad.drift_cents = 5.0f;
  pad.amp_env = fallback_env(400.0f, 600.0f, 0.8f, 800.0f);
  pad.cutoff_hz = 2800.0f;
  pad.stereo_spread = 0.6f;
  pad.gain = 0.5f;

  // New age (GM 88): the bell-lit pad. A slow second LFO on the cutoff is the
  // shimmer; the stack thins so the top stays clear enough to hear it.
  o.pad_new_age = pad;
  o.pad_new_age.unison = 5;
  o.pad_new_age.cutoff_hz = 3400.0f;
  o.pad_new_age.resonance_q = 1.4f;
  o.pad_new_age.amp_env = fallback_env(350.0f, 900.0f, 0.75f, 1400.0f);
  o.pad_new_age.lfo2_rate_hz = 0.25f;
  o.pad_new_age.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kCutoffCents, 900.0f};
  o.pad_new_age.gain = 0.35f;

  // Warm (GM 89): the shared pad taken down and slowed. Nothing else — warm is
  // the absence of edge, not the presence of anything.
  o.pad_warm = pad;
  o.pad_warm.cutoff_hz = 1800.0f;
  o.pad_warm.amp_env = fallback_env(620.0f, 800.0f, 0.85f, 1100.0f);
  o.pad_warm.gain = 0.47f;

  // Polysynth (GM 90): a pad you can still play chords on, so the attack comes
  // back inside a tenth of a second and the stack is wide and bright.
  o.pad_polysynth = pad;
  o.pad_polysynth.unison = 5;
  o.pad_polysynth.detune_cents = 24.0f;
  o.pad_polysynth.cutoff_hz = 4200.0f;
  o.pad_polysynth.drive = 0.15f;
  o.pad_polysynth.amp_env = fallback_env(90.0f, 700.0f, 0.7f, 600.0f);
  o.pad_polysynth.filter_env = fallback_env(20.0f, 900.0f, 0.5f, 600.0f);
  o.pad_polysynth.env_to_cutoff_cents = 1500.0f;
  o.pad_polysynth.gain = 0.77f;

  // Choir (GM 91): the same formant bank the vocal lead uses, over the pad's
  // envelope instead of the lead's.
  o.pad_choir = pad;
  o.pad_choir.unison = 5;
  o.pad_choir.detune_cents = 14.0f;
  o.pad_choir.cutoff_hz = 2400.0f;
  o.pad_choir.amp_env = fallback_env(500.0f, 700.0f, 0.85f, 900.0f);
  o.pad_choir.body = BodyType::kVocal;
  o.pad_choir.body_mix = 0.7f;
  o.pad_choir.gain = 0.48f;

  // Bowed (GM 92): bowed glass. The swell is longer than any of the others and
  // the corpus resonance gives it the wooden body a bow implies; the model is
  // not the bowed-string waveguide, which belongs to an instrument that has
  // one, so this stays a pad wearing a bow's envelope.
  o.pad_bowed = pad;
  o.pad_bowed.unison = 5;
  o.pad_bowed.detune_cents = 12.0f;
  o.pad_bowed.cutoff_hz = 3000.0f;
  o.pad_bowed.resonance_q = 2.5f;
  o.pad_bowed.amp_env = fallback_env(900.0f, 900.0f, 0.9f, 1200.0f);
  o.pad_bowed.body = BodyType::kViolin;
  o.pad_bowed.body_mix = 0.35f;
  o.pad_bowed.gain = 0.34f;

  // Metallic (GM 93): a narrow resonant band on a square is how a subtractive
  // synth makes metal — the state-variable filter is the only model here with
  // a bandpass output, so the metallic pad is the one that must not be on a
  // ladder.
  o.pad_metallic = pad;
  o.pad_metallic.waveform = VaWaveform::kSquare;
  o.pad_metallic.unison = 5;
  o.pad_metallic.detune_cents = 20.0f;
  o.pad_metallic.filter_model = SynthFilterModel::kSvf;
  o.pad_metallic.filter_output = SynthFilterOutput::kBandpass;
  o.pad_metallic.cutoff_hz = 2600.0f;
  o.pad_metallic.resonance_q = 6.0f;
  o.pad_metallic.key_track = 0.6f;
  o.pad_metallic.amp_env = fallback_env(300.0f, 1200.0f, 0.6f, 1000.0f);
  o.pad_metallic.gain = 0.46f;

  // Halo (GM 94): distance. The slowest attack, the widest scatter, the most
  // drift, and the top rolled off so nothing in it is close to the listener.
  o.pad_halo = pad;
  o.pad_halo.drift_cents = 9.0f;
  o.pad_halo.cutoff_hz = 2200.0f;
  o.pad_halo.amp_env = fallback_env(1200.0f, 900.0f, 0.85f, 1600.0f);
  o.pad_halo.stereo_spread = 0.85f;
  o.pad_halo.gain = 1.02f;

  // Sweep (GM 95): the modulation is the program. A slow LFO across most of
  // the filter's range, deep enough that the sweep is the thing heard and not
  // a wobble on top of a pad.
  o.pad_sweep = pad;
  o.pad_sweep.cutoff_hz = 1500.0f;
  o.pad_sweep.resonance_q = 3.0f;
  o.pad_sweep.lfo2_rate_hz = 0.3f;
  o.pad_sweep.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kCutoffCents, 2600.0f};
  o.pad_sweep.gain = 0.38f;

  // --- Synth effects (GM 96-103) -------------------------------------------
  // Sample-like GM gestures use the closest source, envelope, filter and modulation available.
  NativeSynthPatch fx{};
  fx.filter_model = SynthFilterModel::kSvf;
  fx.gain = 0.5f;

  // FX 1 Rain: a high-passed noise wash with a granular-looking decay.
  o.fx_rain = fx;
  o.fx_rain.waveform = VaWaveform::kNoise;
  o.fx_rain.amp_env = fallback_env(2.0f, 260.0f, 0.05f, 160.0f);
  o.fx_rain.filter_output = SynthFilterOutput::kHighpass;
  o.fx_rain.cutoff_hz = 6200.0f;
  o.fx_rain.resonance_q = 1.1f;
  o.fx_rain.gain = 0.68f;

  // FX 2 Soundtrack: a broad saw stack and long envelope make the score-like wash.
  o.fx_soundtrack = fx;
  o.fx_soundtrack.waveform = VaWaveform::kSaw;
  o.fx_soundtrack.unison = 5;
  o.fx_soundtrack.detune_cents = 22.0f;
  o.fx_soundtrack.drift_cents = 4.0f;
  o.fx_soundtrack.amp_env = fallback_env(700.0f, 1200.0f, 0.7f, 1400.0f);
  o.fx_soundtrack.cutoff_hz = 2400.0f;
  o.fx_soundtrack.resonance_q = 1.2f;
  o.fx_soundtrack.stereo_spread = 0.6f;
  o.fx_soundtrack.gain = 0.43f;

  // FX 3 Crystal: a narrow, high-Q triangle band makes upper partials speak first.
  o.fx_crystal = fx;
  o.fx_crystal.waveform = VaWaveform::kTriangle;
  o.fx_crystal.unison = 2;
  o.fx_crystal.detune_cents = 4.0f;
  o.fx_crystal.amp_env = fallback_env(1.0f, 1800.0f, 0.05f, 500.0f);
  o.fx_crystal.filter_output = SynthFilterOutput::kBandpass;
  o.fx_crystal.cutoff_hz = 5200.0f;
  o.fx_crystal.resonance_q = 8.0f;
  o.fx_crystal.filter_env = fallback_env(1.0f, 900.0f, 0.0f, 300.0f);
  o.fx_crystal.env_to_cutoff_cents = 1800.0f;
  o.fx_crystal.gain = 0.58f;

  // FX 4 Atmosphere: wide drift supplies the motion possible in a one-note voice.
  o.fx_atmosphere = fx;
  o.fx_atmosphere.waveform = VaWaveform::kSaw;
  o.fx_atmosphere.unison = 7;
  o.fx_atmosphere.detune_cents = 35.0f;
  o.fx_atmosphere.drift_cents = 12.0f;
  o.fx_atmosphere.amp_env = fallback_env(1200.0f, 1800.0f, 0.75f, 1800.0f);
  o.fx_atmosphere.cutoff_hz = 1300.0f;
  o.fx_atmosphere.resonance_q = 1.0f;
  o.fx_atmosphere.stereo_spread = 0.9f;
  o.fx_atmosphere.gain = 0.52f;

  // FX 5 Brightness: square-wave and high-pass edge with a little brittle drive.
  o.fx_brightness = fx;
  o.fx_brightness.waveform = VaWaveform::kSquare;
  o.fx_brightness.unison = 3;
  o.fx_brightness.detune_cents = 8.0f;
  o.fx_brightness.amp_env = fallback_env(4.0f, 550.0f, 0.35f, 260.0f);
  o.fx_brightness.filter_output = SynthFilterOutput::kHighpass;
  o.fx_brightness.cutoff_hz = 7600.0f;
  o.fx_brightness.resonance_q = 2.0f;
  o.fx_brightness.filter_env = fallback_env(1.0f, 180.0f, 0.0f, 150.0f);
  o.fx_brightness.env_to_cutoff_cents = 1200.0f;
  o.fx_brightness.drive = 0.15f;
  o.fx_brightness.gain = 0.5f;

  // FX 6 Goblins: detune, drift and seeded random pitch make a deterministic twitch.
  o.fx_goblins = fx;
  o.fx_goblins.waveform = VaWaveform::kSquare;
  o.fx_goblins.unison = 2;
  o.fx_goblins.detune_cents = 48.0f;
  o.fx_goblins.drift_cents = 8.0f;
  o.fx_goblins.amp_env = fallback_env(40.0f, 250.0f, 0.55f, 350.0f);
  o.fx_goblins.cutoff_hz = 1900.0f;
  o.fx_goblins.resonance_q = 3.0f;
  o.fx_goblins.filter_env = fallback_env(2.0f, 160.0f, 0.1f, 200.0f);
  o.fx_goblins.env_to_cutoff_cents = 2600.0f;
  o.fx_goblins.mod_matrix.routes[0] = {ModSource::kRandom, ModDestination::kPitchCents, 80.0f};
  o.fx_goblins.gain = 0.54f;

  // FX 7 Echoes: without a delay line, slow LFO gain pulses mimic repeats.
  o.fx_echoes = fx;
  o.fx_echoes.waveform = VaWaveform::kTriangle;
  o.fx_echoes.unison = 4;
  o.fx_echoes.detune_cents = 9.0f;
  o.fx_echoes.amp_env = fallback_env(8.0f, 1400.0f, 0.45f, 900.0f);
  o.fx_echoes.cutoff_hz = 4200.0f;
  o.fx_echoes.resonance_q = 1.2f;
  o.fx_echoes.lfo2_rate_hz = 1.8f;
  o.fx_echoes.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, -0.65f};
  o.fx_echoes.stereo_spread = 0.55f;
  o.fx_echoes.gain = 0.55f;

  // FX 8 Sci-Fi: an opening resonant radio band with a slow synthetic sweep.
  o.fx_sci_fi = fx;
  o.fx_sci_fi.waveform = VaWaveform::kNoise;
  o.fx_sci_fi.amp_env = fallback_env(20.0f, 1000.0f, 0.2f, 500.0f);
  o.fx_sci_fi.filter_output = SynthFilterOutput::kBandpass;
  o.fx_sci_fi.cutoff_hz = 900.0f;
  o.fx_sci_fi.resonance_q = 12.0f;
  o.fx_sci_fi.filter_env = fallback_env(1.0f, 500.0f, 0.0f, 250.0f);
  o.fx_sci_fi.env_to_cutoff_cents = 4800.0f;
  o.fx_sci_fi.lfo2_rate_hz = 1.1f;
  o.fx_sci_fi.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kCutoffCents, 800.0f};
  o.fx_sci_fi.gain = 0.6f;
}

/// GM 38-39 and 62-63, the four synth programs that sit outside 80-103. They
/// were the last programs sharing a family patch with a sibling — 38 and 39
/// rendered byte-identically, and so did 62 and 63 — and unlike the leads and
/// pads above they each have a captured reference, so the pair is separated by
/// what the two references disagree about rather than by their names.
///
/// Both basses are mono and sustained in the reference (stereo width 0.00,
/// under 0.1 dB/s under a held key), so neither takes a spread or a decay. Both
/// carry a resonant low-pass parked near the second harmonic and NOT tracking
/// the keyboard: the octave partial sits level with the fundamental at the
/// bottom of the range and 18-20 dB under it two octaves up, which a tracked
/// cutoff cannot produce. What separates them is what survives above that knee
/// — bass 1 drops everything over the octave onto one flat step, bass 2 keeps a
/// falling series four partials deep — and how the attack answers velocity.
///
/// The brasses stay on the family's FM stack, where the modulator index is what
/// a velocity-driven brightness sweep needs. What separates them is the
/// feedback operator, and that is measured: scored on third-octave shape
/// against their own references, brass 1 is best with it off (7.3 dB RMS, 17.3
/// at the family's value) and brass 2 wants it near the top of its range (6.2).
/// One value cannot serve both, which is the case for the split.
///
/// One residual on the brasses is structural rather than a value: both
/// references carry a roughly 1 Hz amplitude modulation 5 to 7 dB deep, and it
/// is what their scattered onset numbers are measuring — the envelope peak
/// lands on whichever crest is highest, 0.7 to 2.5 s in, which is why the
/// comparison reads the model as arriving 1.2 s early on a patch whose attack
/// is 40 ms. The engine has the mechanism (an LFO2 route to amp gain, as FX 7
/// uses) and no knob in the fit could reach it. It is left unbuilt because both
/// timbres come from one product and a 5 dB tremolo on every synth brass note
/// is a large thing to adopt on one opinion.
///
/// Two dimensions are out of reach here and are left as gaps rather than argued
/// away. All four references span about 12 dB from the softest velocity to the
/// hardest and the engine's own curve spans about 24, and no patch field scales
/// it. And `stereo_spread` is a per-voice pan scatter, so one held note comes
/// out panned rather than wide — its channels correlate at +1.0000 whatever the
/// setting — and the width the references measure is unreachable at the one
/// note the comparison takes. The setting is not inert, it is just invisible
/// there: four voices correlate at 0.90 against 0.96 for half the spread, and
/// the same scatter puts a single note 7.8 dB off centre against 4.9. Both
/// numbers move together, so the pair's split is the references' width ratio
/// rather than either of their values.
SONARE_TUNED_CONSTEXPR void configure_synth_bass_and_brass_programs(ProgramOverrides& o) noexcept {
  const std::array<NativeSynthPatch, 16> fam = build_family_patches();

  // The shared synth bass: one saw into a resonant low-pass that stays where it
  // is put. No unison, no drift and no drive — the references measure 40 to 54
  // dB tone-to-noise, which is cleaner than a detuned stack can be.
  NativeSynthPatch sbass = fam[4];
  sbass.waveform = VaWaveform::kSaw;
  sbass.unison = 1;
  sbass.detune_cents = 0.0f;
  sbass.drift_cents = 0.0f;
  sbass.drive = 0.0f;
  sbass.filter_model = SynthFilterModel::kSvf;
  sbass.resonance_q = 2.0f;
  sbass.key_track = 0.0f;
  sbass.filter_env = DahdsrConfig{};
  sbass.env_to_cutoff_cents = 0.0f;
  sbass.vel_to_cutoff_cents = 0.0f;
  sbass.stereo_spread = 0.0f;

  // Synth Bass 1 (GM 38): the fundamental and its octave, and a flat step for
  // everything above — h3 to h6 all sit within 6 dB of each other at every note
  // and every velocity, so there is no series above the knee to shape.
  o.synth_bass_1 = sbass;
  o.synth_bass_1.cutoff_hz = 180.925f;
  o.synth_bass_1.resonance_q = 1.23538f;
  // The reference falls 0.06 dB/s under a held key, which is nearly flat and is
  // not nothing: at a dead-flat sustain the envelope has no peak to find and the
  // comparison's onset term reads wherever the argmax lands, 2.2 s in.
  o.synth_bass_1.amp_env = fallback_env(6.68737f, 6000.0f, 0.96f, 39.706f);
  // Levelled to its sibling, not to anything outside the pair: the two came out
  // 6.6 dB apart in held RMS at E2 and are the only same-envelope comparison
  // either of them has, so each moves half of it off the family's 0.5.
  o.synth_bass_1.gain = 0.342f;

  // Synth Bass 2 (GM 39): a deeper series (h3 to h6 fall 21 dB across an
  // octave) and an attack that lengthens with velocity, which the filter
  // envelope carries since the amp attack cannot answer velocity.
  o.synth_bass_2 = sbass;
  o.synth_bass_2.filter_model = SynthFilterModel::kMoogLadder;
  o.synth_bass_2.cutoff_hz = 221.42f;
  o.synth_bass_2.resonance_q = 4.8522f;
  o.synth_bass_2.vel_to_cutoff_cents = 566.563f;
  o.synth_bass_2.env_to_cutoff_cents = 167.184f;
  o.synth_bass_2.amp_env = fallback_env(16.7184f, 0.0f, 1.0f, 36.9329f);
  o.synth_bass_2.filter_env = fallback_env(120.0f, 400.0f, 1.0f, 150.0f);
  o.synth_bass_2.gain = 0.731f;

  // Synth Brass 1 (GM 62): the family stack with the feedback operator off, and
  // the fit put it there on its own, against the range floor. It is the wider
  // of the pair (reference width 0.70 to 1.00 against 0.23 to 0.51) and the
  // shorter (damper release 50 to 75 ms), and its brightness answers velocity
  // hardest — the centroid doubles from the softest note to the hardest.
  o.synth_brass_1 = fam[7];
  o.synth_brass_1.fm.ops[2].feedback = 0.0f;
  o.synth_brass_1.fm.ops[1].level = 3.50155f;
  o.synth_brass_1.fm.ops[1].vel_to_level = 0.944272f;
  o.synth_brass_1.fm.ops[2].vel_to_level = 0.583592f;
  o.synth_brass_1.fm.ops[1].env.decay_ms = 175.915f;
  o.synth_brass_1.cutoff_hz = 16316.9f;
  o.synth_brass_1.amp_env = fallback_env(40.0f, 200.0f, 0.85f, 60.0f);
  o.synth_brass_1.stereo_spread = 0.6f;

  // Synth Brass 2 (GM 63): darker in the harmonics (h3 sits 8 to 12 dB under
  // brass 1's), narrower (width 0.23 to 0.51) and slower to let go (120 to 170
  // ms), with the feedback operator carrying its high shelf.
  o.synth_brass_2 = fam[7];
  o.synth_brass_2.fm.ops[2].feedback = 1.88854f;
  o.synth_brass_2.fm.ops[1].level = 1.16718f;
  o.synth_brass_2.fm.ops[2].level = 1.16718f;
  o.synth_brass_2.fm.ops[1].vel_to_level = 0.944272f;
  o.synth_brass_2.fm.ops[2].vel_to_level = 0.0557281f;
  o.synth_brass_2.fm.ops[1].env.decay_ms = 1547.3f;
  o.synth_brass_2.cutoff_hz = 1078.64f;
  o.synth_brass_2.amp_env = fallback_env(40.0f, 200.0f, 0.85f, 150.0f);
  o.synth_brass_2.stereo_spread = 0.3f;
}

}  // namespace sonare::midi::synth::detail
