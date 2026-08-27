#pragma once

#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

/// GM 80-95, the synth leads and pads. Sixteen programs that shared two family
/// patches: subtractive is the right engine for all of them, so what was
/// missing was not a model but a voice each. Each one is voiced from what its
/// GM name names — the waveform for `square` and `sawtooth`, the attack
/// transient for `chiff`, the resonant formant bank for `voice` and `choir`,
/// the modulation for `sweep` — rather than by taste, because the names are the
/// only specification these programs have.
///
/// The sixteen gains are levelled rather than voiced: a synthetic program has
/// no physical output level, and with each patch's filter and stack chosen
/// independently the block spanned 9.6 dB before they were set. Measured on a
/// held C4 at velocity 100 they now sit inside 2.2 dB, leads a little over
/// pads. That says nothing about the rest of the bank, which spans 22 dB on the
/// same note and is its own question.
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
}

}  // namespace sonare::midi::synth::detail
