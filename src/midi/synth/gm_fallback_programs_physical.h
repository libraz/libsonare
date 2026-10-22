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
    // The first stick drags the string off its mean, a ~10 Hz step the body cannot
    // radiate; a section's seats stacked it 12-14 dB over the held note.
    p.hp_cutoff_hz = 20.0f;
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
  o.violin.lfo_to_pitch_cents = 14.7039f;
  o.violin.amp_env.attack_ms = 255.757f;
  o.violin.amp_env.release_ms = 163.86f;
  o.violin.bowed_string.attack_ms = 36.8818f;
  o.violin.bowed_string.bow_force = 0.1f;
  o.violin.bowed_string.bow_position = 0.17016f;
  o.violin.bowed_string.bow_speed = 0.6f;
  o.violin.bowed_string.damping = 0.14f;
  o.violin.bowed_string.release_ms = 300.0f;
  o.violin.amp_env.decay_ms = 140.69f;
  o.violin.amp_env.sustain = 0.631966f;
  o.violin.body_mix = 0.297933f;
  o.violin.bowed_string.stribeck = 0.811146f;
  o.violin.bowed_string.sympathetic = 0.35846f;
  o.violin.bowed_string.vel_to_speed = 0.312461f;
  o.violin.bowed_string.velocity_exponent = 1.52f;
  o.violin.drift_cents = 1.22416f;
  o.violin.drift_rate_hz = 0.490131f;
  o.violin.stereo_spread = 0.141641f;
  // The bristle memory and the rosin stream each lay a broadband floor under the
  // note: body below f0 read 15 dB over the reference with them, 1.3 dB without.
  o.violin.bowed_string.elasto_plastic = false;
  o.violin.bowed_string.rosin = 0.0f;
  o.viola = bowed(0.13f, 0.55f, 0.42f, 0.34f, 55.0f, 120.0f, 0.34f, 0.3f);
  o.viola.lfo_rate_hz = 5.43422f;
  o.viola.amp_env.attack_ms = 393.35f;
  o.viola.amp_env.decay_ms = 1.0923f;
  o.viola.amp_env.release_ms = 4.56146f;
  o.viola.amp_env.sustain = 0.740741f;
  o.viola.bowed_string.attack_ms = 857.198f;
  o.viola.bowed_string.bow_force = 0.404514f;
  o.viola.bowed_string.bow_position = 0.111153f;
  o.viola.bowed_string.damping = 0.0577136f;
  o.viola.bowed_string.release_ms = 10.0997f;
  o.viola.bowed_string.stribeck = 0.730394f;
  o.viola.bowed_string.velocity_exponent = 1.38f;
  o.viola.drift_rate_hz = 0.632726f;
  o.viola.resonance_q = 0.836075f;
  // corpus_scale scales every mode centre in the violin corpus bank uniformly;
  // these are measured A0 air-mode ratios against violin, not a fit. A uniform
  // scale places A0 but not B1+/- or the bridge hill — viola's A0 ratio tracks
  // its body-length ratio (0.88 against 0.87), so its upper modes land plausibly.
  o.viola.bowed_string.corpus_scale = 0.88f;
  // On the bristle table the floor under the note read 13 dB over the reference;
  // the hyperbolic junction without rosin brings it to 3 dB under.
  o.viola.bowed_string.elasto_plastic = false;
  o.viola.bowed_string.rosin = 0.0f;
  o.viola.bowed_string.hyperbolic_friction = true;
  o.viola.bowed_string.string_length_m = 0.37f;
  o.viola.bowed_string.string_tension_n = 55.0f;
  o.viola.bowed_string.bow_width_m = 0.0185f;
  // Schelleng's minimum force rises toward the bass: at 2 C3 and F#3 fell out of
  // Helmholtz motion (h2 over h1, 6 dB down); 3 holds every note in it.
  o.viola.bowed_string.force_ratio = 3.0f;
  o.viola.bowed_string.bow_speed = 0.9f;
  o.viola.bowed_string.brightness = 0.75f;
  o.viola.cutoff_hz = 5000.0f;
  o.viola.lfo_to_pitch_cents = 12.0f;
  // The body radiates little below its air mode: the reference's C3 and F#3 carry
  // h2-h6 within 3 dB of h1, the sawtooth 12 dB under it.
  o.viola.hp_cutoff_hz = 200.0f;
  o.cello = bowed(0.14f, 0.60f, 0.44f, 0.38f, 70.0f, 140.0f, 0.40f, 0.28f);
  o.cello.lfo_rate_hz = 4.3315f;
  o.cello.lfo_to_pitch_cents = 4.43038f;
  o.cello.amp_env.attack_ms = 6.71239f;
  o.cello.amp_env.release_ms = 388.584f;
  o.cello.amp_env.sustain = 0.398099f;
  // The reference's slow sample attack arrives at 300-450 ms; at 347 the model came
  // in 117 ms early, at 580 within 50.
  o.cello.bowed_string.attack_ms = 580.0f;
  o.cello.bowed_string.bow_force = 0.374109f;
  o.cello.bowed_string.bow_position = 0.167379f;
  o.cello.bowed_string.bow_speed = 0.937158f;
  o.cello.bowed_string.brightness = 0.14f;
  o.cello.bowed_string.damping = 0.355529f;
  o.cello.bowed_string.release_ms = 66.9834f;
  o.cello.bowed_string.stribeck = 0.310161f;
  o.cello.bowed_string.velocity_exponent = 1.30f;
  o.cello.cutoff_hz = 12130.9f;
  o.cello.drift_rate_hz = 0.0730796f;
  o.cello.resonance_q = 0.5f;
  // Cello's A0 ratio runs about 21% steeper than its body-length ratio, so its
  // B1+/- and hill land that much low under this same uniform scale — a cello
  // is not a scaled violin.
  o.cello.bowed_string.corpus_scale = 0.37f;
  o.cello.amp_env.decay_ms = 32.7064f;
  o.cello.body_mix = 0.212461f;
  // Contrabass (GM 43): fitted to its reference on the hyperbolic bow junction.
  // The bridge reflection filter is fixed in Hz, so at these pitches its whole
  // range sits above every partial and the darkening has to come from the string.
  o.contrabass = bowed(0.177f, 0.58f, 0.15f, 0.18f, 185.0f, 160.0f, 0.06f, 0.69f);
  o.contrabass.lfo_rate_hz = 4.83651f;
  o.contrabass.lfo_to_pitch_cents = 15.2584f;
  o.contrabass.cutoff_hz = 2093.58f;
  o.contrabass.resonance_q = 2.04715f;
  o.contrabass.bowed_string.release_ms = 195.626f;
  o.contrabass.bowed_string.bow_speed = 0.112373f;
  o.contrabass.bowed_string.vel_to_speed = 0.0301324f;
  o.contrabass.bowed_string.stribeck = 0.106625f;
  o.contrabass.bowed_string.sympathetic = 0.0847627f;
  // The reference's held level settles 3.8 to 6.4 dB under its own attack peak and
  // then holds flat, so the sustain is that settle rather than a decay.
  o.contrabass.amp_env.attack_ms = 273.932f;
  o.contrabass.amp_env.decay_ms = 126.294f;
  o.contrabass.amp_env.sustain = 0.549535f;
  o.contrabass.amp_env.release_ms = 179.169f;
  o.contrabass.bowed_string.attack_ms = 3.19923f;
  o.contrabass.bowed_string.bow_force = 0.821558f;
  o.contrabass.bowed_string.bow_position = 0.274455f;
  o.contrabass.bowed_string.damping = 0.0688832f;
  o.contrabass.drift_rate_hz = 2.07078f;
  o.contrabass.body_mix = 0.462168f;
  o.contrabass.drift_cents = 4.55576f;
  o.contrabass.bowed_string.brightness = 0.00173835f;
  o.contrabass.bowed_string.rosin = 0.593912f;
  o.contrabass.bowed_string.velocity_exponent = 1.58f;
  o.contrabass.drive = 0.104102f;
  o.contrabass.stereo_spread = 0.24582f;
  // At 1.44 the level was still climbing 3 dB at two seconds and arrived 650 ms late.
  o.contrabass.bowed_string.force_ratio = 2.02851f;
  o.contrabass.bowed_string.bow_width_m = 0.0185494f;
  o.contrabass.bowed_string.corpus_scale = 1.74736f;
  o.contrabass.bowed_string.hyperbolic_friction = true;
  o.contrabass.bowed_string.string_length_m = 1.04f;
  o.contrabass.bowed_string.string_tension_n = 300.0f;

  // Fiddle (GM 110): the same violin, bowed the other way. A short hard stroke
  // near the bridge with the rosin audible and next to no vibrato — the
  // variation is the hand, so this derives from the violin rather than
  // restating it.
  o.fiddle = o.violin;
  // Still bowed on the bristle friction, with the violin's former values.
  o.fiddle.bowed_string.elasto_plastic = true;
  o.fiddle.bowed_string.bow_speed = 0.689765f;
  o.fiddle.bowed_string.damping = 0.0822536f;
  o.fiddle.bowed_string.bow_position = 0.10f;
  o.fiddle.bowed_string.bow_force = 0.68f;
  o.fiddle.bowed_string.brightness = 0.58f;
  o.fiddle.bowed_string.attack_ms = 13.4658f;
  o.fiddle.bowed_string.rosin = 0.169969f;
  o.fiddle.bowed_string.sympathetic = 0.252098f;  // the open strings ring under the tune
  o.fiddle.amp_env.attack_ms = 39.1732f;
  // At 7000 the centroid read 11 % over the reference; 4500 brings the median inside.
  o.fiddle.cutoff_hz = 4500.0f;
  o.fiddle.lfo_to_pitch_cents = 2.0f;
  o.fiddle.amp_env.release_ms = 89.3211f;
  o.fiddle.amp_env.sustain = 0.618034f;
  o.fiddle.body_mix = 0.595867f;
  o.fiddle.bowed_string.corpus_scale = 1.8614f;
  o.fiddle.bowed_string.release_ms = 55.129f;
  o.fiddle.bowed_string.velocity_exponent = 1.26f;
  o.fiddle.bowed_string.vel_to_speed = 0.443769f;
  o.fiddle.drift_cents = 1.22416f;
  o.fiddle.drift_rate_hz = 0.490131f;

  // String Ensemble 1/2 (GM 48-49): the bowed string in section. One waveguide
  // is one player, so the section is the section stage: six seats, each with its
  // own vibrato, read off the string's output so the bow never sees the pitch
  // move. Section 2 is the slower, warmer half of the pair.
  o.string_ensemble_1 = bowed(0.13f, 0.52f, 0.40f, 0.36f, 140.0f, 260.0f, 0.36f, 0.32f);
  // A section does not enter as one bow: the reference reaches its held level
  // 45-135 ms in, and at 8 ms the voice read 77 ms early on every note.
  o.string_ensemble_1.amp_env.attack_ms = 140.0f;
  o.string_ensemble_1.cutoff_hz = 3398.59f;
  o.string_ensemble_1.bowed_string.polarization = 0.150685f;
  o.string_ensemble_1.bowed_string.sympathetic = 0.364805f;
  o.string_ensemble_1.bowed_string.rosin = 0.239966f;
  o.string_ensemble_1.drift_cents = 9.6654f;
  o.string_ensemble_1.stereo_spread = 0.0631312f;
  o.string_ensemble_1.lfo_rate_hz = 1.87579f;
  o.string_ensemble_1.lfo_to_pitch_cents = 7.42545f;
  o.string_ensemble_1.amp_env.decay_ms = 45.3946f;
  o.string_ensemble_1.amp_env.release_ms = 849.558f;
  o.string_ensemble_1.amp_env.sustain = 0.838679f;
  o.string_ensemble_1.bowed_string.attack_ms = 13.4315f;
  o.string_ensemble_1.bowed_string.bow_force = 0.139013f;
  o.string_ensemble_1.bowed_string.bow_position = 0.299943f;
  o.string_ensemble_1.bowed_string.bow_speed = 0.49877f;
  o.string_ensemble_1.bowed_string.brightness = 0.0321249f;
  o.string_ensemble_1.bowed_string.damping = 0.690198f;
  o.string_ensemble_1.bowed_string.release_ms = 121.014f;
  o.string_ensemble_1.bowed_string.stribeck = 0.439863f;
  o.string_ensemble_1.drift_rate_hz = 1.69817f;
  o.string_ensemble_1.resonance_q = 1.39165f;
  // Past about 7 the drifting string falls into period tripling: energy at f0/3 read
  // 4.8 dB over the reference under the note. At 4.5 it read 3.6 dB under.
  o.string_ensemble_1.bowed_string.force_ratio = 6.0f;
  o.string_ensemble_1.bowed_string.bow_width_m = 0.0188825f;
  o.string_ensemble_1.body_mix = 0.0507968f;
  o.string_ensemble_1.bowed_string.corpus_scale = 0.77371f;
  o.string_ensemble_1.bowed_string.vel_to_speed = 0.341634f;
  o.string_ensemble_1.bowed_string.velocity_exponent = 1.57655f;
  o.string_ensemble_1.bowed_string.hyperbolic_friction = true;
  // The section's noise is skirts 4-8 % of f0 around each partial, not the gaps
  // between them: one string read 12 dB cleaner, and 15 cents per seat closes it.
  o.string_ensemble_1.bowed_string.section_spread_cents = 15.0f;
  o.string_ensemble_1.bowed_string.section_rate_hz = 5.0f;
  // Held level falls toward the bass against the reference, 12 dB from C2 to C5. Two routes
  // multiply to (1 - 0.15 k)^2: within 0.5 dB of one route at -0.3 over C2-C5, and positive to
  // note 127, where one route at -0.3 reached zero at E7.
  o.string_ensemble_1.mod_matrix.routes[0] = {ModSource::kKeyTrack, ModDestination::kAmpGain,
                                              -0.15f};
  o.string_ensemble_1.mod_matrix.routes[1] = o.string_ensemble_1.mod_matrix.routes[0];
  o.string_ensemble_2 = o.string_ensemble_1;
  // Section 2's held level already follows the reference across the keyboard.
  o.string_ensemble_2.mod_matrix.routes[0] = ModRoute{};
  o.string_ensemble_2.mod_matrix.routes[1] = ModRoute{};
  o.string_ensemble_2.amp_env.attack_ms = 361.485f;
  o.string_ensemble_2.bowed_string.attack_ms = 80.7759f;
  o.string_ensemble_2.bowed_string.brightness = 0.452887f;
  o.string_ensemble_2.bowed_string.damping = 0.191758f;
  o.string_ensemble_2.cutoff_hz = 4755.43f;
  o.string_ensemble_2.drift_cents = 3.50183f;
  o.string_ensemble_2.amp_env.decay_ms = 12.8399f;
  o.string_ensemble_2.amp_env.release_ms = 264.454f;
  o.string_ensemble_2.amp_env.sustain = 0.452227f;
  o.string_ensemble_2.bowed_string.bow_force = 0.790656f;
  o.string_ensemble_2.bowed_string.bow_position = 0.225855f;
  o.string_ensemble_2.bowed_string.bow_speed = 0.867228f;
  o.string_ensemble_2.bowed_string.release_ms = 110.201f;
  o.string_ensemble_2.bowed_string.stribeck = 0.778821f;
  o.string_ensemble_2.bowed_string.velocity_exponent = 1.18f;
  o.string_ensemble_2.drift_rate_hz = 0.0354479f;
  o.string_ensemble_2.lfo_rate_hz = 0.365881f;
  o.string_ensemble_2.resonance_q = 0.688613f;
  o.string_ensemble_2.bowed_string.hyperbolic_friction = true;
  o.string_ensemble_2.bowed_string.force_ratio = 5.89992f;
  o.string_ensemble_2.bowed_string.bow_width_m = 0.0181513f;
  o.string_ensemble_2.bowed_string.rosin = 0.334405f;
  o.string_ensemble_2.body_mix = 0.636903f;
  o.string_ensemble_2.bowed_string.corpus_scale = 1.32895f;
  o.string_ensemble_2.bowed_string.vel_to_speed = 0.823056f;
  o.string_ensemble_2.bowed_string.section_spread_cents = 15.0f;
  o.string_ensemble_2.bowed_string.section_rate_hz = 6.0f;

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
  // The beating reed goes only to the voices whose references it improves on
  // every dimension at once: the low saxes and the bassoon, all of which the
  // linearised table left more than an octave dark. The double reeds and the
  // clarinet reach their brightness through it too, and pay for it in
  // tone-to-noise and in the band under the note, so they keep the table. Each
  // fitted gain holds the sustained level the table had, so only timbre moves.
  o.soprano_sax = reed(true, 0.55f, 0.55f, 0.64f, 0.32f, 16.0f, 80.0f, 0.78f, 0.30f, 0.55f);
  o.soprano_sax.cutoff_hz = 2010.43f;
  o.soprano_sax.lfo_rate_hz = 1.39493f;
  o.soprano_sax.lfo_to_pitch_cents = 1.10793f;
  o.soprano_sax.reed.growl = 0.15f;
  o.soprano_sax.reed.chiff = 0.76839f;
  o.soprano_sax.amp_env.attack_ms = 0.970615f;
  o.soprano_sax.amp_env.decay_ms = 12.023f;
  o.soprano_sax.amp_env.release_ms = 41.459f;
  o.soprano_sax.amp_env.sustain = 0.846997f;
  o.soprano_sax.drift_rate_hz = 0.578362f;
  o.soprano_sax.reed.attack_ms = 2.25692f;
  o.soprano_sax.reed.breath_noise = 0.527053f;
  o.soprano_sax.reed.breath_pressure = 0.157512f;
  o.soprano_sax.reed.brightness = 0.448914f;
  o.soprano_sax.reed.chiff_ms = 8.15432f;
  o.soprano_sax.reed.damping = 0.263819f;
  o.soprano_sax.reed.reed_opening = 0.778698f;
  o.soprano_sax.reed.reed_stiffness = 0.618202f;
  o.soprano_sax.reed.release_ms = 478.75f;
  o.soprano_sax.reed.vel_to_breath = 0.768676f;
  o.soprano_sax.resonance_q = 0.626897f;
  o.soprano_sax.body_mix = 0.494427f;
  o.soprano_sax.reed.velocity_exponent = 0.73f;
  o.alto_sax = reed(true, 0.55f, 0.55f, 0.62f, 0.34f, 16.0f, 90.0f, 0.78f, 0.32f, 0.1882f);
  // Fitted with the pressure held inside the valve's oscillating band, which
  // reed_voice.cpp states; the worst of 35 captured notes then holds within
  // 0.2 dB/s of its reference, and the gain centres the grid's level.
  o.alto_sax.reed.closing_pressure = 1.88816f;
  o.alto_sax.reed.flow_gain = 1.05175f;
  o.alto_sax.cutoff_hz = 2773.85f;
  o.alto_sax.lfo_rate_hz = 3.01923f;
  o.alto_sax.lfo_to_pitch_cents = 20.3309f;
  o.alto_sax.reed.growl = 0.084017f;
  o.alto_sax.reed.chiff = 0.6f;
  o.alto_sax.reed.reed_opening = 0.62f;
  o.alto_sax.reed.breath_noise = 0.802129f;
  o.alto_sax.amp_env.attack_ms = 72.9492f;
  o.alto_sax.amp_env.decay_ms = 136.673f;
  o.alto_sax.amp_env.release_ms = 30.0285f;
  o.alto_sax.amp_env.sustain = 0.456559f;
  o.alto_sax.body_mix = 0.459725f;
  o.alto_sax.drift_cents = 0.878369f;
  o.alto_sax.drift_rate_hz = 0.394728f;
  o.alto_sax.reed.attack_ms = 4.72187f;
  o.alto_sax.reed.breath_pressure = 0.706935f;
  o.alto_sax.reed.brightness = 0.41387f;
  o.alto_sax.reed.chiff_ms = 28.3312f;
  o.alto_sax.reed.cone_growth = 0.0838137f;
  o.alto_sax.reed.damping = 0.320851f;
  o.alto_sax.reed.release_ms = 18.1302f;
  o.alto_sax.resonance_q = 1.64558f;
  o.alto_sax.stereo_spread = 0.078303f;
  o.alto_sax.reed.velocity_exponent = 1.22313f;
  o.tenor_sax = reed(true, 0.60f, 0.50f, 0.56f, 0.36f, 20.0f, 100.0f, 0.78f, 0.36f, 0.8803f);
  // Placed where the solved valve holds one period across the grid: at 1.27 the lowest notes
  // put lines up to 34 dB under the fundamental, at 2.0 none closer than 49 dB; 2.2 leaves the
  // note arriving 425 ms late.
  o.tenor_sax.reed.closing_pressure = 2.0f;
  o.tenor_sax.reed.flow_gain = 0.85809f;
  o.tenor_sax.cutoff_hz = 4770.65f;
  // Depth fitted against the reference's 5 Hz vibrato; tone-to-noise reads its sidebands as tone.
  o.tenor_sax.lfo_rate_hz = 4.68214f;
  o.tenor_sax.lfo_to_pitch_cents = 5.2443f;
  o.tenor_sax.reed.growl = 0.0541918f;
  o.tenor_sax.reed.chiff = 0.7714f;
  o.tenor_sax.reed.breath_noise = 0.348762f;
  o.tenor_sax.amp_env.attack_ms = 106.412f;
  o.tenor_sax.amp_env.release_ms = 44.3496f;
  o.tenor_sax.amp_env.sustain = 0.584104f;
  o.tenor_sax.drift_rate_hz = 0.52401f;
  o.tenor_sax.reed.attack_ms = 7.51687f;
  o.tenor_sax.reed.breath_pressure = 0.67445f;
  o.tenor_sax.reed.chiff_ms = 77.2934f;
  o.tenor_sax.reed.damping = 0.256414f;
  o.tenor_sax.reed.release_ms = 338.817f;
  o.tenor_sax.reed.vel_to_breath = 1.0f;
  o.tenor_sax.resonance_q = 0.587518f;
  o.tenor_sax.stereo_spread = 0.506966f;
  // Radiation corner a fit chose with it free: held-out loss 7.70 units against
  // 10.22 for the same budget held at 0. On these values alone it takes gate excess
  // 13.45 -> 12.40 and centroid/f0 error 0.42 -> 0.29 octave.
  o.tenor_sax.reed.bell_radiation_hz = 420.675f;
  o.tenor_sax.amp_env.decay_ms = 126.564f;
  o.tenor_sax.body_mix = 0.65144f;
  o.tenor_sax.drift_cents = 0.5f;
  o.tenor_sax.drive = 0.0f;
  o.tenor_sax.reed.velocity_exponent = 0.794909f;
  o.tenor_sax.reed.brightness = 0.640952f;
  o.tenor_sax.reed.cone_growth = 0.218769f;
  o.tenor_sax.reed.pressure_scale = 0.983204f;
  o.tenor_sax.reed.register_vent = 0.0477458f;
  o.tenor_sax.reed.tonehole = 0.0772542f;
  o.baritone_sax = reed(true, 0.60f, 0.50f, 0.5f, 0.40f, 26.0f, 120.0f, 0.78f, 0.40f, 0.2849f);
  // Fitted inside the valve's oscillating band; the worst of 35 captured notes
  // holds within 1.5 dB/s of its reference.
  o.baritone_sax.reed.closing_pressure = 2.04294f;
  o.baritone_sax.reed.flow_gain = 0.85836f;
  o.baritone_sax.cutoff_hz = 1039.85f;
  o.baritone_sax.lfo_rate_hz = 4.8f;
  o.baritone_sax.lfo_to_pitch_cents = 4.0f;
  o.baritone_sax.reed.growl = 0.0520812f;
  o.baritone_sax.reed.chiff = 0.656231f;
  o.baritone_sax.amp_env.attack_ms = 12.1217f;
  o.baritone_sax.amp_env.release_ms = 24.1736f;
  o.baritone_sax.amp_env.sustain = 0.335043f;
  o.baritone_sax.body_mix = 0.443285f;
  o.baritone_sax.drift_cents = 1.24354f;
  o.baritone_sax.drift_rate_hz = 0.490131f;
  o.baritone_sax.reed.attack_ms = 26.0f;
  o.baritone_sax.reed.breath_noise = 0.1f;
  o.baritone_sax.reed.breath_pressure = 0.3f;
  o.baritone_sax.reed.brightness = 0.763932f;
  o.baritone_sax.reed.chiff_ms = 178.538f;
  o.baritone_sax.reed.cone_growth = 0.0487028f;
  o.baritone_sax.reed.tonehole = 0.309017f;
  o.baritone_sax.reed.vel_to_breath = 0.191796f;
  o.baritone_sax.resonance_q = 1.08741f;
  o.baritone_sax.reed.velocity_exponent = 1.42f;
  o.baritone_sax.amp_env.decay_ms = 427.032f;
  o.baritone_sax.reed.damping = 0.775f;
  o.baritone_sax.stereo_spread = 0.225445f;
  o.oboe = reed(true, 0.80f, 0.35f, 0.74f, 0.30f, 18.0f, 70.0f, 0.62f, 0.30f, 0.6f);
  o.oboe.cutoff_hz = 3143.15f;
  o.oboe.lfo_rate_hz = 4.87846f;
  o.oboe.lfo_to_pitch_cents = 0.818501f;
  o.oboe.amp_env.attack_ms = 44.7151f;
  o.oboe.amp_env.release_ms = 560.0f;
  o.oboe.amp_env.sustain = 0.462463f;
  o.oboe.drift_rate_hz = 0.0802635f;
  o.oboe.reed.attack_ms = 3.587f;
  o.oboe.reed.breath_noise = 0.413193f;
  o.oboe.reed.breath_pressure = 0.0401911f;
  o.oboe.reed.brightness = 0.522692f;
  o.oboe.reed.chiff = 0.687938f;
  o.oboe.reed.chiff_ms = 4.25196f;
  o.oboe.reed.damping = 0.14149f;
  o.oboe.reed.reed_opening = 1.0f;
  o.oboe.reed.reed_stiffness = 0.924031f;
  o.oboe.reed.release_ms = 42.3337f;
  o.oboe.reed.vel_to_breath = 1.0f;
  o.oboe.resonance_q = 2.96294f;
  o.oboe.amp_env.decay_ms = 98.0261f;
  o.oboe.reed.velocity_exponent = 0.93f;
  o.english_horn = reed(true, 0.70f, 0.40f, 0.64f, 0.34f, 24.0f, 90.0f, 0.64f, 0.34f, 0.6f);
  o.english_horn.cutoff_hz = 1396.28f;
  // The reference carries a 5 Hz, 6.1 c vibrato (7.8 c here reads the same);
  // 5 c keeps the partial stack inside its bound, which 7.8 c does not.
  o.english_horn.lfo_rate_hz = 5.0f;
  o.english_horn.lfo_to_pitch_cents = 5.0f;
  o.english_horn.amp_env.attack_ms = 2.12781f;
  o.english_horn.amp_env.decay_ms = 812.385f;
  o.english_horn.amp_env.release_ms = 29.2249f;
  o.english_horn.amp_env.sustain = 0.347453f;
  o.english_horn.drift_rate_hz = 0.0904652f;
  o.english_horn.reed.attack_ms = 2.90522f;
  o.english_horn.reed.breath_noise = 0.302845f;
  o.english_horn.reed.breath_pressure = 0.56477f;
  o.english_horn.reed.brightness = 0.622385f;
  o.english_horn.reed.chiff = 0.311299f;
  o.english_horn.reed.chiff_ms = 43.9177f;
  o.english_horn.reed.damping = 0.644397f;
  o.english_horn.reed.reed_opening = 0.678517f;
  o.english_horn.reed.release_ms = 31.3368f;
  o.english_horn.reed.vel_to_breath = 0.199999f;
  o.english_horn.resonance_q = 1.11982f;
  o.english_horn.body_mix = 0.586169f;
  o.english_horn.drift_cents = 0.239228f;
  o.english_horn.drive = 0.612907f;
  o.english_horn.reed.cone_growth = 0.727496f;
  o.english_horn.reed.reed_stiffness = 0.388361f;
  o.english_horn.reed.tonehole = 0.190983f;
  o.english_horn.stereo_spread = 0.285089f;
  o.english_horn.reed.velocity_exponent = 0.88f;
  o.bassoon = reed(true, 0.65f, 0.45f, 0.5f, 0.40f, 30.0f, 120.0f, 0.68f, 0.40f, 0.4554f);
  // Placed where the solved valve holds one period: at 1.07 the reed sat closed 30% of the
  // cycle and the loop ran period-doubled, half-integer lines 42-48 dB under the fundamental;
  // at 1.6 it still beats (20% closed) and the lines sit 62 dB down and more.
  o.bassoon.reed.closing_pressure = 1.6f;
  o.bassoon.reed.flow_gain = 0.811499f;
  // Low because the drive ahead of it adds the upper partials; the centroid reads 32% under the
  // reference's.
  o.bassoon.cutoff_hz = 755.142f;
  // No vibrato: the reference's 3-8 Hz pitch modulation reads under 0.1 c.
  o.bassoon.amp_env.attack_ms = 43.1601f;
  o.bassoon.amp_env.release_ms = 52.3634f;
  o.bassoon.amp_env.sustain = 0.596887f;
  o.bassoon.drift_rate_hz = 0.228195f;
  o.bassoon.reed.attack_ms = 59.9853f;
  // Breath noise is what fills the band under the note that the reference carries: here that
  // band matches and the tone reads 11 dB noisier than the reference; at 0.3 the tone is 4 dB
  // cleaner and the band 8 dB short. No setting meets both.
  o.bassoon.reed.breath_noise = 0.8f;
  o.bassoon.reed.breath_pressure = 0.741138f;
  o.bassoon.reed.brightness = 0.942493f;
  o.bassoon.reed.chiff = 0.623818f;
  o.bassoon.reed.chiff_ms = 1.52222f;
  o.bassoon.reed.release_ms = 93.718f;
  o.bassoon.reed.vel_to_breath = 0.184872f;
  o.bassoon.resonance_q = 2.73855f;
  o.bassoon.amp_env.decay_ms = 251.586f;
  o.bassoon.reed.velocity_exponent = 2.18f;
  o.bassoon.body_mix = 0.772581f;
  o.bassoon.drift_cents = 1.41556f;
  o.bassoon.drive = 0.704915f;
  o.bassoon.reed.cone_growth = 0.401064f;
  o.bassoon.reed.damping = 0.211803f;
  o.bassoon.reed.tonehole = 0.322949f;
  o.bassoon.stereo_spread = 0.248766f;
  o.clarinet = reed(false, 0.40f, 0.50f, 0.54f, 0.30f, 25.0f, 90.0f, 0.72f, 0.25f, 0.6f);
  o.clarinet.cutoff_hz = 4800.0f;
  // No vibrato: the reference's f0 holds within 0.1 c peak to peak; a 2.5 c LFO
  // read as 21 dB of sideband noise against it.
  o.clarinet.reed.velocity_exponent = 2.80f;

  // Bag pipe (GM 109): the chanter — a stiff double reed on a conical bore fed
  // from the bag, so the pressure never varies and the note is never tongued.
  // vel_to_breath is zero because a piper has no dynamics, not as a voicing
  // choice. The drones are not voiced: one waveguide is one pipe.
  o.bag_pipe = reed(true, 0.82f, 0.32f, 0.86f, 0.24f, 30.0f, 60.0f, 0.86f, 0.22f, 0.55f);
  o.bag_pipe.cutoff_hz = 2818.92f;
  o.bag_pipe.reed.vel_to_breath = 0.0f;
  o.bag_pipe.reed.chiff = 0.764825f;
  o.bag_pipe.reed.breath_noise = 0.159515f;
  o.bag_pipe.amp_env.attack_ms = 29.2278f;
  o.bag_pipe.amp_env.decay_ms = 3.03525f;
  o.bag_pipe.amp_env.release_ms = 44.8033f;
  o.bag_pipe.amp_env.sustain = 0.817048f;
  o.bag_pipe.drift_rate_hz = 0.0886129f;
  o.bag_pipe.reed.attack_ms = 1.80993f;
  o.bag_pipe.reed.breath_pressure = 0.76746f;
  o.bag_pipe.reed.brightness = 0.34615f;
  o.bag_pipe.reed.chiff_ms = 22.643f;
  o.bag_pipe.reed.damping = 0.691292f;
  o.bag_pipe.reed.reed_opening = 0.781414f;
  o.bag_pipe.reed.reed_stiffness = 0.183843f;
  o.bag_pipe.resonance_q = 3.96034f;
  o.bag_pipe.drift_cents = 2.45065f;
  o.bag_pipe.drive = 0.381966f;
  o.bag_pipe.reed.cone_growth = 0.401722f;
  o.bag_pipe.reed.release_ms = 20.019f;
  o.bag_pipe.reed.tonehole = 0.118034f;
  o.bag_pipe.stereo_spread = 0.35846f;
  o.bag_pipe.reed.velocity_exponent = 0.95f;

  // Shanai (GM 111): a double reed on a wooden cone into a flared METAL bell,
  // which is where its edge comes from and why it is the one reed here on the
  // brass body rather than the wood tube. Played with a wide expressive slide,
  // so the pitch modulation is deep next to an orchestral double reed's.
  o.shanai = reed(true, 0.86f, 0.30f, 0.82f, 0.26f, 22.0f, 80.0f, 0.72f, 0.28f, 0.58f);
  o.shanai.body = BodyType::kBrassBell;
  o.shanai.cutoff_hz = 3308.58f;
  o.shanai.reed.chiff = 0.333747f;
  o.shanai.lfo_rate_hz = 2.43617f;
  o.shanai.lfo_to_pitch_cents = 1.5f;
  o.shanai.amp_env.attack_ms = 3.37582f;
  o.shanai.amp_env.decay_ms = 391.635f;
  o.shanai.amp_env.release_ms = 48.2381f;
  o.shanai.amp_env.sustain = 0.497501f;
  o.shanai.drift_rate_hz = 2.41039f;
  o.shanai.reed.attack_ms = 1.81075f;
  o.shanai.reed.breath_noise = 0.0475289f;
  o.shanai.reed.breath_pressure = 0.567458f;
  o.shanai.reed.brightness = 0.567585f;
  o.shanai.reed.chiff_ms = 6.6656f;
  o.shanai.reed.damping = 0.760238f;
  o.shanai.reed.reed_opening = 0.311036f;
  o.shanai.reed.reed_stiffness = 0.901321f;
  o.shanai.reed.release_ms = 52.1026f;
  o.shanai.resonance_q = 17.0241f;
  o.shanai.body_mix = 0.53685f;
  o.shanai.drift_cents = 1.91728f;
  o.shanai.drive = 0.309017f;
  o.shanai.reed.cone_growth = 0.152786f;
  o.shanai.reed.vel_to_breath = 0.715248f;
  o.shanai.stereo_spread = 0.269934f;
  o.shanai.reed.velocity_exponent = 0.96f;

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
    // Brass physics gates: the cuivré shock shaper adds the blare of a loud
    // brass, and the 2-DOF lip livens the attack buzz. What makes the tone
    // bright at all is the bell radiation each voice sets below — driven to
    // full, the shaper supplies a twelfth of the partial stack a reference
    // brass has, because the bore pressure it reshapes barely has one.
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
  // The bell corner and the voice lowpass that goes with it: what the flare
  // radiates is what it did not reflect, so the top of the range closes in by
  // roughly what the corner lifts. Fitted; the corners order as the bells do.
  o.trumpet = brass(false, 0.55f, 0.30f, 0.75f, 0.28f, 12.0f, 80.0f, 0.88f, 0.50f, 0.90f);
  o.trumpet.brass.lip_aperture = 0.93213f;
  o.trumpet.cutoff_hz = 1850.65f;
  o.trumpet.brass.brassiness = 0.62918f;
  o.trumpet.brass.cuivre_dynamics = 0.813931f;
  // No vibrato: the reference's f0 holds within 1.2 c; a fitted 24 Hz pitch LFO
  // read as 10 dB of sideband noise against it.
  o.trumpet.brass.bell_cutoff_hz = 461.622f;
  o.trumpet.brass.breath_pressure = 0.658586f;
  o.trumpet.brass.brightness = 0.840833f;
  o.trumpet.brass.damping = 0.290831f;
  o.trumpet.brass.lip_damping = 0.827166f;
  o.trumpet.brass.lip_tension = 0.478307f;
  o.trumpet.gain = 1.62865f;
  o.trumpet.brass.bore_nonlinearity = 0.161808f;
  // Settle read off the reference's onset; the overshoot rises with velocity as its takes do.
  o.trumpet.brass.onset_breath_overshoot_db = 6.88525f;
  o.trumpet.brass.onset_breath_overshoot_vel_db = 1.35125f;
  o.trumpet.brass.onset_breath_settle_ms = 134.484f;
  o.trumpet.brass.velocity_exponent = 1.59f;
  o.trumpet.amp_env.attack_ms = 64.315f;
  o.trumpet.amp_env.decay_ms = 127.124f;
  o.trumpet.amp_env.release_ms = 70.7607f;
  o.trumpet.amp_env.sustain = 0.72828f;
  o.trumpet.body_mix = 0.54794f;
  o.trumpet.brass.attack_ms = 19.6724f;
  o.trumpet.brass.breath_noise = 0.0434807f;
  o.trumpet.brass.chiff = 0.494655f;
  o.trumpet.brass.chiff_ms = 40.7337f;
  o.trumpet.brass.dynamic_lip = 0.290392f;
  o.trumpet.brass.release_ms = 161.737f;
  o.trumpet.brass.vel_to_breath = 0.427559f;
  o.trumpet.drift_cents = 1.25207f;
  o.trumpet.drift_rate_hz = 0.106066f;
  o.trumpet.resonance_q = 0.898862f;
  o.trumpet.stereo_spread = 0.428326f;
  o.trombone = brass(false, 0.48f, 0.45f, 0.85f, 0.32f, 26.0f, 100.0f, 0.85f, 0.0f, 0.92f);
  o.trombone.brass.lip_aperture = 0.153818f;
  o.trombone.cutoff_hz = 580.0f;
  o.trombone.brass.brassiness = 0.998735f;
  o.trombone.brass.cuivre_dynamics = 0.340141f;
  o.trombone.lfo_rate_hz = 2.47118f;
  o.trombone.lfo_to_pitch_cents = 0.597068f;
  o.trombone.amp_env.attack_ms = 79.4293f;
  o.trombone.amp_env.release_ms = 99.453f;
  o.trombone.amp_env.sustain = 0.532329f;
  o.trombone.brass.attack_ms = 2.33858f;
  o.trombone.brass.breath_pressure = 0.993783f;
  o.trombone.brass.brightness = 0.284872f;
  o.trombone.brass.chiff = 0.300794f;
  o.trombone.brass.chiff_ms = 7.27184f;
  o.trombone.brass.damping = 0.495809f;
  o.trombone.brass.lip_damping = 1.0f;
  o.trombone.brass.lip_tension = 0.452265f;
  o.trombone.brass.release_ms = 31.8924f;
  o.trombone.brass.vel_to_breath = 0.200621f;
  o.trombone.drift_rate_hz = 0.506794f;
  o.trombone.resonance_q = 1.30183f;
  o.trombone.brass.bell_cutoff_hz = 2450.08f;
  o.trombone.brass.bore_nonlinearity = 0.381076f;
  o.trombone.gain = 16.0f;
  o.trombone.amp_env.decay_ms = 179.829f;
  o.trombone.brass.dynamic_lip = 0.177051f;
  o.trombone.brass.half_valve = 0.190983f;
  o.trombone.brass.onset_breath_overshoot_db = 2.1f;
  o.trombone.brass.onset_breath_settle_ms = 70.0f;
  o.trombone.brass.velocity_exponent = 1.375f;
  o.tuba = brass(true, 0.42f, 0.70f, 0.38f, 0.42f, 40.0f, 140.0f, 0.88f, 0.0f, 0.92f);
  o.tuba.brass.lip_aperture = 0.817288f;
  o.tuba.cutoff_hz = 1200.0f;
  o.tuba.brass.brassiness = 0.541796f;
  o.tuba.brass.cuivre_dynamics = 0.691899f;
  o.tuba.lfo_to_pitch_cents = 1.5f;
  o.tuba.brass.bell_cutoff_hz = 784.162f;
  o.tuba.brass.breath_pressure = 0.591639f;
  o.tuba.brass.brightness = 0.378016f;
  o.tuba.brass.damping = 0.215189f;
  o.tuba.brass.lip_damping = 0.557119f;
  o.tuba.gain = 2.08962f;
  o.tuba.brass.bore_nonlinearity = 0.1f;
  o.tuba.brass.vel_to_breath = 0.330303f;
  o.tuba.amp_env.release_ms = 67.0412f;
  o.tuba.brass.attack_ms = 4.45289f;
  o.tuba.brass.onset_breath_overshoot_db = 6.338f;
  o.tuba.brass.onset_breath_settle_ms = 83.9202f;
  o.tuba.amp_env.attack_ms = 7.02696f;
  o.tuba.amp_env.decay_ms = 103.874f;
  o.tuba.amp_env.sustain = 0.559017f;
  o.tuba.brass.breath_noise = 0.141641f;
  o.tuba.brass.chiff = 0.590325f;
  o.tuba.brass.chiff_ms = 49.6409f;
  o.tuba.brass.dynamic_lip = 0.704915f;
  o.tuba.brass.half_valve = 0.513932f;
  o.tuba.brass.release_ms = 85.6914f;
  o.tuba.brass.velocity_exponent = 0.988f;
  o.tuba.drift_cents = 2.45065f;
  o.tuba.drift_rate_hz = 0.899146f;
  o.tuba.lfo_rate_hz = 3.06041f;
  o.tuba.resonance_q = 0.907078f;
  o.tuba.stereo_spread = 0.35846f;
  // The muted trumpet plays through the real mute model instead of the old
  // dimmed-brightness fake.
  // The mute already attenuates, so this voice needs less of the radiation
  // make-up than the rest of the family — 2.7 dB of it comes back out here.
  o.muted_trumpet = brass(false, 0.58f, 0.35f, 0.62f, 0.30f, 16.0f, 75.0f, 0.80f, 0.0f, 0.60f);
  o.muted_trumpet.brass.lip_aperture = 0.684851f;
  o.muted_trumpet.brass.brassiness = 0.595097f;
  o.muted_trumpet.brass.cuivre_dynamics = 0.90409f;
  o.muted_trumpet.brass.mute = 1.0f;
  o.muted_trumpet.lfo_rate_hz = 1.14931f;
  o.muted_trumpet.lfo_to_pitch_cents = 0.0336192f;
  o.muted_trumpet.amp_env.attack_ms = 21.3873f;
  o.muted_trumpet.amp_env.decay_ms = 170.323f;
  o.muted_trumpet.amp_env.release_ms = 107.846f;
  o.muted_trumpet.amp_env.sustain = 0.55814f;
  o.muted_trumpet.brass.attack_ms = 1.97162f;
  o.muted_trumpet.brass.breath_pressure = 0.0894544f;
  o.muted_trumpet.brass.chiff = 0.50745f;
  o.muted_trumpet.brass.chiff_ms = 92.5717f;
  o.muted_trumpet.brass.damping = 0.902268f;
  o.muted_trumpet.brass.lip_damping = 0.818029f;
  o.muted_trumpet.brass.lip_tension = 0.505124f;
  o.muted_trumpet.brass.release_ms = 2.97828f;
  o.muted_trumpet.brass.vel_to_breath = 0.616017f;
  o.muted_trumpet.resonance_q = 0.598471f;
  o.muted_trumpet.brass.bell_cutoff_hz = 429.085f;
  o.muted_trumpet.brass.brightness = 0.655602f;
  o.muted_trumpet.gain = 1.51195f;
  o.muted_trumpet.brass.bore_nonlinearity = 0.0994201f;
  // With the bell corner held at or above the fundamental, D5-E5 overshot their early sustain
  // 2.25 dB past the reference at p90 against a 2.05 bound; these read 1.92.
  o.muted_trumpet.brass.onset_breath_overshoot_db = 6.3f;
  o.muted_trumpet.brass.onset_breath_settle_ms = 26.0f;
  o.muted_trumpet.body_mix = 0.195493f;
  o.muted_trumpet.brass.breath_noise = 0.0827071f;
  o.muted_trumpet.brass.dynamic_lip = 0.606152f;
  o.muted_trumpet.brass.half_valve = 0.802302f;
  o.muted_trumpet.brass.velocity_exponent = 1.31541f;
  o.muted_trumpet.drift_cents = 0.816073f;
  o.muted_trumpet.drift_rate_hz = 0.154675f;
  o.muted_trumpet.drive = 0.151158f;
  o.muted_trumpet.filter_env.attack_ms = 3.43917f;
  o.muted_trumpet.filter_env.decay_ms = 54.1906f;
  o.muted_trumpet.filter_env.release_ms = 433.683f;
  o.muted_trumpet.filter_env.sustain = 0.166451f;
  o.muted_trumpet.stereo_spread = 0.766937f;
  o.muted_trumpet.unison = 2;
  o.muted_trumpet.cutoff_hz = 3530.56f;
  o.french_horn = brass(true, 0.50f, 0.55f, 0.48f, 0.34f, 30.0f, 110.0f, 0.82f, 0.0f, 0.88f);
  o.french_horn.brass.lip_aperture = 0.73703f;
  o.french_horn.cutoff_hz = 637.0f;
  o.french_horn.brass.brassiness = 0.606392f;
  o.french_horn.brass.cuivre_dynamics = 0.414392f;
  o.french_horn.lfo_to_pitch_cents = 1.17353f;
  o.french_horn.brass.bell_cutoff_hz = 821.625f;
  o.french_horn.brass.breath_pressure = 0.285863f;
  o.french_horn.brass.brightness = 0.370238f;
  o.french_horn.brass.damping = 0.744488f;
  o.french_horn.brass.lip_damping = 0.895683f;
  o.french_horn.brass.lip_tension = 0.5f;
  o.french_horn.gain = 2.18181f;
  o.french_horn.brass.bore_nonlinearity = 0.208044f;
  o.french_horn.amp_env.attack_ms = 58.7598f;
  o.french_horn.amp_env.decay_ms = 128.979f;
  o.french_horn.amp_env.release_ms = 67.329f;
  o.french_horn.amp_env.sustain = 0.663119f;
  o.french_horn.brass.attack_ms = 2.95397f;
  o.french_horn.brass.breath_noise = 0.212374f;
  o.french_horn.brass.chiff = 0.0277836f;
  o.french_horn.brass.chiff_ms = 34.3792f;
  o.french_horn.brass.dynamic_lip = 0.293441f;
  o.french_horn.brass.release_ms = 328.562f;
  o.french_horn.brass.vel_to_breath = 0.856261f;
  o.french_horn.drift_cents = 0.270029f;
  o.french_horn.drift_rate_hz = 0.407723f;
  o.french_horn.lfo_rate_hz = 15.3679f;
  o.french_horn.resonance_q = 1.33736f;
  o.french_horn.brass.onset_breath_overshoot_db = 2.32198f;
  o.french_horn.brass.onset_breath_settle_ms = 15.9492f;
  o.french_horn.brass.velocity_exponent = 1.04803f;
  o.french_horn.drive = 0.0404657f;
  o.french_horn.stereo_spread = 0.674021f;

  // Brass Section (GM 61): the lip reed in section, on the same argument as the
  // string ensemble — the spread is what makes it a section. A section tongues
  // together far less precisely than a soloist, so the speech is slower and
  // softer-edged while the summed blare stays.
  o.brass_section = brass(false, 0.50f, 0.42f, 0.72f, 0.32f, 45.0f, 130.0f, 0.85f, 0.35f, 0.85f);
  o.brass_section.brass.lip_aperture = 0.810896f;
  o.brass_section.amp_env.attack_ms = 36.6655f;
  o.brass_section.cutoff_hz = 570.505f;
  o.brass_section.brass.brassiness = 0.228995f;
  o.brass_section.brass.cuivre_dynamics = 0.748863f;
  o.brass_section.brass.chiff = 0.203084f;
  o.brass_section.drift_cents = 10.209f;
  o.brass_section.stereo_spread = 0.972044f;
  o.brass_section.lfo_rate_hz = 1.79471f;
  o.brass_section.lfo_to_pitch_cents = 2.01483f;
  o.brass_section.amp_env.decay_ms = 0.648809f;
  o.brass_section.amp_env.release_ms = 69.7742f;
  o.brass_section.amp_env.sustain = 0.420284f;
  o.brass_section.brass.attack_ms = 9.20641f;
  o.brass_section.brass.breath_pressure = 0.673627f;
  o.brass_section.brass.brightness = 0.743112f;
  o.brass_section.brass.chiff_ms = 6.94592f;
  o.brass_section.brass.damping = 0.59656f;
  o.brass_section.brass.lip_damping = 0.812364f;
  o.brass_section.brass.lip_tension = 0.646858f;
  o.brass_section.brass.release_ms = 264.072f;
  o.brass_section.brass.vel_to_breath = 0.201144f;
  o.brass_section.drift_rate_hz = 2.95782f;
  o.brass_section.resonance_q = 0.506819f;
  o.brass_section.brass.bell_cutoff_hz = 3859.29f;
  o.brass_section.gain = 7.34402f;
  o.brass_section.brass.bore_nonlinearity = 0.479409f;
  o.brass_section.body_mix = 0.843808f;
  o.brass_section.brass.breath_noise = 0.0824593f;
  o.brass_section.brass.dynamic_lip = 0.425212f;
  o.brass_section.brass.half_valve = 0.111068f;
  o.brass_section.brass.velocity_exponent = 1.81084f;
  o.brass_section.drive = 0.113923f;

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
  // The cubic source runs the loop phase servo: the pitch sits on the period whatever the jet does.
  o.piccolo.flute.loop_phase_servo = true;
  o.piccolo.amp_env.attack_ms = 6.03421f;
  o.piccolo.flute.overblow = 0.294968f;
  o.piccolo.amp_env.release_ms = 106.228f;
  o.piccolo.cutoff_hz = 2655.32f;
  o.piccolo.drift_rate_hz = 0.0304707f;
  o.piccolo.drive = 0.0f;
  o.piccolo.flute.attack_ms = 8.84122f;
  o.piccolo.flute.breath_pressure = 0.813217f;
  o.piccolo.flute.brightness = 0.371516f;
  o.piccolo.flute.chiff = 0.63686f;
  o.piccolo.flute.chiff_ms = 1.68815f;
  o.piccolo.flute.end_reflection = 0.685393f;
  o.piccolo.flute.jet_ratio = 0.516196f;
  o.piccolo.flute.jet_reflection = 0.21914f;
  o.piccolo.flute.release_ms = 362.814f;
  o.piccolo.flute.vel_to_breath = 0.457936f;
  o.piccolo.flute.vibrato_depth = 0.0f;
  o.piccolo.flute.vibrato_rate_hz = 1.04251f;
  o.piccolo.resonance_q = 0.629953f;
  o.piccolo.amp_env.sustain = 0.809017f;
  o.piccolo.flute.breath_noise = 0.155929f;
  o.piccolo.amp_env.decay_ms = 98.0261f;
  o.piccolo.flute.velocity_exponent = 2.17f;
  o.concert_flute = flute(0.50f, 0.55f, 0.30f, 0.35f, 0.2f, 0.15f, 0.60f, 0.85f);
  o.concert_flute.cutoff_hz = 5961.8f;
  o.concert_flute.amp_env.attack_ms = 90.0f;
  o.concert_flute.flute.overblow = 0.35f;
  o.concert_flute.flute.velocity_exponent = 1.12f;
  o.concert_flute.flute.jet_drive = true;
  o.concert_flute.flute.jet_cycle_mean = true;
  o.concert_flute.flute.jet_ratio = 0.503f;
  o.concert_flute.flute.jet_offset = 0.787258f;
  o.concert_flute.flute.jet_deflection_limit = 2.82715f;
  o.concert_flute.flute.jet_receptivity_window = 0.353174f;
  o.concert_flute.flute.jet_centring_per_octave = 0.9f;
  o.concert_flute.flute.vortex_loss = 0.0325243f;
  o.concert_flute.flute.end_reflection = 0.890181f;
  o.concert_flute.flute.lattice_cutoff_hz = 2420.66f;
  o.concert_flute.flute.brightness = 0.372594f;
  o.concert_flute.flute.damping = 0.153756f;
  o.concert_flute.flute.breath_noise = 0.00590309f;
  o.concert_flute.flute.breath_radiation = 0.08f;
  o.concert_flute.flute.release_ms = 184.428f;
  o.concert_flute.flute.onset_breath_overshoot_db = 3.549f;
  o.recorder = flute(0.50f, 0.50f, 0.35f, 0.14f, 0.55f, 0.05f, 0.55f, 0.85f);
  o.recorder.flute.loop_phase_servo = true;
  o.recorder.body = BodyType::kWoodTube;
  o.recorder.body_mix = 0.972592f;
  o.recorder.amp_env.attack_ms = 31.0135f;
  o.recorder.amp_env.release_ms = 45.2658f;
  o.recorder.cutoff_hz = 2286.9f;
  o.recorder.drift_cents = 0.500475f;
  o.recorder.drift_rate_hz = 0.521148f;
  o.recorder.flute.attack_ms = 5.64826f;
  o.recorder.flute.breath_noise = 0.0210929f;
  o.recorder.flute.breath_pressure = 0.509221f;
  o.recorder.flute.brightness = 0.170085f;
  o.recorder.flute.chiff = 0.451025f;
  o.recorder.flute.chiff_ms = 10.6141f;
  o.recorder.flute.jet_reflection = 0.236068f;
  o.recorder.flute.jet_turbulence = 0.0212446f;
  o.recorder.flute.overblow = 0.190983f;
  o.recorder.flute.release_ms = 122.317f;
  o.recorder.flute.vel_to_breath = 0.381966f;
  o.recorder.flute.vibrato_rate_hz = 1.9886f;
  o.recorder.resonance_q = 0.524082f;
  o.recorder.stereo_spread = 0.071363f;
  o.recorder.amp_env.sustain = 0.976127f;
  o.recorder.flute.onset_breath_overshoot_db = 2.47214f;
  o.recorder.flute.onset_breath_overshoot_vel_db = 1.63525f;
  o.recorder.flute.onset_breath_settle_ms = 24.4833f;
  o.recorder.flute.velocity_exponent = 1.67f;
  // Jet delay on the half period: 0.52 pulled the pitch 4.4 cents flat and brightened the stack.
  o.pan_flute = flute(0.50f, 0.42f, 0.40f, 0.40f, 0.30f, 0.08f, 0.55f, 0.85f);
  o.pan_flute.flute.loop_phase_servo = true;
  o.pan_flute.flute.vortex = 0.35f;
  o.pan_flute.body = BodyType::kWoodTube;
  o.pan_flute.body_mix = 0.15f;
  o.pan_flute.amp_env.attack_ms = 23.9772f;
  o.pan_flute.flute.velocity_exponent = 1.39f;
  // The reference stops 85-95 ms after the key lifts; the breath ramp sets that, not the VCA.
  o.pan_flute.flute.release_ms = 400.0f;
  o.blown_bottle = flute(0.50f, 0.35f, 0.50f, 0.35f, 0.25f, 0.0f, 0.55f, 0.85f);
  o.blown_bottle.flute.loop_phase_servo = true;
  o.blown_bottle.amp_env.release_ms = 45.2492f;
  o.blown_bottle.amp_env.sustain = 0.45047f;
  o.blown_bottle.flute.damping = 0.671722f;
  o.blown_bottle.flute.end_reflection = 1.0f;
  o.blown_bottle.flute.jet_reflection = 0.162748f;
  o.blown_bottle.flute.release_ms = 95.402f;
  o.blown_bottle.amp_env.attack_ms = 4.36086f;
  o.blown_bottle.amp_env.decay_ms = 12.2533f;
  o.blown_bottle.cutoff_hz = 5007.2f;
  // The reference holds its pitch inside a note and its rows sit within 0.5 c of each other.
  o.blown_bottle.drift_cents = 0.5f;
  o.blown_bottle.flute.attack_ms = 4.15482f;
  o.blown_bottle.flute.breath_noise = 0.314996f;
  o.blown_bottle.flute.breath_pressure = 0.775735f;
  o.blown_bottle.flute.velocity_exponent = 1.21f;
  o.shakuhachi = flute(0.52f, 0.48f, 0.35f, 0.55f, 0.30f, 0.20f, 0.58f, 0.85f);
  o.shakuhachi.flute.loop_phase_servo = true;
  o.shakuhachi.flute.vortex = 0.104581f;
  o.shakuhachi.body = BodyType::kWoodTube;
  o.shakuhachi.body_mix = 0.479847f;
  o.shakuhachi.amp_env.attack_ms = 64.009f;
  o.shakuhachi.amp_env.release_ms = 83.7378f;
  o.shakuhachi.amp_env.sustain = 0.317428f;
  o.shakuhachi.cutoff_hz = 7643.73f;
  o.shakuhachi.drift_rate_hz = 0.0677458f;
  o.shakuhachi.flute.attack_ms = 8.93819f;
  o.shakuhachi.flute.breath_noise = 0.668968f;
  o.shakuhachi.flute.breath_pressure = 0.757325f;
  o.shakuhachi.flute.brightness = 0.386933f;
  o.shakuhachi.flute.chiff = 0.205638f;
  o.shakuhachi.flute.chiff_ms = 43.2034f;
  o.shakuhachi.flute.damping = 0.0864287f;
  o.shakuhachi.flute.end_reflection = 0.913557f;
  o.shakuhachi.flute.jet_ratio = 0.542106f;
  o.shakuhachi.flute.jet_reflection = 0.901881f;
  o.shakuhachi.flute.release_ms = 65.8372f;
  o.shakuhachi.flute.vel_to_breath = 0.680257f;
  o.shakuhachi.flute.vibrato_rate_hz = 5.21179f;
  o.shakuhachi.resonance_q = 0.5f;
  o.shakuhachi.amp_env.decay_ms = 179.829f;
  o.shakuhachi.flute.velocity_exponent = 0.80f;
  o.tin_whistle = flute(0.48f, 0.70f, 0.28f, 0.10f, 0.45f, 0.04f, 0.62f, 0.80f);
  o.tin_whistle.flute.loop_phase_servo = true;
  o.tin_whistle.flute.overblow = 0.25f;
  o.tin_whistle.flute.velocity_exponent = 0.50f;
  // The reference stops 65 ms after the key lifts; the breath ramp sets that, not the VCA.
  o.tin_whistle.flute.release_ms = 250.0f;
  o.ocarina = flute(0.50f, 0.40f, 0.55f, 0.15f, 0.30f, 0.06f, 0.55f, 0.85f);
  o.ocarina.flute.loop_phase_servo = true;
  o.ocarina.amp_env.attack_ms = 2.25225f;
  o.ocarina.amp_env.decay_ms = 8.54137f;
  // The reference is 40 dB down 20 ms after the key lifts; at 6 ms only the step's splatter was
  // left to read.
  o.ocarina.amp_env.release_ms = 20.0f;
  o.ocarina.amp_env.sustain = 0.300173f;
  o.ocarina.cutoff_hz = 1015.67f;
  o.ocarina.drift_rate_hz = 0.0285699f;
  o.ocarina.drive = 0.0f;
  o.ocarina.flute.attack_ms = 4.53609f;
  o.ocarina.flute.breath_pressure = 0.352329f;
  o.ocarina.flute.brightness = 0.639144f;
  o.ocarina.flute.chiff = 0.292181f;
  o.ocarina.flute.chiff_ms = 19.9084f;
  o.ocarina.flute.end_reflection = 0.877128f;
  o.ocarina.flute.jet_ratio = 0.49458f;
  o.ocarina.flute.jet_reflection = 0.126649f;
  o.ocarina.flute.release_ms = 287.352f;
  o.ocarina.flute.vel_to_breath = 0.282063f;
  o.ocarina.resonance_q = 0.94061f;
  o.ocarina.drift_cents = 0.918123f;
  o.ocarina.flute.breath_noise = 0.0269825f;
  o.ocarina.flute.damping = 0.307256f;
  o.ocarina.flute.edge_hysteresis = 0.0780765f;
  o.ocarina.flute.jet_turbulence = 0.0274937f;
  o.ocarina.flute.overblow = 0.0568644f;
  o.ocarina.flute.vibrato_depth = 0.114336f;
  o.ocarina.flute.vibrato_rate_hz = 1.82255f;
  o.ocarina.stereo_spread = 0.339801f;
  o.ocarina.flute.velocity_exponent = 0.72f;
}

}  // namespace sonare::midi::synth::detail
