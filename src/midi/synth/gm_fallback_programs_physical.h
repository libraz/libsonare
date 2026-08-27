#pragma once

#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

constexpr void configure_physical_programs(ProgramOverrides& o) noexcept {
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

  // Fiddle (GM 110): the same violin, bowed the other way. A short hard stroke
  // near the bridge with the rosin audible and next to no vibrato — the
  // variation is the hand, so this derives from the violin rather than
  // restating it.
  o.fiddle = o.violin;
  o.fiddle.bowed_string.bow_position = 0.10f;
  o.fiddle.bowed_string.bow_force = 0.68f;
  o.fiddle.bowed_string.brightness = 0.58f;
  o.fiddle.bowed_string.attack_ms = 22.0f;
  o.fiddle.bowed_string.rosin = 0.22f;
  o.fiddle.bowed_string.sympathetic = 0.16f;  // the open strings ring under the tune
  o.fiddle.amp_env.attack_ms = 8.0f;
  o.fiddle.cutoff_hz = 7000.0f;
  o.fiddle.lfo_to_pitch_cents = 2.0f;

  // String Ensemble 1/2 (GM 48-49): the bowed string in section. One waveguide
  // is one player, so everything the section adds has to come from the spread —
  // per-voice pitch drift, a slower and less unanimous speech, a wide image, and
  // the second polarization plane opened past the soloists', because what reads
  // as "many" is beating rather than count. Section 2 is the slower, warmer
  // half of the pair.
  o.string_ensemble_1 = bowed(0.13f, 0.52f, 0.40f, 0.36f, 140.0f, 260.0f, 0.36f, 0.32f);
  o.string_ensemble_1.amp_env.attack_ms = 120.0f;
  o.string_ensemble_1.cutoff_hz = 5000.0f;
  o.string_ensemble_1.bowed_string.polarization = 0.35f;
  o.string_ensemble_1.bowed_string.sympathetic = 0.14f;
  o.string_ensemble_1.bowed_string.rosin = 0.06f;  // twenty bows average the grit out
  o.string_ensemble_1.drift_cents = 7.0f;
  o.string_ensemble_1.stereo_spread = 0.6f;
  o.string_ensemble_1.lfo_rate_hz = 4.6f;
  o.string_ensemble_1.lfo_to_pitch_cents = 5.0f;
  o.string_ensemble_2 = o.string_ensemble_1;
  o.string_ensemble_2.amp_env.attack_ms = 260.0f;
  o.string_ensemble_2.bowed_string.attack_ms = 280.0f;
  o.string_ensemble_2.bowed_string.brightness = 0.34f;
  o.string_ensemble_2.bowed_string.damping = 0.40f;
  o.string_ensemble_2.cutoff_hz = 4000.0f;
  o.string_ensemble_2.drift_cents = 8.0f;

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

  // Bag pipe (GM 109): the chanter — a stiff double reed on a conical bore fed
  // from the bag, so the pressure never varies and the note is never tongued.
  // vel_to_breath is zero because a piper has no dynamics, not as a voicing
  // choice. The drones are not voiced: one waveguide is one pipe.
  o.bag_pipe = reed(true, 0.82f, 0.32f, 0.86f, 0.24f, 30.0f, 60.0f, 0.86f, 0.22f, 0.55f);
  o.bag_pipe.cutoff_hz = 8000.0f;
  o.bag_pipe.reed.vel_to_breath = 0.0f;
  o.bag_pipe.reed.chiff = 0.08f;
  o.bag_pipe.reed.breath_noise = 0.06f;

  // Shanai (GM 111): a double reed on a wooden cone into a flared METAL bell,
  // which is where its edge comes from and why it is the one reed here on the
  // brass body rather than the wood tube. Played with a wide expressive slide,
  // so the pitch modulation is deep next to an orchestral double reed's.
  o.shanai = reed(true, 0.86f, 0.30f, 0.82f, 0.26f, 22.0f, 80.0f, 0.72f, 0.28f, 0.58f);
  o.shanai.body = BodyType::kBrassBell;
  o.shanai.cutoff_hz = 6500.0f;
  o.shanai.reed.chiff = 0.45f;
  o.shanai.lfo_rate_hz = 5.8f;
  o.shanai.lfo_to_pitch_cents = 12.0f;

  // Brass / lip reed (GM 56-60): one lip-reed waveguide voiced across the
  // trumpets, horns and low brass. Small-bore bells (trumpet family) get the
  // radiation formant; large-bore / mellow brass stays on the round linear
  // tone — mirrors the brass presets. Brass Section (61) is the same waveguide
  // in section; SynthBrass (62-63) stays FM by design.
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

  // Brass Section (GM 61): the lip reed in section, on the same argument as the
  // string ensemble — the spread is what makes it a section. A section tongues
  // together far less precisely than a soloist, so the speech is slower and
  // softer-edged while the summed blare stays.
  o.brass_section = brass(false, 0.50f, 0.42f, 0.72f, 0.32f, 45.0f, 130.0f, 0.85f, 0.35f, 0.85f);
  o.brass_section.amp_env.attack_ms = 40.0f;
  o.brass_section.cutoff_hz = 5000.0f;
  o.brass_section.brass.brassiness = 0.7f;
  o.brass_section.brass.cuivre_dynamics = 0.7f;
  o.brass_section.brass.chiff = 0.25f;
  o.brass_section.drift_cents = 6.0f;
  o.brass_section.stereo_spread = 0.4f;
  o.brass_section.lfo_rate_hz = 5.0f;
  o.brass_section.lfo_to_pitch_cents = 3.0f;

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
}

}  // namespace sonare::midi::synth::detail
