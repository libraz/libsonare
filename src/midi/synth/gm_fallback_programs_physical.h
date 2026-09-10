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
  o.violin.amp_env.attack_ms = 51.5215f;
  o.violin.amp_env.release_ms = 267.709f;
  o.violin.bowed_string.attack_ms = 47.142f;
  o.violin.bowed_string.bow_force = 0.0643318f;
  o.violin.bowed_string.bow_position = 0.17016f;
  o.violin.bowed_string.bow_speed = 0.630748f;
  o.violin.bowed_string.damping = 0.0822536f;
  o.violin.bowed_string.release_ms = 165.23f;
  o.viola = bowed(0.13f, 0.55f, 0.42f, 0.34f, 55.0f, 120.0f, 0.34f, 0.3f);
  o.viola.lfo_rate_hz = 5.43422f;
  o.viola.lfo_to_pitch_cents = 1.9526f;
  o.viola.amp_env.attack_ms = 393.35f;
  o.viola.amp_env.decay_ms = 1.78457f;
  o.viola.amp_env.release_ms = 7.06872f;
  o.viola.amp_env.sustain = 0.740741f;
  o.viola.bowed_string.attack_ms = 857.198f;
  o.viola.bowed_string.bow_force = 0.404514f;
  o.viola.bowed_string.bow_position = 0.111153f;
  o.viola.bowed_string.bow_speed = 0.781176f;
  o.viola.bowed_string.brightness = 0.385659f;
  o.viola.bowed_string.damping = 0.0577136f;
  o.viola.bowed_string.release_ms = 10.0997f;
  o.viola.bowed_string.stribeck = 0.730394f;
  o.viola.cutoff_hz = 2339.23f;
  o.viola.drift_rate_hz = 0.632726f;
  o.viola.resonance_q = 0.836075f;
  o.cello = bowed(0.14f, 0.60f, 0.44f, 0.38f, 70.0f, 140.0f, 0.40f, 0.28f);
  o.cello.lfo_rate_hz = 4.3315f;
  o.cello.lfo_to_pitch_cents = 4.43038f;
  o.cello.amp_env.attack_ms = 10.9665f;
  o.cello.amp_env.release_ms = 79.3572f;
  o.cello.amp_env.sustain = 0.733218f;
  o.cello.bowed_string.attack_ms = 346.911f;
  o.cello.bowed_string.bow_force = 0.374109f;
  o.cello.bowed_string.bow_position = 0.167379f;
  o.cello.bowed_string.bow_speed = 0.937158f;
  o.cello.bowed_string.brightness = 0.161039f;
  o.cello.bowed_string.damping = 0.355529f;
  o.cello.bowed_string.release_ms = 66.9834f;
  o.cello.bowed_string.stribeck = 0.310161f;
  o.cello.cutoff_hz = 12130.9f;
  o.cello.drift_rate_hz = 0.0730796f;
  o.cello.resonance_q = 0.5f;
  // Contrabass (GM 43): the one member of the family with a reference, and fitted
  // to it. The bridge reflection filter is fixed in Hz, so at these pitches its
  // whole range sits above every partial and the darkening has to come from the
  // patch filter below. The corpus is all but muted because kViolin is a violin.
  // Gain restated because the darker string and the muted corpus together cost
  // 6.7 dB: it puts the model back on the reference's own peak and held level.
  o.contrabass = bowed(0.177f, 0.58f, 0.15f, 0.18f, 185.0f, 160.0f, 0.06f, 0.69f);
  o.contrabass.lfo_rate_hz = 8.67559f;
  o.contrabass.lfo_to_pitch_cents = 2.62525f;
  o.contrabass.cutoff_hz = 456.942f;
  o.contrabass.resonance_q = 2.04715f;
  o.contrabass.bowed_string.release_ms = 299.56f;
  o.contrabass.bowed_string.bow_speed = 0.342101f;
  o.contrabass.bowed_string.vel_to_speed = 0.370868f;
  o.contrabass.bowed_string.stribeck = 0.0524382f;
  o.contrabass.bowed_string.sympathetic = 0.00371787f;
  // The reference's held level settles 3.8 to 6.4 dB under its own attack peak and
  // then holds flat, so the sustain is that settle rather than a decay.
  o.contrabass.amp_env.attack_ms = 142.548f;
  o.contrabass.amp_env.decay_ms = 58.9522f;
  o.contrabass.amp_env.sustain = 0.579043f;
  o.contrabass.amp_env.release_ms = 109.666f;
  o.contrabass.bowed_string.attack_ms = 92.5436f;
  o.contrabass.bowed_string.bow_force = 0.806606f;
  o.contrabass.bowed_string.bow_position = 0.36863f;
  o.contrabass.bowed_string.damping = 0.516476f;
  o.contrabass.drift_rate_hz = 1.61494f;

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
  o.string_ensemble_1.amp_env.attack_ms = 110.233f;
  o.string_ensemble_1.cutoff_hz = 3136.31f;
  o.string_ensemble_1.bowed_string.polarization = 0.150685f;
  o.string_ensemble_1.bowed_string.sympathetic = 0.364805f;
  o.string_ensemble_1.bowed_string.rosin = 0.06f;  // twenty bows average the grit out
  o.string_ensemble_1.drift_cents = 9.6654f;
  o.string_ensemble_1.stereo_spread = 0.0631312f;
  o.string_ensemble_1.lfo_rate_hz = 1.87579f;
  o.string_ensemble_1.lfo_to_pitch_cents = 7.42545f;
  o.string_ensemble_1.amp_env.decay_ms = 45.3946f;
  o.string_ensemble_1.amp_env.release_ms = 849.558f;
  o.string_ensemble_1.amp_env.sustain = 0.838679f;
  o.string_ensemble_1.bowed_string.attack_ms = 37.4924f;
  o.string_ensemble_1.bowed_string.bow_force = 0.139013f;
  o.string_ensemble_1.bowed_string.bow_position = 0.296798f;
  o.string_ensemble_1.bowed_string.bow_speed = 1.0f;
  o.string_ensemble_1.bowed_string.brightness = 0.132531f;
  o.string_ensemble_1.bowed_string.damping = 0.196473f;
  o.string_ensemble_1.bowed_string.release_ms = 121.014f;
  o.string_ensemble_1.bowed_string.stribeck = 0.439863f;
  o.string_ensemble_1.drift_rate_hz = 1.69817f;
  o.string_ensemble_1.resonance_q = 1.39165f;
  o.string_ensemble_2 = o.string_ensemble_1;
  o.string_ensemble_2.amp_env.attack_ms = 240.332f;
  o.string_ensemble_2.bowed_string.attack_ms = 180.437f;
  o.string_ensemble_2.bowed_string.brightness = 0.116989f;
  o.string_ensemble_2.bowed_string.damping = 0.344621f;
  o.string_ensemble_2.cutoff_hz = 13464.2f;
  o.string_ensemble_2.drift_cents = 3.50183f;
  o.string_ensemble_2.amp_env.decay_ms = 38.483f;
  o.string_ensemble_2.amp_env.release_ms = 432.057f;
  o.string_ensemble_2.amp_env.sustain = 0.613685f;
  o.string_ensemble_2.bowed_string.bow_force = 0.790656f;
  o.string_ensemble_2.bowed_string.bow_position = 0.105649f;
  o.string_ensemble_2.bowed_string.bow_speed = 0.627663f;
  o.string_ensemble_2.bowed_string.release_ms = 110.201f;
  o.string_ensemble_2.bowed_string.stribeck = 0.778821f;
  o.string_ensemble_2.drift_rate_hz = 0.0354479f;
  o.string_ensemble_2.lfo_rate_hz = 0.365881f;
  o.string_ensemble_2.resonance_q = 0.688613f;

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
  o.soprano_sax.amp_env.attack_ms = 2.90908f;
  o.soprano_sax.amp_env.decay_ms = 36.0348f;
  o.soprano_sax.amp_env.release_ms = 25.3763f;
  o.soprano_sax.amp_env.sustain = 0.851871f;
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
  o.alto_sax = reed(true, 0.55f, 0.55f, 0.62f, 0.34f, 16.0f, 90.0f, 0.78f, 0.32f, 1.53f);
  // No capture: the setting is read off its two fitted neighbours.
  o.alto_sax.reed.closing_pressure = 2.4f;
  o.alto_sax.reed.flow_gain = 0.6f;
  o.alto_sax.cutoff_hz = 4500.0f;
  o.alto_sax.lfo_rate_hz = 5.2f;
  o.alto_sax.lfo_to_pitch_cents = 6.0f;
  o.alto_sax.reed.growl = 0.15f;
  o.alto_sax.reed.chiff = 0.6f;
  o.alto_sax.reed.reed_opening = 0.62f;
  o.alto_sax.reed.breath_noise = 0.3f;
  o.tenor_sax = reed(true, 0.60f, 0.50f, 0.56f, 0.36f, 20.0f, 100.0f, 0.78f, 0.36f, 1.602f);
  o.tenor_sax.reed.closing_pressure = 0.32753f;
  o.tenor_sax.reed.flow_gain = 0.815874f;
  o.tenor_sax.cutoff_hz = 2827.72f;
  o.tenor_sax.lfo_rate_hz = 18.3065f;
  o.tenor_sax.lfo_to_pitch_cents = 0.625687f;
  o.tenor_sax.reed.growl = 0.317241f;
  o.tenor_sax.reed.chiff = 0.531633f;
  o.tenor_sax.reed.breath_noise = 0.319254f;
  o.tenor_sax.amp_env.attack_ms = 31.8048f;
  o.tenor_sax.amp_env.release_ms = 269.651f;
  o.tenor_sax.amp_env.sustain = 0.796456f;
  o.tenor_sax.drift_rate_hz = 0.52401f;
  o.tenor_sax.reed.attack_ms = 3.44883f;
  o.tenor_sax.reed.breath_pressure = 0.529918f;
  o.tenor_sax.reed.chiff_ms = 23.6422f;
  o.tenor_sax.reed.damping = 0.853888f;
  o.tenor_sax.reed.release_ms = 265.076f;
  o.tenor_sax.reed.vel_to_breath = 0.605556f;
  o.tenor_sax.resonance_q = 0.550554f;
  o.tenor_sax.stereo_spread = 0.0f;
  o.baritone_sax = reed(true, 0.60f, 0.50f, 0.5f, 0.40f, 26.0f, 120.0f, 0.78f, 0.40f, 1.602f);
  o.baritone_sax.reed.closing_pressure = 2.4f;
  o.baritone_sax.reed.flow_gain = 0.7f;
  o.baritone_sax.cutoff_hz = 3800.0f;
  o.baritone_sax.lfo_rate_hz = 4.8f;
  o.baritone_sax.lfo_to_pitch_cents = 4.0f;
  o.baritone_sax.reed.growl = 0.18f;
  o.baritone_sax.reed.chiff = 0.6f;
  o.oboe = reed(true, 0.80f, 0.35f, 0.74f, 0.30f, 18.0f, 70.0f, 0.62f, 0.30f, 0.6f);
  o.oboe.cutoff_hz = 3143.15f;
  o.oboe.lfo_rate_hz = 4.87846f;
  o.oboe.lfo_to_pitch_cents = 0.818501f;
  o.oboe.amp_env.attack_ms = 14.9192f;
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
  o.english_horn = reed(true, 0.70f, 0.40f, 0.64f, 0.34f, 24.0f, 90.0f, 0.64f, 0.34f, 0.6f);
  o.english_horn.cutoff_hz = 7494.24f;
  o.english_horn.lfo_rate_hz = 0.341075f;
  o.english_horn.lfo_to_pitch_cents = 0.772132f;
  o.english_horn.amp_env.attack_ms = 1.88055f;
  o.english_horn.amp_env.decay_ms = 233.7f;
  o.english_horn.amp_env.release_ms = 15.7681f;
  o.english_horn.amp_env.sustain = 0.39408f;
  o.english_horn.drift_rate_hz = 0.136536f;
  o.english_horn.reed.attack_ms = 7.41553f;
  o.english_horn.reed.breath_noise = 0.109483f;
  o.english_horn.reed.breath_pressure = 0.729728f;
  o.english_horn.reed.brightness = 0.411548f;
  o.english_horn.reed.chiff = 0.410274f;
  o.english_horn.reed.chiff_ms = 8.32053f;
  o.english_horn.reed.damping = 0.82913f;
  o.english_horn.reed.reed_opening = 0.694686f;
  o.english_horn.reed.release_ms = 115.826f;
  o.english_horn.reed.vel_to_breath = 0.49538f;
  o.english_horn.resonance_q = 0.982102f;
  o.bassoon = reed(true, 0.65f, 0.45f, 0.5f, 0.40f, 30.0f, 120.0f, 0.68f, 0.40f, 2.808f);
  o.bassoon.reed.closing_pressure = 0.507749f;
  o.bassoon.reed.flow_gain = 0.628207f;
  o.bassoon.cutoff_hz = 3537.47f;
  o.bassoon.lfo_rate_hz = 14.7409f;
  o.bassoon.lfo_to_pitch_cents = 0.680254f;
  o.bassoon.amp_env.attack_ms = 32.2785f;
  o.bassoon.amp_env.release_ms = 40.9668f;
  o.bassoon.amp_env.sustain = 0.77613f;
  o.bassoon.drift_rate_hz = 0.683934f;
  o.bassoon.reed.attack_ms = 36.7159f;
  o.bassoon.reed.breath_noise = 0.62601f;
  o.bassoon.reed.breath_pressure = 0.489349f;
  o.bassoon.reed.brightness = 0.685527f;
  o.bassoon.reed.chiff = 0.505784f;
  o.bassoon.reed.chiff_ms = 1.5f;
  o.bassoon.reed.release_ms = 35.1109f;
  o.bassoon.reed.vel_to_breath = 0.156748f;
  o.bassoon.resonance_q = 1.23723f;
  o.clarinet = reed(false, 0.40f, 0.50f, 0.54f, 0.30f, 25.0f, 90.0f, 0.72f, 0.25f, 0.6f);
  o.clarinet.cutoff_hz = 4800.0f;
  o.clarinet.lfo_rate_hz = 5.0f;
  o.clarinet.lfo_to_pitch_cents = 2.5f;

  // Bag pipe (GM 109): the chanter — a stiff double reed on a conical bore fed
  // from the bag, so the pressure never varies and the note is never tongued.
  // vel_to_breath is zero because a piper has no dynamics, not as a voicing
  // choice. The drones are not voiced: one waveguide is one pipe.
  o.bag_pipe = reed(true, 0.82f, 0.32f, 0.86f, 0.24f, 30.0f, 60.0f, 0.86f, 0.22f, 0.55f);
  o.bag_pipe.cutoff_hz = 4796.17f;
  o.bag_pipe.reed.vel_to_breath = 0.0f;
  o.bag_pipe.reed.chiff = 0.503784f;
  o.bag_pipe.reed.breath_noise = 0.175717f;
  o.bag_pipe.amp_env.attack_ms = 7.9076f;
  o.bag_pipe.amp_env.decay_ms = 3.87963f;
  o.bag_pipe.amp_env.release_ms = 73.1983f;
  o.bag_pipe.amp_env.sustain = 0.725002f;
  o.bag_pipe.drift_rate_hz = 0.0886129f;
  o.bag_pipe.reed.attack_ms = 2.67067f;
  o.bag_pipe.reed.breath_pressure = 0.514945f;
  o.bag_pipe.reed.brightness = 0.34615f;
  o.bag_pipe.reed.chiff_ms = 22.643f;
  o.bag_pipe.reed.damping = 0.691793f;
  o.bag_pipe.reed.reed_opening = 0.781414f;
  o.bag_pipe.reed.reed_stiffness = 0.183843f;
  o.bag_pipe.resonance_q = 6.75404f;

  // Shanai (GM 111): a double reed on a wooden cone into a flared METAL bell,
  // which is where its edge comes from and why it is the one reed here on the
  // brass body rather than the wood tube. Played with a wide expressive slide,
  // so the pitch modulation is deep next to an orchestral double reed's.
  o.shanai = reed(true, 0.86f, 0.30f, 0.82f, 0.26f, 22.0f, 80.0f, 0.72f, 0.28f, 0.58f);
  o.shanai.body = BodyType::kBrassBell;
  o.shanai.cutoff_hz = 3308.58f;
  o.shanai.reed.chiff = 0.148253f;
  o.shanai.lfo_rate_hz = 3.98014f;
  o.shanai.lfo_to_pitch_cents = 1.5f;
  o.shanai.amp_env.attack_ms = 2.10191f;
  o.shanai.amp_env.decay_ms = 532.262f;
  o.shanai.amp_env.release_ms = 42.667f;
  o.shanai.amp_env.sustain = 0.663066f;
  o.shanai.drift_rate_hz = 1.3923f;
  o.shanai.reed.attack_ms = 1.81075f;
  o.shanai.reed.breath_noise = 0.0763358f;
  o.shanai.reed.breath_pressure = 0.165558f;
  o.shanai.reed.brightness = 0.848084f;
  o.shanai.reed.chiff_ms = 2.83683f;
  o.shanai.reed.damping = 0.760238f;
  o.shanai.reed.reed_opening = 0.314303f;
  o.shanai.reed.reed_stiffness = 0.901321f;
  o.shanai.reed.release_ms = 46.0852f;
  o.shanai.resonance_q = 3.12944f;

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
  // Bell radiation, and the voice lowpass that goes with it: the highpass lifts
  // the whole band above the flare cutoff, so the top of the range needs closing
  // in by roughly the same amount to keep the model's tone-to-noise where it was.
  // The four values are fitted against their references; the cutoffs order as a
  // bell's flare does, the horn widest and the trumpet narrowest.
  o.trumpet = brass(false, 0.55f, 0.30f, 0.75f, 0.28f, 12.0f, 80.0f, 0.88f, 0.50f, 0.90f);
  o.trumpet.cutoff_hz = 2400.0f;
  o.trumpet.brass.bell_radiation_hz = 1900.0f;
  o.trumpet.brass.brassiness = 0.55f;
  o.trumpet.brass.cuivre_dynamics = 0.7f;
  o.trumpet.lfo_rate_hz = 5.5f;
  o.trumpet.lfo_to_pitch_cents = 4.0f;
  o.trombone = brass(false, 0.48f, 0.45f, 0.85f, 0.32f, 26.0f, 100.0f, 0.85f, 0.0f, 0.92f);
  o.trombone.cutoff_hz = 978.041f;
  o.trombone.brass.bell_radiation_hz = 1346.68f;
  o.trombone.brass.brassiness = 0.950735f;
  o.trombone.brass.cuivre_dynamics = 0.442127f;
  o.trombone.lfo_rate_hz = 2.47118f;
  o.trombone.lfo_to_pitch_cents = 0.597068f;
  o.trombone.amp_env.attack_ms = 26.5016f;
  o.trombone.amp_env.release_ms = 99.453f;
  o.trombone.amp_env.sustain = 0.743293f;
  o.trombone.brass.attack_ms = 5.5069f;
  o.trombone.brass.breath_pressure = 0.735302f;
  o.trombone.brass.brightness = 0.245804f;
  o.trombone.brass.chiff = 0.287488f;
  o.trombone.brass.chiff_ms = 7.27184f;
  o.trombone.brass.damping = 0.99069f;
  o.trombone.brass.lip_damping = 1.0f;
  o.trombone.brass.lip_tension = 0.900029f;
  o.trombone.brass.release_ms = 31.8924f;
  o.trombone.brass.vel_to_breath = 0.656219f;
  o.trombone.drift_rate_hz = 0.506794f;
  o.trombone.resonance_q = 1.30183f;
  o.tuba = brass(true, 0.42f, 0.70f, 0.38f, 0.42f, 40.0f, 140.0f, 0.88f, 0.0f, 0.92f);
  // The tuba and the muted trumpet have no reference; their flare cutoffs follow
  // the bore, below the trombone's and level with the trumpet's respectively.
  o.tuba.cutoff_hz = 1200.0f;
  o.tuba.brass.bell_radiation_hz = 500.0f;
  o.tuba.brass.brassiness = 0.25f;
  o.tuba.brass.cuivre_dynamics = 0.5f;
  o.tuba.lfo_to_pitch_cents = 1.5f;
  // The muted trumpet plays through the real mute model instead of the old
  // dimmed-brightness fake.
  // The mute already attenuates, so this voice needs less of the radiation
  // make-up than the rest of the family — 2.7 dB of it comes back out here.
  o.muted_trumpet = brass(false, 0.58f, 0.35f, 0.62f, 0.30f, 16.0f, 75.0f, 0.80f, 0.0f, 0.60f);
  o.muted_trumpet.cutoff_hz = 526.781f;
  o.muted_trumpet.brass.bell_radiation_hz = 692.571f;
  o.muted_trumpet.brass.brassiness = 0.242353f;
  o.muted_trumpet.brass.cuivre_dynamics = 0.747426f;
  o.muted_trumpet.brass.mute = 0.256358f;
  o.muted_trumpet.lfo_rate_hz = 0.459626f;
  o.muted_trumpet.lfo_to_pitch_cents = 0.156186f;
  o.muted_trumpet.amp_env.attack_ms = 54.9812f;
  o.muted_trumpet.amp_env.decay_ms = 281.136f;
  o.muted_trumpet.amp_env.release_ms = 52.7576f;
  o.muted_trumpet.amp_env.sustain = 0.790166f;
  o.muted_trumpet.brass.attack_ms = 7.69449f;
  o.muted_trumpet.brass.breath_pressure = 0.231571f;
  o.muted_trumpet.brass.chiff = 0.0574322f;
  o.muted_trumpet.brass.chiff_ms = 37.0073f;
  o.muted_trumpet.brass.damping = 0.563597f;
  o.muted_trumpet.brass.lip_damping = 0.926993f;
  o.muted_trumpet.brass.lip_tension = 0.798806f;
  o.muted_trumpet.brass.release_ms = 12.0762f;
  o.muted_trumpet.brass.vel_to_breath = 0.198345f;
  o.muted_trumpet.resonance_q = 1.11169f;
  o.french_horn = brass(true, 0.50f, 0.55f, 0.48f, 0.34f, 30.0f, 110.0f, 0.82f, 0.0f, 0.88f);
  o.french_horn.cutoff_hz = 1600.0f;
  o.french_horn.brass.bell_radiation_hz = 700.0f;
  o.french_horn.brass.brassiness = 0.3f;
  o.french_horn.brass.cuivre_dynamics = 0.6f;
  o.french_horn.lfo_to_pitch_cents = 1.5f;

  // Brass Section (GM 61): the lip reed in section, on the same argument as the
  // string ensemble — the spread is what makes it a section. A section tongues
  // together far less precisely than a soloist, so the speech is slower and
  // softer-edged while the summed blare stays.
  o.brass_section = brass(false, 0.50f, 0.42f, 0.72f, 0.32f, 45.0f, 130.0f, 0.85f, 0.35f, 0.85f);
  o.brass_section.amp_env.attack_ms = 67.2627f;
  o.brass_section.cutoff_hz = 824.427f;
  o.brass_section.brass.bell_radiation_hz = 5447.92f;
  o.brass_section.brass.brassiness = 0.59236f;
  o.brass_section.brass.cuivre_dynamics = 0.998511f;
  o.brass_section.brass.chiff = 0.203084f;
  o.brass_section.drift_cents = 1.54191f;
  o.brass_section.stereo_spread = 0.5512f;
  o.brass_section.lfo_rate_hz = 1.09851f;
  o.brass_section.lfo_to_pitch_cents = 0.934083f;
  o.brass_section.amp_env.decay_ms = 0.813926f;
  o.brass_section.amp_env.release_ms = 38.0346f;
  o.brass_section.amp_env.sustain = 0.714046f;
  o.brass_section.brass.attack_ms = 15.993f;
  o.brass_section.brass.breath_pressure = 0.847405f;
  o.brass_section.brass.brightness = 0.945981f;
  o.brass_section.brass.chiff_ms = 5.41661f;
  o.brass_section.brass.damping = 0.288396f;
  o.brass_section.brass.lip_damping = 0.803849f;
  o.brass_section.brass.lip_tension = 0.944813f;
  o.brass_section.brass.release_ms = 118.929f;
  o.brass_section.brass.vel_to_breath = 0.169958f;
  o.brass_section.drift_rate_hz = 2.87174f;
  o.brass_section.resonance_q = 0.506819f;

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
  o.piccolo.amp_env.attack_ms = 3.69343f;
  o.piccolo.flute.overblow = 0.294968f;
  o.piccolo.amp_env.release_ms = 35.4431f;
  o.piccolo.cutoff_hz = 2655.32f;
  o.piccolo.drift_rate_hz = 0.0349556f;
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
  o.blown_bottle.amp_env.release_ms = 45.2492f;
  o.blown_bottle.amp_env.sustain = 0.570843f;
  o.blown_bottle.flute.damping = 0.671722f;
  o.blown_bottle.flute.end_reflection = 1.0f;
  o.blown_bottle.flute.jet_reflection = 0.162748f;
  o.blown_bottle.flute.release_ms = 95.402f;
  o.shakuhachi = flute(0.52f, 0.48f, 0.35f, 0.55f, 0.30f, 0.20f, 0.58f, 0.85f);
  o.shakuhachi.flute.vortex = 0.104581f;
  o.shakuhachi.body = BodyType::kWoodTube;
  o.shakuhachi.body_mix = 0.276409f;
  o.shakuhachi.amp_env.attack_ms = 4.36147f;
  o.shakuhachi.amp_env.release_ms = 83.7378f;
  o.shakuhachi.amp_env.sustain = 0.624315f;
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
  o.tin_whistle = flute(0.48f, 0.70f, 0.28f, 0.10f, 0.45f, 0.04f, 0.62f, 0.80f);
  o.tin_whistle.flute.overblow = 0.25f;
  o.ocarina = flute(0.50f, 0.40f, 0.55f, 0.15f, 0.30f, 0.06f, 0.55f, 0.85f);
  o.ocarina.amp_env.attack_ms = 1.04753f;
  o.ocarina.amp_env.decay_ms = 11.2384f;
  o.ocarina.amp_env.release_ms = 7.68608f;
  o.ocarina.amp_env.sustain = 0.32307f;
  o.ocarina.cutoff_hz = 3667.94f;
  o.ocarina.drift_rate_hz = 0.0312387f;
  o.ocarina.drive = 0.0f;
  o.ocarina.flute.attack_ms = 2.303f;
  o.ocarina.flute.breath_pressure = 0.719247f;
  o.ocarina.flute.brightness = 0.302582f;
  o.ocarina.flute.chiff = 0.14569f;
  o.ocarina.flute.chiff_ms = 31.597f;
  o.ocarina.flute.end_reflection = 0.877128f;
  o.ocarina.flute.jet_ratio = 0.518187f;
  o.ocarina.flute.jet_reflection = 0.141403f;
  o.ocarina.flute.release_ms = 224.812f;
  o.ocarina.flute.vel_to_breath = 0.21608f;
  o.ocarina.resonance_q = 0.5f;
}

}  // namespace sonare::midi::synth::detail
