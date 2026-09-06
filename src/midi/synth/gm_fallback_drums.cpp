#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/patch_sections.h"
#include "midi/synth/patch_tuning.h"
#include "util/tunable.h"

namespace sonare::midi::synth::detail {
namespace {

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

constexpr DrumPatches build_drum_patches() noexcept {
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
  d.kick.amp_env = fallback_env(0.5f, 220.0f, 0.0f, 60.0f);
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
  d.snare.amp_env = fallback_env(0.5f, 250.0f, 0.0f, 80.0f);
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

  // Hi-hats: high-passed noise shimmer, closed short / open ringing, over the
  // low plate modes of the pair. A hi-hat is two cymbals and radiates like one:
  // the measured kit peaks at 315 Hz on the closed hat and at 280 on the pedal,
  // 20 dB over the 1 kHz valley above it, and a voice built only from
  // high-passed noise has no way to put anything there. The ratios are the
  // cymbal archetype's, because this is the same object; what tells a hat from
  // a crash is that its plate is small, its modes die in tens of milliseconds
  // under the chick, and the pair damps each other.
  d.closed_hat = piece;
  d.closed_hat.amp_env = fallback_env(0.5f, 90.0f, 0.0f, 40.0f);
  d.closed_hat.percussion.num_modes = 3;
  d.closed_hat.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 0.0f, 0.0f, 0.0f};
  d.closed_hat.percussion.base_freq_hz = 300.0f;
  d.closed_hat.percussion.mode_decay_s = 0.05f;
  d.closed_hat.percussion.tone_gain = 1.4f;
  d.closed_hat.percussion.noise_gain = 1.0f;
  d.closed_hat.percussion.noise_decay_ms = 35.0f;
  d.closed_hat.percussion.noise_cutoff_hz = 7500.0f;
  d.closed_hat.percussion.noise_output = SynthFilterOutput::kHighpass;
  // The plate is what makes a hat sound like metal rather than like a small
  // drum. Three ring modes are a bar, not a cymbal, and the difference is
  // countable: over a slice of the closed hat's aftersound the sampled kit
  // resolves 40 separate resonances between 2 and 4 kHz and 71 between 4 and 8,
  // which is a field rather than a bank, and no gain on three modes can make
  // three into forty. Driving the bank instead only concentrates the energy on
  // the partials it already had. The network answers the same strike with a
  // thousand inharmonic partials in that band without touching the level of
  // anything. A closed pair is small and damps itself, so its lowest partial is
  // high and its ring is short.
  d.closed_hat.percussion.plate_gain = 0.8f;
  d.closed_hat.percussion.plate_t60_s = 0.7f;
  d.closed_hat.percussion.plate_hf_ratio = 0.8f;
  d.closed_hat.percussion.plate_low_hz = 500.0f;
  d.closed_hat.gain = 0.5f;
  // Open: nothing damps the pair, so the same plate rings an order of magnitude
  // longer and a little higher — the two cymbals are no longer loading each
  // other.
  d.open_hat = d.closed_hat;
  d.open_hat.amp_env = fallback_env(0.5f, 550.0f, 0.0f, 150.0f);
  d.open_hat.percussion.base_freq_hz = 330.0f;
  d.open_hat.percussion.mode_decay_s = 0.35f;
  d.open_hat.percussion.tone_gain = 4.0f;
  d.open_hat.percussion.noise_decay_ms = 350.0f;
  // Nothing is loading the pair, so the same plate rings far longer and holds
  // its top; the plate is a little larger open than closed because the two
  // cymbals are no longer clamped together.
  d.open_hat.percussion.plate_t60_s = 2.4f;
  d.open_hat.percussion.plate_hf_ratio = 0.85f;
  d.open_hat.percussion.plate_low_hz = 420.0f;

  // Toms: note-tracked membrane (full Rayleigh set) with a pitch drop.
  d.tom = piece;
  d.tom.amp_env = fallback_env(0.5f, 400.0f, 0.0f, 120.0f);
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
  d.cymbal.amp_env = fallback_env(0.5f, 1400.0f, 0.0f, 400.0f);
  d.cymbal.percussion.num_modes = 4;
  d.cymbal.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 2.15f, 0.0f, 0.0f};
  d.cymbal.percussion.base_freq_hz = 3600.0f;
  d.cymbal.percussion.mode_decay_s = 1.1f;
  d.cymbal.percussion.tone_gain = 0.25f;
  d.cymbal.percussion.noise_gain = 0.9f;
  d.cymbal.percussion.noise_decay_ms = 900.0f;
  // The wash is a band centred where the plate speaks, not a corner it speaks
  // above. Every cymbal in the measured kit peaks between 2 and 4 kHz and falls
  // away on both sides of that - a ride is 24 dB down at 1 kHz and 48 dB down
  // at 8 - and a high-pass under a ceiling is a plateau between the two, which
  // is the one shape that cannot be fitted to that. It measured as a model
  // within a couple of dB where the reference peaked and 9 to 25 dB over it in
  // the valley below and the roll-off above.
  d.cymbal.percussion.noise_cutoff_hz = 3000.0f;
  d.cymbal.percussion.noise_q = 1.0f;
  d.cymbal.percussion.noise_output = SynthFilterOutput::kBandpass;
  // Nonlinear shimmer: the inharmonic modes pump a high wash that swells after
  // the crash and rides the long ring -- the cymbal "bloom" a static bank
  // lacks.
  d.cymbal.percussion.shimmer = 6.0f;
  d.cymbal.percussion.shimmer_attack_ms = 60.0f;
  d.cymbal.percussion.shimmer_cutoff_hz = 9000.0f;
  // Dense inharmonic plate. The four ring modes above give a cymbal its pitch
  // centre and the wash gives it its noise, but neither gives it the field of
  // hundreds of partials between them, which is what the ear reads as metal.
  // Per-plate values below; the archetype's are a 16 inch crash's.
  d.cymbal.percussion.plate_gain = 0.8f;
  d.cymbal.percussion.plate_t60_s = 1.4f;
  d.cymbal.percussion.plate_hf_ratio = 0.85f;
  d.cymbal.percussion.plate_low_hz = 220.0f;
  d.cymbal.gain = 0.5f;

  // Everything else (claps, shakers, latin percussion): short noise burst
  // with a faint note-tracked knock.
  d.percussion = piece;
  d.percussion.amp_env = fallback_env(0.5f, 200.0f, 0.0f, 80.0f);
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

// Per-note GM/GS drum map (keys 27..87): each key is a distinct instrument
// built from a mechanism archetype (fixed-pitch membrane / struck wood / struck
// metal / whistle / noise) plus a fixed tuning, on top of the shared kit
// archetypes above. Unmapped keys fall back to the generic short burst so every
// drum key stays audible.
//
// A piece's `gain` is how loud it stands against the rest of the kit, and that
// balance is not something one reference can settle: two commercial GM kits
// measured against each other disagree by 7 dB RMS over the map and by 25 on a
// tambourine, because where a shaker sits under a snare is a mix decision as
// much as an instrument's property. So a gain here is moved only when this kit
// falls outside the range the two of them span, and only as far as the nearer
// of the two — a value some real kit actually uses, taking no side on which.
// A key is moved only where the peak and the RMS reading agree on the
// direction, and by the smaller of the two; a decay or a brightness moving
// takes the level with it, so the balance is re-measured after any fit. Four
// keys sit at `gain`'s ceiling of 4 and still measure under the nearer kit —
// 40 by 9.4 dB, 36 by 3.5, 37 by 2.5 and 52 by 0.4 — which is a voice too
// quiet rather than a gain too low. The lever itself is exact: `gain` applies
// after the drive, the filter and the envelope, so doubling it moves the
// rendered peak +6.02 dB at every velocity and moves no other dimension.
SONARE_TUNED_CONSTEXPR std::array<NativeSynthPatch, 128> build_drum_note_table() noexcept {
  const DrumPatches d = build_drum_patches();
  std::array<NativeSynthPatch, 128> t{};

  NativeSynthPatch piece{};
  piece.mode = SynthEngineMode::kPercussion;
  piece.one_shot = true;
  piece.cutoff_hz = 20000.0f;

  // Fixed-pitch membrane (conga/bongo/timbale/surdo): unlike the key-tracked
  // toms, GM pins one head frequency per key.
  auto make_membrane = [&](float base_hz, float decay_s, float drop, float shell_hz, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = fallback_env(0.5f, decay_s * 1000.0f + 120.0f, 0.0f, 40.0f);
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
    p.amp_env = fallback_env(0.3f, decay_s * 1000.0f + 40.0f, 0.0f, 20.0f);
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
    p.amp_env = fallback_env(0.3f, decay_s * 1000.0f + 60.0f, 0.0f, 30.0f);
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
    p.amp_env = fallback_env(3.0f, decay_s * 1000.0f + 40.0f, 0.0f, 25.0f);
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

  // Shaker (PhISEM): a burst of stochastic bead collisions voiced by the band
  // they radiate directly — maracas, cabasa, shaker, tambourine, vibraslap. The
  // body they sound inside is a separate resonance, set per instrument below.
  auto make_shaker = [&](float beans, float energy_ms, float res_hz, float res_q, float gain) {
    NativeSynthPatch p = piece;
    p.amp_env = fallback_env(0.5f, energy_ms + 200.0f, 0.0f, 40.0f);
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
    p.amp_env = fallback_env(0.5f, energy_ms + 200.0f, 0.0f, 40.0f);
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

  // Struck cymbal: the shared wash-plus-ring-mode archetype, told apart by the
  // four things that separate one cymbal from another. `tone_gain` sets how
  // defined the stick attack is against the wash, `noise_decay_ms` how long
  // the wash lasts, `mode_decay_s` how long the plate rings under it, and
  // `shimmer` how much the nonlinear bloom swells after the strike. In those
  // terms a ride is a crash with the attack brought forward and the bloom taken
  // away, a splash is a crash that stops, and a china is a crash whose ring
  // modes are detuned until none of them is a pitch.
  //
  // The plate lengths are the physical ones - a splash is 8 to 10 inches and a
  // ride 20 to 22 - so those are starting points a calibration can reach rather
  // than fitted values; the wash corners below have since been measured. They
  // exist per piece because a shared patch cannot be calibrated at all: every
  // knob moved for the ride moved the crash by the same amount.
  auto make_cymbal = [&](float base_hz, float mode_decay_s, float tone_gain, float noise_decay_ms,
                         float noise_cutoff_hz, float shimmer, float length_ms, float gain) {
    NativeSynthPatch p = d.cymbal;
    // Release is unused by a one-shot voice in normal play; `length_ms` is what
    // decides how long the piece sounds.
    p.amp_env = fallback_env(0.5f, length_ms, 0.0f, length_ms * 0.29f);
    p.percussion.base_freq_hz = base_hz;
    p.percussion.mode_decay_s = mode_decay_s;
    p.percussion.tone_gain = tone_gain;
    p.percussion.noise_decay_ms = noise_decay_ms;
    p.percussion.noise_cutoff_hz = noise_cutoff_hz;
    p.percussion.shimmer = shimmer;
    p.gain = gain;
    return p;
  };

  // Hand clap: a dense band-passed noise burst.
  NativeSynthPatch clap = piece;
  clap.amp_env = fallback_env(0.5f, 120.0f, 0.0f, 40.0f);
  clap.percussion.noise_gain = 1.0f;
  clap.percussion.noise_decay_ms = 90.0f;
  clap.percussion.noise_cutoff_hz = 1300.0f;
  clap.percussion.noise_q = 1.2f;
  clap.percussion.noise_output = SynthFilterOutput::kBandpass;
  clap.gain = 0.7f;

  // Default every key to the generic short burst (keeps unmapped keys audible;
  // also the current home of the not-yet-built stochastic shakers/scrapers).
  for (auto& p : t) p = d.percussion;

  // --- kit archetypes ---
  t[35] = d.kick;
  t[35].amp_env.attack_ms = 0.651532f;
  t[35].amp_env.decay_ms = 73.3945f;
  t[35].amp_env.sustain = 0.876601f;
  t[35].cutoff_hz = 22000.0f;
  t[35].drive = 0.133416f;
  t[35].percussion.contact = 0.454382f;
  t[35].percussion.mode_decay_s = 0.0391114f;
  t[35].percussion.mode_ratios[0] = 0.857872f;
  t[35].percussion.mode_ratios[1] = 5.5857f;
  t[35].percussion.noise_cutoff_hz = 297.277f;
  t[35].percussion.noise_decay_ms = 90.3274f;
  t[35].percussion.noise_gain = 1.41687f;
  t[35].percussion.noise_q = 1.27328f;
  t[35].percussion.pitch_drop = 2.55066f;
  t[35].percussion.pitch_drop_ms = 7.83614f;
  t[35].percussion.shell_mix = 0.686431f;
  t[35].percussion.shell_t60_s[0] = 0.417702f;
  t[35].percussion.shell_weight[0] = 3.23221f;
  t[35].percussion.strike_r = 0.354637f;
  t[35].percussion.strike_theta = 0.85586f;
  t[35].percussion.tone_gain = 0.180484f;
  t[35].percussion.wire_buzz = 2.09007f;
  t[35].stereo_spread = 0.187262f;
  t[35].gain = 2.7519f;
  t[36] = d.kick;
  t[36].gain = 4.0f;
  t[46] = d.open_hat;
  // The six toms start from one patch because they are key-tracked — the struck
  // key sets the head frequency — and then part company below, each fitted
  // against its own captured drum.
  t[41] = t[43] = t[45] = t[47] = t[48] = t[50] = d.tom;
  t[47].amp_env.attack_ms = 1.5172f;
  t[47].amp_env.decay_ms = 50.0f;
  t[47].amp_env.sustain = 0.125947f;
  t[47].cutoff_hz = 4404.95f;
  t[47].drive = 0.356028f;
  t[47].percussion.contact = 0.347392f;
  t[47].percussion.mode_decay_s = 0.357981f;
  t[47].percussion.mode_ratios[0] = 0.920847f;
  t[47].percussion.mode_ratios[1] = 0.661637f;
  t[47].percussion.mode_ratios[2] = 7.8434f;
  t[47].percussion.mode_ratios[3] = 3.75206f;
  t[47].percussion.mode_ratios[4] = 2.18956f;
  t[47].percussion.noise_cutoff_hz = 228.362f;
  t[47].percussion.noise_decay_ms = 4.75026f;
  t[47].percussion.noise_gain = 0.578683f;
  t[47].percussion.noise_q = 1.81726f;
  t[47].percussion.num_modes = 1;
  t[47].percussion.pitch_drop = 1.9615f;
  t[47].percussion.pitch_drop_ms = 14.7123f;
  t[47].percussion.plate_gain = 0.767737f;
  t[47].percussion.shell_mix = 0.549229f;
  t[47].percussion.shell_num_modes = 3;
  t[47].percussion.shell_t60_s[0] = 0.0528995f;
  t[47].percussion.shell_weight[0] = 0.464666f;
  t[47].percussion.shell_weight[1] = 2.8029f;
  t[47].percussion.strike_r = 0.876996f;
  t[47].percussion.strike_theta = 0.638249f;
  t[47].percussion.tone_gain = 2.87243f;
  t[47].percussion.wire_buzz = 0.888697f;
  t[47].resonance_q = 1.23387f;
  t[47].stereo_spread = 0.733672f;
  t[47].gain = 1.7579f;
  t[45].amp_env.attack_ms = 0.412972f;
  t[45].amp_env.decay_ms = 39.0723f;
  t[45].amp_env.sustain = 0.218642f;
  t[45].cutoff_hz = 439.419f;
  t[45].drive = 1.0f;
  t[45].percussion.contact = 0.859948f;
  t[45].percussion.mode_decay_s = 0.136273f;
  t[45].percussion.mode_ratios[0] = 0.119677f;
  t[45].percussion.mode_ratios[1] = 2.3049f;
  t[45].percussion.mode_ratios[2] = 0.523409f;
  t[45].percussion.mode_ratios[3] = 1.46114f;
  t[45].percussion.mode_ratios[4] = 9.04296f;
  t[45].percussion.noise_cutoff_hz = 120.322f;
  t[45].percussion.noise_decay_ms = 7.01112f;
  t[45].percussion.noise_gain = 1.88579f;
  t[45].percussion.noise_q = 1.55848f;
  t[45].percussion.num_modes = 3;
  t[45].percussion.pitch_drop = 1.1347f;
  t[45].percussion.pitch_drop_ms = 287.935f;
  t[45].percussion.plate_gain = 2.70378f;
  t[45].percussion.shell_mix = 0.493173f;
  t[45].percussion.shell_num_modes = 2;
  t[45].percussion.shell_t60_s[0] = 0.699765f;
  t[45].percussion.shell_t60_s[1] = 0.130808f;
  t[45].percussion.shell_weight[0] = 1.10919f;
  t[45].percussion.shell_weight[1] = 0.0f;
  t[45].percussion.strike_r = 0.368635f;
  t[45].percussion.strike_theta = 0.672819f;
  t[45].percussion.tone_gain = 2.68692f;
  t[45].percussion.wire_buzz = 0.695567f;
  t[45].resonance_q = 0.5f;
  t[45].stereo_spread = 0.525785f;
  t[45].percussion.contact_ms = 0.112507f;
  t[45].percussion.plate_hf_ratio = 0.64863f;
  t[45].percussion.plate_low_hz = 158.424f;
  t[45].percussion.plate_t60_s = 1.52302f;
  t[45].percussion.shell_weight[2] = 0.227623f;
  t[45].percussion.tone_direct = 0.534888f;
  t[45].percussion.wire_threshold = 3.31008f;
  t[45].gain = 1.8134f;
  t[50].amp_env.attack_ms = 1.13446f;
  t[50].amp_env.decay_ms = 60.7551f;
  t[50].cutoff_hz = 9241.57f;
  t[50].percussion.contact = 1.20857f;
  t[50].percussion.mode_decay_s = 1.4464f;
  t[50].percussion.mode_ratios[0] = 0.160763f;
  t[50].percussion.mode_ratios[1] = 0.0691419f;
  t[50].percussion.mode_ratios[2] = 1.78945f;
  t[50].percussion.mode_ratios[3] = 6.08448f;
  t[50].percussion.mode_ratios[4] = 9.12403f;
  t[50].percussion.noise_cutoff_hz = 105.803f;
  t[50].percussion.noise_decay_ms = 135.572f;
  t[50].percussion.noise_gain = 1.79751f;
  t[50].percussion.noise_q = 1.99401f;
  t[50].percussion.num_modes = 0;
  t[50].percussion.pitch_drop = 1.24141f;
  t[50].percussion.pitch_drop_ms = 328.792f;
  t[50].percussion.plate_gain = 2.84131f;
  t[50].percussion.shell_mix = 0.337229f;
  t[50].percussion.shell_num_modes = 0;
  t[50].percussion.shell_t60_s[0] = 0.0367571f;
  t[50].percussion.shell_weight[0] = 1.66629f;
  t[50].percussion.shell_weight[1] = 2.01492f;
  t[50].percussion.strike_r = 0.64041f;
  t[50].percussion.strike_theta = 0.427684f;
  t[50].percussion.tone_gain = 0.947679f;
  t[50].percussion.wire_buzz = 1.38156f;
  t[50].amp_env.sustain = 0.473151f;
  t[50].drive = 0.508784f;
  t[50].percussion.contact_ms = 0.159226f;
  t[50].percussion.plate_hf_ratio = 0.0911714f;
  t[50].percussion.plate_low_hz = 389.298f;
  t[50].percussion.plate_t60_s = 0.262769f;
  t[50].percussion.wire_threshold = 1.11887f;
  t[50].resonance_q = 1.95192f;
  t[50].percussion.shell_t60_s[1] = 0.0501966f;
  t[50].percussion.tone_direct = 0.162503f;
  t[48].amp_env.attack_ms = 0.0425854f;
  t[48].amp_env.decay_ms = 335.306f;
  t[48].amp_env.sustain = 0.477489f;
  t[48].cutoff_hz = 4590.22f;
  t[48].drive = 0.154601f;
  t[48].percussion.mode_decay_s = 0.0138718f;
  t[48].percussion.mode_ratios[0] = 0.281591f;
  t[48].percussion.mode_ratios[1] = 49.1507f;
  t[48].percussion.mode_ratios[2] = 16.1877f;
  t[48].percussion.mode_ratios[3] = 1.27713f;
  t[48].percussion.mode_ratios[4] = 1.64009f;
  t[48].percussion.noise_cutoff_hz = 122.338f;
  t[48].percussion.noise_decay_ms = 149.378f;
  t[48].percussion.noise_gain = 1.98885f;
  t[48].percussion.noise_q = 0.71776f;
  t[48].percussion.pitch_drop = 0.366487f;
  t[48].percussion.pitch_drop_ms = 55.3347f;
  t[48].percussion.plate_gain = 1.15659f;
  t[48].percussion.shell_mix = 0.521556f;
  t[48].percussion.shell_num_modes = 2;
  t[48].percussion.shell_t60_s[0] = 0.0942461f;
  t[48].percussion.shell_weight[1] = 2.80129f;
  t[48].percussion.strike_r = 0.163287f;
  t[48].percussion.strike_theta = 0.931919f;
  t[48].percussion.tone_gain = 3.39111f;
  t[48].percussion.wire_buzz = 1.13978f;
  t[48].percussion.contact = 0.442594f;
  t[48].percussion.num_modes = 1;
  t[48].percussion.plate_hf_ratio = 0.835957f;
  t[48].percussion.plate_low_hz = 265.555f;
  t[48].percussion.plate_t60_s = 0.490177f;
  t[48].percussion.shell_weight[0] = 0.771718f;
  t[48].percussion.tone_direct = 0.463782f;
  t[48].percussion.wire_threshold = 2.77501f;
  t[48].resonance_q = 0.5f;
  t[48].gain = 1.7916f;
  t[41].amp_env.attack_ms = 1.30323f;
  t[41].amp_env.decay_ms = 55.9659f;
  t[41].amp_env.sustain = 0.0983869f;
  t[41].cutoff_hz = 4923.92f;
  t[41].drive = 0.354172f;
  t[41].percussion.contact = 0.166701f;
  t[41].percussion.mode_decay_s = 0.922246f;
  t[41].percussion.mode_ratios[0] = 0.734396f;
  t[41].percussion.mode_ratios[1] = 0.958137f;
  t[41].percussion.mode_ratios[2] = 2.64812f;
  t[41].percussion.mode_ratios[3] = 1.51843f;
  t[41].percussion.mode_ratios[4] = 18.735f;
  t[41].percussion.noise_cutoff_hz = 246.394f;
  t[41].percussion.noise_decay_ms = 5.18107f;
  t[41].percussion.noise_gain = 0.820234f;
  t[41].percussion.noise_q = 5.84366f;
  t[41].percussion.num_modes = 2;
  t[41].percussion.pitch_drop = 1.90096f;
  t[41].percussion.pitch_drop_ms = 220.769f;
  t[41].percussion.plate_gain = 2.02608f;
  t[41].percussion.shell_mix = 0.300043f;
  t[41].percussion.shell_num_modes = 3;
  t[41].percussion.shell_t60_s[0] = 0.0433665f;
  t[41].percussion.shell_t60_s[1] = 0.0553053f;
  t[41].percussion.shell_weight[0] = 0.593744f;
  t[41].percussion.shell_weight[1] = 2.41562f;
  t[41].percussion.strike_r = 0.801913f;
  t[41].percussion.strike_theta = 0.367653f;
  t[41].percussion.tone_gain = 3.0687f;
  t[41].percussion.wire_buzz = 0.123627f;
  t[41].stereo_spread = 0.13347f;
  t[41].gain = 1.3631f;
  t[43].amp_env.attack_ms = 0.0214768f;
  t[43].amp_env.decay_ms = 58.724f;
  t[43].amp_env.sustain = 0.111286f;
  t[43].cutoff_hz = 708.889f;
  t[43].drive = 0.0186866f;
  t[43].percussion.contact = 0.489255f;
  t[43].percussion.mode_decay_s = 0.886665f;
  t[43].percussion.mode_ratios[0] = 1.18929f;
  t[43].percussion.mode_ratios[1] = 0.413419f;
  t[43].percussion.mode_ratios[2] = 2.88348f;
  t[43].percussion.mode_ratios[3] = 0.356884f;
  t[43].percussion.mode_ratios[4] = 15.7198f;
  t[43].percussion.noise_cutoff_hz = 701.536f;
  t[43].percussion.noise_decay_ms = 14.4177f;
  t[43].percussion.noise_gain = 1.85473f;
  t[43].percussion.noise_q = 7.35009f;
  t[43].percussion.num_modes = 3;
  t[43].percussion.pitch_drop = 0.606076f;
  t[43].percussion.pitch_drop_ms = 69.9541f;
  t[43].percussion.shell_mix = 0.144526f;
  t[43].percussion.shell_t60_s[0] = 0.0493077f;
  t[43].percussion.shell_t60_s[1] = 0.0111126f;
  t[43].percussion.shell_weight[0] = 0.426064f;
  t[43].percussion.shell_weight[1] = 2.20036f;
  t[43].percussion.strike_r = 0.456311f;
  t[43].percussion.strike_theta = 0.868581f;
  t[43].percussion.tone_gain = 1.72636f;
  t[43].percussion.wire_buzz = 0.281158f;
  t[43].resonance_q = 0.71466f;
  t[43].stereo_spread = 0.738875f;
  t[43].percussion.contact_ms = 0.0752723f;
  t[43].percussion.plate_gain = 2.6512f;
  t[43].percussion.shell_num_modes = 1;
  t[43].percussion.wire_cutoff_hz = 909.225f;
  t[43].percussion.wire_threshold = 0.752055f;
  t[43].gain = 1.246f;

  // --- cymbals ---
  //
  // Six keys, six plates. They cannot share one patch the way the toms do: a
  // cymbal patch pins `base_freq_hz`, so a shared one renders the same plate at
  // the same pitch on every key and only the noise seed tells them apart.
  //
  // The wash is high-passed, so its corner is what makes a plate dark or
  // bright: a lower corner lets more of the low-mid body through. The two
  // members of each pair are the two sizes a kit actually carries - a 16 inch
  // crash against an 18, a 20 inch ride against a 22 - so the larger of each is
  // darker, slower and longer.
  //
  // The corners are measured against the sampled kit, on the bands that
  // reference can resolve, and they land where the plate sizes say they should:
  // a splash speaks from 3.4 kHz up, a crash from around 1.2, and a ride - the
  // largest plate here and the one whose lowest modes are lowest - from 200 Hz.
  // Each is several times under the corner the archetype started with, which is
  // why every cymbal in the kit was reading 20 to 45 dB short across its whole
  // midrange while matching at the top.
  //                       base   ring  tone   wash  cutoff shimm   len   gain
  //
  // The lengths and washes are the measured kit's rather than the archetype's.
  // Every cymbal here sounded for a fraction of the time its reference does — a
  // crash fell 40 dB in 1.2 s against 3.6, a splash in 0.2 s against 2.0 — and
  // a plate cannot ring past the envelope that gates it, so the field the
  // network builds was being cut off before it was audible. The wash runs about
  // as long as the piece, because it is what keeps re-exciting the plate: a
  // struck cymbal is not one impulse into a resonator but a plate whose modes
  // keep trading energy, and a wash that stops leaves the partials to ring on
  // undisturbed, which reads as a bell rather than as a cymbal.
  t[49] = make_cymbal(3600.0f, 1.10f, 0.20f, 4000.0f, 3000.0f, 6.0f, 5000.0f, 0.50f);  // Crash 1
  t[57] = make_cymbal(2500.0f, 1.55f, 0.22f, 4500.0f, 2800.0f, 5.0f, 3360.0f, 0.52f);  // Crash 2
  // A ride is played on its shoulder with the tip of the stick, so what carries
  // is a defined ping over a wash kept short enough to stay out of its way; a
  // ride that blooms like a crash is a ride nobody can play time on.
  t[51] = make_cymbal(2800.0f, 2.20f, 0.96f, 3500.0f, 3000.0f, 1.0f, 6000.0f, 0.50f);    // Ride 1
  t[59] = make_cymbal(4750.0f, 2.80f, 0.70f, 4000.0f, 2600.0f, 0.6f, 7000.0f, 0.1495f);  // Ride 2
  t[55] = make_cymbal(5200.0f, 0.30f, 0.30f, 1500.0f, 4200.0f, 2.5f, 2500.0f, 0.45f);    // Splash
  t[52] = make_cymbal(2600.0f, 0.35f, 0.70f, 2500.0f, 2520.0f, 3.0f, 1600.0f, 0.55f);    // China
  // The china's upturned flange is what makes it trashy, and trashy is neither
  // dark nor bright: it concentrates the wash into one harsh band instead of
  // spreading it up the spectrum the way a flat plate does. That is a different
  // filter rather than a different corner - every other cymbal here high-passes
  // its wash, and moving the corner alone only ever slides the china between
  // the two crashes. Its partials are pulled off the plate ratios the others
  // share until nothing in the sound reads as a pitch, and it is those partials
  // rather than the wash that carry it, hence the high tone gain and the short,
  // abrupt ring.
  t[52].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[52].percussion.noise_q = 3.0091f;
  t[52].percussion.mode_ratios = {1.0f, 1.19f, 1.51f, 1.83f, 0.0f, 0.0f};

  // Each plate's own partial field. `plate_low_hz` is the lowest partial the
  // network places, so it is the piece's diameter read as a frequency - a 20
  // inch ride reaches lower than a 16 inch crash and far lower than an 8 inch
  // splash - and `plate_t60_s` is how long the plate itself rings, which is
  // bounded by the amplitude envelope's length above and so tracks it. The
  // china is the exception in both: its flange stiffens the plate, which takes
  // its lowest partial up rather than down for its size, and stops it early.
  t[49].percussion.plate_low_hz = 498.198f;
  t[49].percussion.plate_t60_s = 1.28054f;
  t[49].percussion.plate_gain = 0.0821527f;
  t[49].percussion.plate_hf_ratio = 0.151528f;
  t[57].percussion.plate_low_hz = 1037.06f;
  t[57].percussion.plate_t60_s = 1.4058f;
  t[57].percussion.plate_gain = 3.15282f;
  t[57].percussion.plate_hf_ratio = 0.583589f;
  t[57].percussion.shimmer_cutoff_hz = 1462.25f;
  t[51].percussion.plate_low_hz = 211.186f;
  t[51].percussion.plate_t60_s = 0.0817966f;
  t[51].percussion.plate_gain = 0.981567f;
  t[51].percussion.plate_hf_ratio = 0.925295f;
  t[51].percussion.mode_ratios[1] = 7.00082f;
  t[51].percussion.noise_q = 3.04653f;
  t[51].amp_env.sustain = 0.165936f;
  t[51].drive = 0.359081f;
  t[59].percussion.plate_low_hz = 390.472f;
  t[59].percussion.plate_t60_s = 1.64807f;
  t[59].percussion.plate_gain = 0.334388f;
  t[59].percussion.plate_hf_ratio = 0.921336f;
  t[59].percussion.mode_ratios[0] = 0.0654023f;
  t[59].percussion.noise_gain = 2.5936f;
  t[59].percussion.noise_q = 3.02876f;
  t[59].percussion.wire_buzz = 0.936704f;
  t[59].amp_env.sustain = 0.713879f;
  t[55].percussion.plate_low_hz = 262.282f;
  t[55].percussion.plate_t60_s = 0.128504f;
  t[55].percussion.plate_gain = 0.645203f;
  t[55].percussion.phisem_beans = 3.34772f;
  // The china's plate is at the top of its clamp. The search asked for more
  // than the range allows, which is a result and not a setting: either the
  // bound is wrong for a piece this small and stiff, or what it wants is not
  // more of this plate.
  t[52].percussion.plate_low_hz = 1907.68f;
  t[52].percussion.plate_t60_s = 0.148375f;
  t[52].percussion.plate_gain = 0.326908f;
  t[52].percussion.plate_hf_ratio = 0.458156f;
  t[52].key_track = 1.0f;
  t[52].resonance_q = 1.42262f;
  // Every plate needs a top as well as a bottom. Where the wash corners above
  // came from the reference's measured band edge, these come from the same
  // place: the band each piece still radiates in, above which the network would
  // otherwise answer a broadband strike with as much as it answers the notes
  // the piece is played on.
  //
  // Three bounds per piece, and they do different jobs. `plate_air_hz` is the
  // top of the band the network responds in; `noise_air_hz` is the top of the
  // wash, kept even though the wash is a band now, because a single pole pair
  // falls 6 dB per octave above its centre and a ride's reference falls 40 dB
  // in the octave and a third above its peak; `cutoff_hz` is the voice corner
  // over the sum. Removing any one of them was tried and measured: without the
  // ceiling the ride reads 20 dB over its reference at 8 kHz, and without the
  // plate it reads 29 dB over, so neither is the other's substitute.
  t[49].percussion.plate_air_hz = 2324.05f;
  t[49].percussion.noise_air_hz = 978.472f;
  t[49].cutoff_hz = 4301.52f;
  t[57].percussion.plate_air_hz = 12243.9f;
  t[57].percussion.noise_air_hz = 2967.21f;
  t[57].percussion.noise_q = 7.92907f;
  t[57].cutoff_hz = 367.758f;
  t[51].percussion.plate_air_hz = 15752.7f;
  t[51].percussion.noise_air_hz = 494.564f;
  t[51].cutoff_hz = 5239.88f;
  t[59].percussion.plate_air_hz = 6.06302f;
  t[59].percussion.noise_air_hz = 1295.74f;
  t[59].cutoff_hz = 3070.41f;
  t[55].percussion.plate_air_hz = 393.504f;
  t[55].percussion.noise_air_hz = 2408.96f;
  t[55].percussion.noise_cutoff_hz = 2620.58f;
  t[55].percussion.noise_q = 2.05623f;
  t[55].cutoff_hz = 3842.29f;
  t[52].percussion.plate_air_hz = 3319.08f;
  t[52].percussion.noise_air_hz = 2826.14f;
  t[52].cutoff_hz = 514.869f;

  // --- snares ---
  t[38] = d.snare;  // Acoustic Snare, the archetype
  // Electric Snare: tuned higher and gated shorter, with the shell body and the
  // wire rattle mostly gone - what a drum machine has instead of a snare is a
  // tight noise crack over a short pitched click.
  t[40] = d.snare;
  t[40].amp_env = fallback_env(0.5f, 170.0f, 0.0f, 60.0f);
  t[40].percussion.base_freq_hz = 220.0f;
  t[40].percussion.mode_decay_s = 0.258761f;
  t[40].percussion.noise_decay_ms = 99.8719f;
  t[40].percussion.noise_cutoff_hz = 5105.77f;
  t[40].percussion.shell_mix = 0.420333f;
  t[40].percussion.wire_buzz = 1.34637f;

  // --- hi-hats (mute group 1) ---
  t[42] = d.closed_hat;  // Closed Hi-Hat, the archetype
  // Pedal Hi-Hat: the foot closes the cymbals against each other rather than a
  // stick striking them, so the "chick" is duller, softer and slightly longer
  // than a stick-closed hat - and, sharing a patch with one, was neither.
  //
  // Fitted against the captured kit in two passes. The first could only reach
  // the top - the voice's own low-pass down to 4.2 kHz with the drive up to
  // hold the body - because the reference peaks at 315 Hz on a hump from 200 to
  // 400 that is the two cymbals clashing, and the voice had no low mode to put
  // there. The plate modes above are that mode; the second pass places them
  // lowest of the three hats and rings them longest, which is what a foot
  // closing the pair does against a stick striking it, and gives back some of
  // the noise the first pass had leaned on. The attack still sits at its clamp
  // against a reference that takes 12 to 18 ms to arrive.
  t[44] = d.closed_hat;
  // Pedal Hi-Hat, taking the same correction as the two it shares a mute group
  // with: a band rather than a corner, the waveshaper's low-mid replaced by the
  // plate mode it should have come from, a ceiling over the network, and the
  // gain the piece needs once it is no longer being carried by distortion.
  t[44].amp_env = fallback_env(25.0f, 38.9229f, 0.263723f, 40.0f);
  t[44].amp_env.delay_ms = 2.5f;
  t[44].cutoff_hz = 4160.9f;
  t[44].drive = 0.494827f;
  t[44].key_track = 0.1f;
  t[44].resonance_q = 3.25156f;
  t[44].percussion.base_freq_hz = 224.0f;
  t[44].percussion.mode_decay_s = 0.12384f;
  t[44].percussion.mode_ratios[1] = 0.185843f;
  t[44].percussion.mode_ratios[2] = 4.45821f;
  t[44].percussion.tone_gain = 1.92456f;
  t[44].percussion.pitch_drop = 0.483151f;
  t[44].percussion.pitch_drop_ms = 4.29143f;
  t[44].percussion.strike_r = 0.877276f;
  t[44].percussion.wire_buzz = 3.40029f;
  t[44].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[44].percussion.noise_cutoff_hz = 428.226f;
  t[44].percussion.noise_decay_ms = 28.6381f;
  t[44].percussion.noise_gain = 3.55515f;
  t[44].percussion.noise_q = 1.13559f;
  t[44].percussion.plate_gain = 3.7106f;
  t[44].percussion.plate_low_hz = 163.796f;
  t[44].percussion.plate_t60_s = 0.429089f;
  t[44].percussion.plate_air_hz = 4755.27f;
  t[44].gain = 0.2228f;

  // Hi-hats share mute group 1; the open hat gets a snappy choke fade (release
  // is unused by one-shot voices in normal play, so this stays bit-identical
  // there — it only governs how fast a closed/pedal strike cuts the open hat).
  t[42].percussion.exclusive_class = 1;
  // Closed Hi-Hat, voiced against the measured kit. The archetype's noise is
  // high-passed at 7.5 kHz, which puts the whole piece above where the
  // reference's energy ends: its band profile peaks at 4 kHz and is 57 dB down
  // by 12.5 kHz, while the model's peaked at 12.5 kHz and was at the -60 dB
  // floor below 630 Hz. The fitted band is a low corner under the voice's own
  // filter rather than a ceiling over it, which is what moves the peak rather
  // than only attenuating past it.
  //
  // The envelope, the wash length and the voice corner are the plate's rather
  // than the fit's. An 11 ms amplitude decay is shorter than the plate takes to
  // fill, so it gated the field off before it existed and left the strike
  // alone; the reference falls 20 dB in 205 ms and 60 dB in 665. The corner
  // comes up because a 2.6 kHz ceiling removes most of what a plate radiates:
  // at 12 kHz the piece holds 88 % of its energy above 2 kHz against the
  // reference's 53 %, and at 3.5 kHz it holds 57 %.
  //
  // The wash is a band and not a corner. A hi-hat's reference peaks at 4 kHz,
  // sits 17 dB below that at 1 kHz and is 24 dB down by 8 kHz — a resonance
  // with a floor on both sides — while a high-pass with a ceiling over it is a
  // plateau between the two, which is why the piece measured within a couple of
  // dB at 4 kHz and 12 dB over at 1 kHz and again at 8. Neither corner could
  // fix that, because the shape wanted is not the shape a corner makes.
  //
  // `drive` comes down with it. The waveshaper was carrying most of the piece's
  // low-mid — switching it off drops the 63 Hz band by 42 dB — and a distortion
  // product is not a body: it put the model 20 dB over its reference below
  // 100 Hz, where a hi-hat radiates nothing at all. What the low-mid should come
  // from is the plate mode, so it does: the mode bank moves onto the band the
  // reference peaks in, at a gain that makes it audible. It was not audible
  // before — at a 50 ms ring and a gain of 1.4, switching the whole mode bank
  // off moved its own band by 0.8 dB.
  //
  // It stays a knock and not a ring, and that distinction is not visible in a
  // band profile. The profile integrates the whole hit, so a body mode ringing
  // for 600 ms and one struck four times as hard and gone in 200 sum to the
  // same third-octave level — and the long one measures better, because it also
  // fills the 1 kHz valley the wash left. It also stops being a hi-hat: with the
  // body ringing under it, the share of energy above 2 kHz a tenth of a second
  // after the strike falls from 54 % to 29 % against a reference that holds
  // 53 %, and what is left is a small drum. Anything voiced against the band
  // profile alone can walk into that trade, so the hats are read on both.
  t[42].amp_env.attack_ms = 9.08185f;
  t[42].amp_env.decay_ms = 174.0f;
  t[42].amp_env.delay_ms = 1.0f;
  t[42].cutoff_hz = 3500.0f;
  t[42].drive = 0.375f;
  t[42].percussion.base_freq_hz = 393.75f;
  t[42].percussion.mode_decay_s = 0.2f;
  t[42].percussion.mode_ratios[1] = 1.072f;
  t[42].percussion.mode_ratios[2] = 1.032f;
  t[42].percussion.tone_gain = 4.0f;
  t[42].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[42].percussion.noise_cutoff_hz = 3800.0f;
  t[42].percussion.noise_decay_ms = 600.0f;
  t[42].percussion.noise_gain = 2.54656f;
  t[42].percussion.noise_q = 1.25f;
  t[42].percussion.plate_low_hz = 200.0f;
  t[42].percussion.plate_t60_s = 2.4f;
  t[42].percussion.plate_air_hz = 7000.0f;
  t[42].resonance_q = 2.0f;
  t[42].stereo_spread = 0.875561f;
  t[42].gain = 0.3464f;
  t[42].key_track = 0.1f;
  t[44].percussion.exclusive_class = 1;
  t[44].amp_env.decay_ms = 102.575f;
  t[44].amp_env.sustain = 0.0957462f;
  t[44].percussion.phisem_beans = 0.00375547f;
  t[44].percussion.shimmer = 9.93934f;
  t[44].percussion.shimmer_cutoff_hz = 197.035f;
  t[44].percussion.mode_alpha[2] = 20.5424f;
  t[44].amp_env.attack_ms = 21.427f;
  t[44].percussion.contact = 0.0926551f;
  t[44].percussion.mode_ratios[0] = 0.0251234f;
  t[44].percussion.num_modes = 0;
  t[44].percussion.phisem_energy_ms = 396.123f;
  t[44].percussion.phisem_sound_ms = 0.710561f;
  t[44].percussion.plate_hf_ratio = 0.490005f;
  t[44].percussion.shimmer_attack_ms = 120.291f;
  t[44].percussion.strike_theta = 0.0165314f;
  t[44].percussion.tone_direct = 0.454577f;
  t[44].percussion.wire_cutoff_hz = 4439.4f;
  t[44].percussion.wire_threshold = 0.717138f;
  t[44].stereo_spread = 0.839969f;
  t[44].percussion.contact_ms = 0.272191f;
  t[46].percussion.exclusive_class = 1;
  t[46].amp_env.release_ms = 40.0f;
  // Open Hi-Hat, voiced against the same kit and with the same defect: it
  // inherits the archetype's 7.5 kHz corner, so it peaked at 12.5 kHz against a
  // reference that peaks at 2.5 kHz and had nothing at all below 630 Hz. The
  // corner comes down to 2.6 kHz, and the noise envelope rather than the
  // amplitude one carries the ring - 1285 ms against a reference that takes
  // about a second to fall 20 dB.
  //
  // Same correction as the closed hat's, and for the same reason: a 2.5 kHz
  // ceiling over a plate leaves 8 % of the piece's energy above 2 kHz where the
  // reference carries 78 %. The mode bank comes down with it — four ring modes
  // at 4x gain put so much at 330 Hz that the network resonated almost nothing
  // else — and the plate is voiced smaller than the pair's physical size, since
  // what the two cymbals radiate together is the higher of their fields.
  t[46].amp_env.attack_ms = 8.45677f;
  t[46].amp_env.decay_ms = 799.63f;
  t[46].cutoff_hz = 2861.2f;
  t[46].drive = 0.226445f;
  t[46].percussion.base_freq_hz = 315.0f;
  t[46].percussion.mode_decay_s = 0.059687f;
  t[46].percussion.mode_ratios[0] = 41.0192f;
  t[46].percussion.mode_ratios[2] = 0.318105f;
  t[46].percussion.tone_gain = 3.88086f;
  t[46].percussion.pitch_drop = 0.180977f;
  t[46].percussion.pitch_drop_ms = 192.0f;
  t[46].percussion.strike_r = 0.349741f;
  t[46].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[46].percussion.noise_cutoff_hz = 321.282f;
  t[46].percussion.noise_decay_ms = 349.732f;
  t[46].percussion.noise_gain = 0.528947f;
  t[46].percussion.noise_q = 1.78641f;
  t[46].percussion.plate_gain = 3.49225f;
  t[46].percussion.plate_low_hz = 322.369f;
  t[46].percussion.plate_air_hz = 14211.0f;
  t[46].resonance_q = 6.71819f;
  t[46].stereo_spread = 0.587404f;
  t[46].gain = 0.3445f;
  t[46].amp_env.delay_ms = 0.4f;
  t[46].percussion.plate_t60_s = 1.24548f;
  t[46].percussion.mode_alpha[1] = 15.3268f;
  t[46].percussion.mode_alpha[2] = 8.21696f;
  t[46].amp_env.sustain = 0.454027f;
  t[46].percussion.contact = 1.86837f;
  t[46].percussion.mode_ratios[1] = 7.64858f;
  t[46].percussion.num_modes = 4;
  t[46].percussion.plate_hf_ratio = 0.01f;
  t[46].percussion.strike_theta = 0.124633f;
  t[46].percussion.tone_direct = 0.878453f;
  t[46].percussion.wire_buzz = 3.82725f;
  t[46].percussion.contact_ms = 0.168279f;
  t[46].percussion.wire_threshold = 2.80521f;

  // --- wooden idiophones + clicks ---
  t[31] = make_wood(1000.0f, 0.0f, 0.03f, 0.6f);  // Sticks
  t[32] = make_wood(1000.0f, 0.0f, 0.02f, 0.5f);  // Square Click
  t[33] = make_wood(1200.0f, 0.0f, 0.02f, 0.5f);  // Metronome Click
  t[37] = make_wood(820.0f, 0.0f, 0.05f, 0.7f);   // Side Stick
  t[37].amp_env.attack_ms = 2.4f;
  t[37].amp_env.decay_ms = 160.699f;
  t[37].amp_env.sustain = 1.0f;
  t[37].percussion.mode_decay_s = 0.00625f;
  t[37].percussion.noise_cutoff_hz = 381.848f;
  t[37].percussion.noise_decay_ms = 32.0f;
  t[37].percussion.noise_gain = 0.683664f;
  t[37].percussion.noise_q = 1.44586f;
  t[37].percussion.strike_r = 0.116788f;
  t[37].gain = 4.0f;
  t[75] = make_wood(2500.0f, 0.0f, 0.025f, 1.3248f);  // Claves (2500 Hz, ~25 ms)
  t[75].amp_env.attack_ms = 0.431828f;
  t[75].amp_env.decay_ms = 30.2516f;
  t[75].amp_env.sustain = 0.00981196f;
  t[75].cutoff_hz = 3233.35f;
  t[75].drive = 0.376893f;
  t[75].percussion.contact = 0.825609f;
  t[75].percussion.mode_decay_s = 0.0228548f;
  t[75].percussion.mode_ratios[0] = 0.793795f;
  t[75].percussion.noise_cutoff_hz = 10123.3f;
  t[75].percussion.noise_decay_ms = 2.56539f;
  t[75].percussion.noise_gain = 1.57942f;
  t[75].percussion.noise_q = 0.58577f;
  t[75].percussion.num_modes = 6;
  t[75].percussion.strike_r = 0.238698f;
  t[75].percussion.tone_gain = 3.74208f;
  t[75].percussion.wire_buzz = 1.25675f;
  t[75].stereo_spread = 0.681734f;
  t[75].percussion.contact_ms = 0.114987f;
  t[75].percussion.wire_cutoff_hz = 13128.0f;
  t[75].percussion.wire_threshold = 2.71494f;
  t[75].resonance_q = 0.859546f;
  t[76] = make_wood(1200.0f, 0.0f, 0.06f, 0.6f);  // Hi Wood Block
  t[76].amp_env.attack_ms = 0.0552037f;
  t[76].amp_env.decay_ms = 503.954f;
  t[76].amp_env.sustain = 0.0442745f;
  t[76].cutoff_hz = 1915.47f;
  t[76].drive = 0.00365726f;
  t[76].percussion.contact = 0.372225f;
  t[76].percussion.mode_decay_s = 0.0514219f;
  t[76].percussion.mode_ratios[0] = 1.39607f;
  t[76].percussion.noise_cutoff_hz = 112.644f;
  t[76].percussion.noise_decay_ms = 145.638f;
  t[76].percussion.noise_gain = 0.681183f;
  t[76].percussion.noise_q = 1.17105f;
  t[76].percussion.num_modes = 4;
  t[76].percussion.plate_gain = 1.36317f;
  t[76].percussion.strike_r = 0.650483f;
  t[76].percussion.tone_gain = 2.53194f;
  t[76].percussion.wire_buzz = 3.42341f;
  t[76].resonance_q = 2.80532f;
  t[76].stereo_spread = 0.899155f;
  t[76].percussion.contact_ms = 5.24755f;
  t[76].percussion.plate_hf_ratio = 0.542482f;
  t[76].percussion.plate_low_hz = 162.88f;
  t[76].percussion.plate_t60_s = 0.218749f;
  t[76].percussion.tone_direct = 0.847765f;
  t[76].percussion.wire_cutoff_hz = 8880.77f;
  t[76].percussion.wire_threshold = 3.37728f;
  t[76].gain = 0.4939f;
  t[77] = make_wood(800.0f, 0.0f, 0.07f, 0.6f);  // Low Wood Block
  t[77].amp_env.attack_ms = 0.15605f;
  t[77].amp_env.decay_ms = 16.8591f;
  t[77].amp_env.sustain = 0.195477f;
  t[77].cutoff_hz = 1519.04f;
  t[77].drive = 0.12199f;
  t[77].percussion.contact = 1.0419f;
  t[77].percussion.mode_decay_s = 0.0763843f;
  t[77].percussion.mode_ratios[0] = 3.30508f;
  t[77].percussion.noise_cutoff_hz = 3780.91f;
  t[77].percussion.noise_decay_ms = 23.8377f;
  t[77].percussion.noise_gain = 2.59048f;
  t[77].percussion.noise_q = 0.615303f;
  t[77].percussion.num_modes = 4;
  t[77].percussion.plate_gain = 2.82841f;
  t[77].percussion.strike_r = 0.452852f;
  t[77].percussion.tone_gain = 2.62933f;
  t[77].percussion.wire_buzz = 3.7011f;
  t[77].resonance_q = 2.19365f;
  t[77].stereo_spread = 0.165385f;
  t[77].percussion.contact_ms = 0.283157f;
  t[77].percussion.plate_hf_ratio = 0.547711f;
  t[77].percussion.plate_low_hz = 439.51f;
  t[77].percussion.plate_t60_s = 0.061714f;
  t[77].percussion.tone_direct = 0.316542f;
  t[77].percussion.wire_cutoff_hz = 4757.66f;
  t[77].percussion.wire_threshold = 3.54802f;
  t[77].gain = 1.0003f;
  t[85] = make_wood(1800.0f, 0.0f, 0.02f, 0.5f);  // Castanets

  // --- metal idiophones + bells ---
  t[34] =
      make_metal(1500.0f, {1.0f, 2.8f, 5.4f, 0.0f, 0.0f, 0.0f}, 3, 0.3f, 0.4f);  // Metronome Bell
  t[53] = make_metal(1200.0f, {1.0f, 1.5f, 2.6f, 0.0f, 0.0f, 0.0f}, 3, 0.6f, 0.4f);  // Ride Bell
  t[53].amp_env.attack_ms = 0.0295325f;
  t[53].amp_env.decay_ms = 93.3168f;
  t[53].amp_env.sustain = 0.238997f;
  t[53].cutoff_hz = 2314.16f;
  t[53].drive = 0.705002f;
  t[53].percussion.contact = 0.302176f;
  t[53].percussion.mode_decay_s = 0.0766601f;
  t[53].percussion.mode_ratios[0] = 14.2512f;
  t[53].percussion.mode_ratios[1] = 3.07541f;
  t[53].percussion.mode_ratios[2] = 1.17999f;
  t[53].percussion.noise_cutoff_hz = 1257.2f;
  t[53].percussion.noise_decay_ms = 9.45646f;
  t[53].percussion.noise_gain = 1.67048f;
  t[53].percussion.noise_q = 1.36764f;
  t[53].percussion.num_modes = 3;
  t[53].percussion.plate_gain = 4.0f;
  t[53].percussion.strike_r = 0.951431f;
  t[53].percussion.tone_gain = 2.13562f;
  t[53].percussion.wire_buzz = 1.47506f;
  t[53].resonance_q = 1.57644f;
  t[53].stereo_spread = 0.24634f;
  t[53].percussion.plate_hf_ratio = 0.858806f;
  t[53].percussion.plate_low_hz = 568.867f;
  t[53].percussion.plate_t60_s = 2.84601f;
  t[53].percussion.tone_direct = 0.326432f;
  t[53].percussion.wire_cutoff_hz = 4951.33f;
  t[53].percussion.wire_threshold = 0.447999f;
  t[53].percussion.strike_theta = 0.116337f;
  t[56] = make_metal(587.0f, {1.0f, 1.44f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.25f,
                     0.5f);  // Cowbell (587/845 Hz)
  t[56].gain = 1.6652f;
  t[67] = make_metal(1200.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.25f, 0.45f);  // High Agogo
  t[67].gain = 0.6823f;
  t[68] = make_metal(900.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.30f, 0.45f);  // Low Agogo
  t[68].amp_env.attack_ms = 0.299523f;
  t[68].amp_env.decay_ms = 848.795f;
  t[68].amp_env.sustain = 0.963954f;
  t[68].cutoff_hz = 2422.35f;
  t[68].drive = 0.696146f;
  t[68].percussion.contact = 2.27253f;
  t[68].percussion.mode_decay_s = 1.08061f;
  t[68].percussion.mode_ratios[0] = 2.07048f;
  t[68].percussion.mode_ratios[1] = 16.7402f;
  t[68].percussion.noise_cutoff_hz = 3977.62f;
  t[68].percussion.noise_decay_ms = 49.8133f;
  t[68].percussion.noise_gain = 1.08079f;
  t[68].percussion.noise_q = 6.07371f;
  t[68].percussion.num_modes = 1;
  t[68].percussion.plate_gain = 0.380971f;
  t[68].percussion.strike_r = 0.435546f;
  t[68].percussion.tone_gain = 1.57692f;
  t[68].percussion.wire_buzz = 1.15614f;
  t[68].stereo_spread = 0.763887f;
  t[68].percussion.plate_hf_ratio = 0.549104f;
  t[68].percussion.plate_low_hz = 311.489f;
  t[68].percussion.plate_t60_s = 0.218884f;
  t[68].percussion.tone_direct = 0.829661f;
  t[68].percussion.wire_cutoff_hz = 2081.57f;
  t[68].percussion.wire_threshold = 1.5372f;
  t[68].resonance_q = 12.1564f;
  t[68].gain = 0.2071f;
  t[83] =
      make_metal(2500.0f, {1.0f, 1.7f, 2.4f, 0.0f, 0.0f, 0.0f}, 3, 0.40f, 0.35f);  // Jingle Bell
  t[84] = make_metal(3000.0f, {1.0f, 1.6f, 2.3f, 3.1f, 0.0f, 0.0f}, 4, 1.50f, 0.30f);  // Belltree

  // Triangle: high inharmonic modes; mute short, open long (mute group 3).
  const std::array<float, kMaxPercussionModes> triangle_ratios = {1.0f,  2.76f, 5.40f,
                                                                  8.90f, 0.0f,  0.0f};
  t[80] = make_metal(5000.0f, triangle_ratios, 4, 0.15f, 0.35f);  // Mute Triangle
  t[81] = make_metal(5000.0f, triangle_ratios, 4, 1.20f, 0.35f);  // Open Triangle
  t[80].percussion.exclusive_class = 3;
  t[80].amp_env.attack_ms = 0.496756f;
  t[80].amp_env.decay_ms = 67.1263f;
  t[80].amp_env.sustain = 0.0f;
  t[80].cutoff_hz = 1076.73f;
  t[80].drive = 0.358197f;
  t[80].percussion.contact = 0.0f;
  t[80].percussion.mode_decay_s = 0.00948575f;
  t[80].percussion.mode_ratios[0] = 5.71289f;
  t[80].percussion.mode_ratios[1] = 8.06782f;
  t[80].percussion.mode_ratios[2] = 0.889723f;
  t[80].percussion.mode_ratios[3] = 2.05889f;
  t[80].percussion.noise_cutoff_hz = 3936.92f;
  t[80].percussion.noise_decay_ms = 3.80459f;
  t[80].percussion.noise_gain = 1.3615f;
  t[80].percussion.noise_q = 2.95751f;
  t[80].percussion.plate_gain = 2.93706f;
  t[80].percussion.strike_r = 0.131708f;
  t[80].percussion.tone_gain = 3.30232f;
  t[80].percussion.wire_buzz = 1.39762f;
  t[80].resonance_q = 0.801302f;
  t[80].stereo_spread = 0.666104f;
  t[80].percussion.contact_ms = 0.0375f;
  t[80].percussion.num_modes = 1;
  t[80].percussion.plate_hf_ratio = 0.296254f;
  t[80].percussion.plate_low_hz = 174.366f;
  t[80].percussion.plate_t60_s = 0.830513f;
  t[80].percussion.strike_theta = 0.266747f;
  t[80].percussion.wire_cutoff_hz = 1202.78f;
  t[80].percussion.wire_threshold = 2.69225f;
  t[80].percussion.tone_direct = 0.871336f;
  t[80].gain = 2.0281f;
  t[81].percussion.exclusive_class = 3;
  t[81].amp_env.attack_ms = 0.0964563f;
  t[81].amp_env.decay_ms = 1586.8f;
  t[81].amp_env.sustain = 0.249509f;
  t[81].cutoff_hz = 2068.25f;
  t[81].drive = 0.70305f;
  t[81].percussion.contact = 0.859528f;
  t[81].percussion.mode_decay_s = 2.24284f;
  t[81].percussion.mode_ratios[0] = 1.23146f;
  t[81].percussion.mode_ratios[1] = 1.49443f;
  t[81].percussion.mode_ratios[2] = 0.880676f;
  t[81].percussion.mode_ratios[3] = 8.27803f;
  t[81].percussion.noise_cutoff_hz = 14497.4f;
  t[81].percussion.noise_decay_ms = 478.975f;
  t[81].percussion.noise_gain = 2.12371f;
  t[81].percussion.noise_q = 3.60831f;
  t[81].percussion.num_modes = 2;
  t[81].percussion.plate_gain = 1.48575f;
  t[81].percussion.strike_r = 0.82515f;
  t[81].percussion.tone_gain = 2.60195f;
  t[81].percussion.wire_buzz = 3.70966f;
  t[81].resonance_q = 2.31974f;
  t[81].stereo_spread = 0.149776f;
  t[81].percussion.strike_theta = 0.523123f;
  t[81].percussion.contact_ms = 0.0563958f;
  t[81].percussion.plate_hf_ratio = 0.228995f;
  t[81].percussion.plate_low_hz = 1159.83f;
  t[81].percussion.plate_t60_s = 3.43851f;
  t[81].percussion.tone_direct = 0.704216f;
  t[81].percussion.wire_cutoff_hz = 862.488f;
  t[81].percussion.wire_threshold = 0.176286f;

  // --- fixed-pitch membranes (congas/bongos/timbales/surdo) ---
  t[60] = make_membrane(260.0f, 0.18f, 0.30f, 0.0f, 0.70f);  // Hi Bongo
  t[60].amp_env.attack_ms = 1.03102f;
  t[60].amp_env.decay_ms = 104.04f;
  t[60].amp_env.sustain = 0.593512f;
  t[60].cutoff_hz = 574.355f;
  t[60].drive = 0.323871f;
  t[60].percussion.contact = 1.91367f;
  t[60].percussion.mode_decay_s = 0.0215435f;
  t[60].percussion.mode_ratios[0] = 0.770968f;
  t[60].percussion.mode_ratios[1] = 7.34388f;
  t[60].percussion.mode_ratios[2] = 2.33571f;
  t[60].percussion.mode_ratios[3] = 0.169987f;
  t[60].percussion.mode_ratios[4] = 1.31019f;
  t[60].percussion.noise_cutoff_hz = 790.016f;
  t[60].percussion.noise_decay_ms = 3.00304f;
  t[60].percussion.noise_gain = 3.14722f;
  t[60].percussion.noise_q = 0.771905f;
  t[60].percussion.num_modes = 2;
  t[60].percussion.pitch_drop = 0.0848906f;
  t[60].percussion.pitch_drop_ms = 34.9145f;
  t[60].percussion.plate_gain = 3.22603f;
  t[60].percussion.strike_r = 0.483167f;
  t[60].percussion.strike_theta = 0.263534f;
  t[60].percussion.tone_gain = 1.52618f;
  t[60].percussion.wire_buzz = 0.795321f;
  t[60].stereo_spread = 0.202579f;
  t[60].percussion.contact_ms = 0.634576f;
  t[60].percussion.plate_hf_ratio = 1.0f;
  t[60].percussion.plate_low_hz = 290.222f;
  t[60].percussion.plate_t60_s = 0.419356f;
  t[60].percussion.tone_direct = 0.873151f;
  t[60].percussion.wire_cutoff_hz = 973.12f;
  t[60].percussion.wire_threshold = 2.38294f;
  t[60].resonance_q = 1.40527f;
  t[60].gain = 0.6443f;
  t[61] = make_membrane(180.0f, 0.20f, 0.30f, 0.0f, 0.70f);  // Low Bongo
  t[61].amp_env.attack_ms = 5.49668f;
  t[61].amp_env.decay_ms = 55.7872f;
  t[61].amp_env.sustain = 0.297213f;
  t[61].cutoff_hz = 11709.9f;
  t[61].drive = 0.210766f;
  t[61].percussion.contact = 0.362062f;
  t[61].percussion.mode_decay_s = 0.0541491f;
  t[61].percussion.mode_ratios[0] = 0.613928f;
  t[61].percussion.mode_ratios[1] = 5.26164f;
  t[61].percussion.mode_ratios[2] = 1.14683f;
  t[61].percussion.mode_ratios[3] = 21.6188f;
  t[61].percussion.mode_ratios[4] = 0.536416f;
  t[61].percussion.noise_cutoff_hz = 30.0142f;
  t[61].percussion.noise_decay_ms = 27.1429f;
  t[61].percussion.noise_gain = 1.57707f;
  t[61].percussion.noise_q = 1.66954f;
  t[61].percussion.pitch_drop = 2.29967f;
  t[61].percussion.pitch_drop_ms = 4.12633f;
  t[61].percussion.plate_gain = 1.99773f;
  t[61].percussion.strike_r = 0.694698f;
  t[61].percussion.strike_theta = 1.34672f;
  t[61].percussion.tone_gain = 2.53059f;
  t[61].percussion.wire_buzz = 1.03659f;
  t[61].stereo_spread = 0.418339f;
  t[61].percussion.contact_ms = 0.109856f;
  t[61].percussion.num_modes = 3;
  t[61].percussion.plate_hf_ratio = 0.695523f;
  t[61].percussion.plate_low_hz = 295.289f;
  t[61].percussion.plate_t60_s = 0.306712f;
  t[61].percussion.tone_direct = 0.915486f;
  t[61].percussion.wire_cutoff_hz = 2088.31f;
  t[61].percussion.wire_threshold = 1.05166f;
  t[61].resonance_q = 0.854144f;
  t[61].gain = 1.0607f;
  t[62] = make_membrane(220.0f, 0.08f, 0.20f, 0.0f, 0.70f);  // Mute Hi Conga
  t[62].amp_env.attack_ms = 0.0882081f;
  t[62].amp_env.decay_ms = 376.595f;
  t[62].cutoff_hz = 4432.96f;
  t[62].drive = 0.486311f;
  t[62].percussion.contact = 3.37961f;
  t[62].percussion.mode_decay_s = 0.35067f;
  t[62].percussion.mode_ratios[0] = 0.204006f;
  t[62].percussion.mode_ratios[1] = 1.64637f;
  t[62].percussion.mode_ratios[2] = 0.202695f;
  t[62].percussion.mode_ratios[3] = 17.5079f;
  t[62].percussion.mode_ratios[4] = 10.7378f;
  t[62].percussion.noise_cutoff_hz = 785.897f;
  t[62].percussion.noise_decay_ms = 36.3864f;
  t[62].percussion.noise_gain = 0.681314f;
  t[62].percussion.noise_q = 3.60454f;
  t[62].percussion.pitch_drop = 0.362074f;
  t[62].percussion.pitch_drop_ms = 6.01089f;
  t[62].percussion.plate_gain = 1.16611f;
  t[62].percussion.strike_r = 0.816789f;
  t[62].percussion.strike_theta = 1.18888f;
  t[62].percussion.tone_gain = 1.20114f;
  t[62].percussion.wire_buzz = 3.95875f;
  t[62].stereo_spread = 0.148505f;
  t[62].amp_env.sustain = 0.498882f;
  t[62].percussion.contact_ms = 2.07134f;
  t[62].percussion.num_modes = 5;
  t[62].percussion.plate_hf_ratio = 0.873691f;
  t[62].percussion.plate_low_hz = 459.58f;
  t[62].percussion.plate_t60_s = 0.0709381f;
  t[62].percussion.tone_direct = 0.204074f;
  t[62].percussion.wire_cutoff_hz = 521.63f;
  t[62].percussion.wire_threshold = 3.70137f;
  t[62].resonance_q = 2.23469f;
  t[62].gain = 0.4905f;
  t[63] = make_membrane(200.0f, 0.25f, 0.30f, 0.0f, 1.1847f);  // Open Hi Conga
  t[63].amp_env.attack_ms = 6.1927f;
  t[63].amp_env.decay_ms = 217.078f;
  t[63].amp_env.sustain = 0.645014f;
  t[63].percussion.mode_decay_s = 0.0554174f;
  t[63].percussion.noise_cutoff_hz = 227.798f;
  t[63].percussion.noise_decay_ms = 4.88626f;
  t[63].percussion.noise_gain = 1.78569f;
  t[63].percussion.noise_q = 13.0937f;
  t[63].percussion.pitch_drop = 0.174511f;
  t[63].percussion.pitch_drop_ms = 778.011f;
  t[63].percussion.strike_r = 0.730228f;
  t[63].percussion.strike_theta = 0.731062f;
  t[63].cutoff_hz = 1083.62f;
  t[63].drive = 0.368634f;
  t[63].percussion.contact = 0.728366f;
  t[63].percussion.mode_ratios[0] = 2.24933f;
  t[63].percussion.mode_ratios[1] = 11.1108f;
  t[63].percussion.mode_ratios[2] = 38.2331f;
  t[63].percussion.mode_ratios[3] = 3.82169f;
  t[63].percussion.mode_ratios[4] = 4.69038f;
  t[63].percussion.num_modes = 1;
  t[63].percussion.plate_gain = 1.34609f;
  t[63].percussion.tone_gain = 3.64902f;
  t[63].resonance_q = 0.846882f;
  t[63].stereo_spread = 0.650573f;
  t[63].percussion.plate_hf_ratio = 0.604102f;
  t[63].percussion.plate_low_hz = 140.0f;
  t[63].percussion.plate_t60_s = 0.447258f;
  t[63].percussion.wire_buzz = 2.96258f;
  t[63].gain = 1.1392f;
  t[64] = make_membrane(130.0f, 0.30f, 0.35f, 0.0f, 1.4014f);  // Low Conga
  t[64].amp_env.attack_ms = 0.103689f;
  t[64].amp_env.decay_ms = 92.8306f;
  t[64].amp_env.sustain = 0.466948f;
  t[64].cutoff_hz = 11671.9f;
  t[64].drive = 0.895751f;
  t[64].percussion.contact = 0.346301f;
  t[64].percussion.mode_decay_s = 0.690647f;
  t[64].percussion.mode_ratios[0] = 0.113204f;
  t[64].percussion.mode_ratios[1] = 1.94495f;
  t[64].percussion.mode_ratios[2] = 2.06324f;
  t[64].percussion.mode_ratios[3] = 11.7225f;
  t[64].percussion.mode_ratios[4] = 1.2523f;
  t[64].percussion.noise_cutoff_hz = 400.224f;
  t[64].percussion.noise_decay_ms = 52.7828f;
  t[64].percussion.noise_gain = 3.2458f;
  t[64].percussion.noise_q = 10.0505f;
  t[64].percussion.num_modes = 2;
  t[64].percussion.pitch_drop = 4.21119f;
  t[64].percussion.pitch_drop_ms = 33.5273f;
  t[64].percussion.strike_r = 0.474688f;
  t[64].percussion.strike_theta = 0.171321f;
  t[64].percussion.tone_gain = 3.14836f;
  t[64].percussion.wire_buzz = 1.22992f;
  t[64].stereo_spread = 0.881839f;
  t[64].percussion.contact_ms = 0.072924f;
  t[64].percussion.plate_gain = 0.00894634f;
  t[64].percussion.wire_cutoff_hz = 2543.09f;
  t[64].percussion.wire_threshold = 2.67961f;
  t[64].resonance_q = 0.639576f;
  t[64].gain = 0.7491f;
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 1.1120f);  // High Timbale
  t[65].amp_env.attack_ms = 0.0703342f;
  t[65].amp_env.decay_ms = 2157.14f;
  t[65].amp_env.sustain = 0.112366f;
  t[65].cutoff_hz = 958.011f;
  t[65].drive = 0.750198f;
  t[65].percussion.contact = 1.69182f;
  t[65].percussion.mode_decay_s = 0.0234058f;
  t[65].percussion.mode_ratios[0] = 3.2351f;
  t[65].percussion.mode_ratios[1] = 6.35146f;
  t[65].percussion.mode_ratios[2] = 0.347988f;
  t[65].percussion.mode_ratios[3] = 0.358939f;
  t[65].percussion.mode_ratios[4] = 4.94214f;
  t[65].percussion.noise_cutoff_hz = 10301.7f;
  t[65].percussion.noise_decay_ms = 9.26909f;
  t[65].percussion.noise_gain = 2.10496f;
  t[65].percussion.noise_q = 0.885976f;
  t[65].percussion.num_modes = 4;
  t[65].percussion.pitch_drop = 0.0361401f;
  t[65].percussion.pitch_drop_ms = 454.298f;
  t[65].percussion.plate_gain = 0.613718f;
  t[65].percussion.shell_mix = 0.808227f;
  t[65].percussion.shell_t60_s[0] = 0.0274245f;
  t[65].percussion.shell_weight[0] = 1.92784f;
  t[65].percussion.strike_r = 0.129494f;
  t[65].percussion.strike_theta = 1.21003f;
  t[65].percussion.tone_gain = 1.80484f;
  t[65].percussion.wire_buzz = 3.62035f;
  t[65].stereo_spread = 0.260304f;
  t[65].percussion.contact_ms = 0.314127f;
  t[65].percussion.plate_hf_ratio = 0.905f;
  t[65].percussion.plate_low_hz = 294.406f;
  t[65].percussion.plate_t60_s = 0.661604f;
  t[65].percussion.tone_direct = 0.907509f;
  t[65].percussion.wire_cutoff_hz = 2658.27f;
  t[65].percussion.wire_threshold = 1.00042f;
  t[65].resonance_q = 1.33548f;
  t[65].percussion.shell_num_modes = 2;
  t[65].gain = 0.7364f;
  t[66] = make_membrane(200.0f, 0.26f, 0.20f, 550.0f, 0.70f);  // Low Timbale
  t[66].amp_env.attack_ms = 0.316485f;
  t[66].amp_env.decay_ms = 40.3508f;
  t[66].amp_env.sustain = 0.312611f;
  t[66].cutoff_hz = 2699.77f;
  t[66].drive = 0.633119f;
  t[66].percussion.contact = 0.828938f;
  t[66].percussion.mode_decay_s = 0.0591567f;
  t[66].percussion.mode_ratios[0] = 13.7448f;
  t[66].percussion.mode_ratios[1] = 9.38057f;
  t[66].percussion.mode_ratios[2] = 0.380002f;
  t[66].percussion.mode_ratios[3] = 16.8864f;
  t[66].percussion.mode_ratios[4] = 5.2565f;
  t[66].percussion.noise_cutoff_hz = 344.461f;
  t[66].percussion.noise_decay_ms = 104.098f;
  t[66].percussion.noise_gain = 3.62819f;
  t[66].percussion.noise_q = 5.33439f;
  t[66].percussion.num_modes = 2;
  t[66].percussion.pitch_drop = 0.265101f;
  t[66].percussion.pitch_drop_ms = 32.2164f;
  t[66].percussion.plate_gain = 1.07744f;
  t[66].percussion.shell_mix = 0.819333f;
  t[66].percussion.shell_t60_s[0] = 0.0329163f;
  t[66].percussion.shell_weight[0] = 2.65968f;
  t[66].percussion.strike_r = 0.660625f;
  t[66].percussion.strike_theta = 0.229912f;
  t[66].percussion.tone_gain = 2.31878f;
  t[66].percussion.wire_buzz = 0.290375f;
  t[66].resonance_q = 6.79381f;
  t[66].stereo_spread = 0.771765f;
  t[66].percussion.contact_ms = 0.153855f;
  t[66].percussion.plate_hf_ratio = 0.618336f;
  t[66].percussion.plate_low_hz = 1556.17f;
  t[66].percussion.plate_t60_s = 0.243625f;
  t[66].percussion.shell_num_modes = 3;
  t[66].percussion.tone_direct = 0.747727f;
  t[66].percussion.wire_cutoff_hz = 13076.9f;
  t[66].percussion.wire_threshold = 0.507762f;
  t[66].percussion.shell_weight[1] = 2.23246f;
  t[86] = make_membrane(95.0f, 0.12f, 0.40f, 0.0f, 0.80f);  // Mute Surdo
  t[87] = make_membrane(80.0f, 0.40f, 0.50f, 0.0f, 0.85f);  // Open Surdo
  t[86].percussion.exclusive_class = 6;
  t[87].percussion.exclusive_class = 6;

  // --- whistles (mute group 4) + hand clap ---
  t[71] = make_whistle(1400.0f, 0.12f, 0.7473f);  // Short Whistle
  t[72] = make_whistle(1400.0f, 0.50f, 0.5f);     // Long Whistle
  t[71].percussion.exclusive_class = 4;
  t[71].amp_env.attack_ms = 22.8528f;
  t[71].amp_env.decay_ms = 41.2269f;
  t[71].amp_env.sustain = 0.0f;
  t[71].cutoff_hz = 13958.6f;
  t[71].drive = 0.737143f;
  t[71].percussion.contact = 0.689194f;
  t[71].percussion.mode_decay_s = 0.634916f;
  t[71].percussion.mode_ratios[0] = 38.2286f;
  t[71].percussion.noise_cutoff_hz = 3456.37f;
  t[71].percussion.noise_decay_ms = 38.7214f;
  t[71].percussion.noise_gain = 2.42753f;
  t[71].percussion.noise_q = 9.20918f;
  t[71].percussion.plate_gain = 3.27065f;
  t[71].percussion.strike_r = 0.435508f;
  t[71].percussion.tone_gain = 2.31126f;
  t[71].percussion.wire_buzz = 2.93265f;
  t[71].stereo_spread = 0.739068f;
  t[71].percussion.num_modes = 0;
  t[71].percussion.plate_hf_ratio = 0.518624f;
  t[71].percussion.plate_low_hz = 392.37f;
  t[71].percussion.plate_t60_s = 0.141835f;
  t[71].percussion.wire_cutoff_hz = 1415.13f;
  t[71].percussion.wire_threshold = 3.22558f;
  t[71].resonance_q = 0.869943f;
  t[71].percussion.contact_ms = 0.147553f;
  t[71].gain = 0.6823f;
  t[72].percussion.exclusive_class = 4;
  t[72].amp_env.attack_ms = 69.341f;
  t[72].amp_env.decay_ms = 671.355f;
  t[72].amp_env.sustain = 0.457957f;
  t[72].cutoff_hz = 10189.4f;
  t[72].drive = 0.329158f;
  t[72].percussion.contact = 0.0f;
  t[72].percussion.mode_decay_s = 0.254392f;
  t[72].percussion.mode_ratios[0] = 7.0373f;
  t[72].percussion.noise_cutoff_hz = 9169.76f;
  t[72].percussion.noise_decay_ms = 76.8772f;
  t[72].percussion.noise_gain = 2.74147f;
  t[72].percussion.noise_q = 10.4924f;
  t[72].percussion.num_modes = 0;
  t[72].percussion.plate_gain = 0.724603f;
  t[72].percussion.strike_r = 0.0568307f;
  t[72].percussion.tone_gain = 1.92962f;
  t[72].percussion.wire_buzz = 0.508078f;
  t[72].stereo_spread = 0.362584f;
  t[72].amp_env.release_ms = 53.3108f;
  t[72].percussion.plate_hf_ratio = 0.609399f;
  t[72].percussion.plate_low_hz = 194.547f;
  t[72].percussion.plate_t60_s = 13.5899f;
  t[72].resonance_q = 2.82361f;
  t[72].percussion.contact_ms = 0.0743891f;
  t[72].gain = 0.2515f;
  t[39] = clap;  // Hand Clap

  // --- radiated ceiling (mute group note) ---
  // A real cymbal, snare or clap stops putting energy into the room well below
  // Nyquist; the noise streams that voice them here do not, so every one of
  // these pieces filled the top 1/3-octave band while the reference had rolled
  // off by 4-6 kHz. Each corner below is solved rather than chosen: rendered,
  // measured against the reference's own top edge, and iterated. The gain beside
  // it restores the peak the ceiling costs, which is a re-gain the piece has to
  // have in the same change - every metric in the comparison is normalised, so a
  // level left 5 dB down reads as correct everywhere and is only audible.
  //
  // The hi-hats are absent on purpose, and for a different reason than they
  // first appeared to have. The filter is on their path; what defeats it is
  // that their noise is high-passed at 7.5 kHz, so a ceiling can only attenuate
  // a band that starts above where the reference's energy ends, and taking the
  // corner to 200 Hz costs 60 dB of level while leaving the top band filled.
  // Their answer is the band itself, below.
  //
  // The shakers and scrapers are absent because the ceiling genuinely does not
  // reach them: the PhISEM particle stream is summed into the mix unfiltered,
  // so the knob moves nothing at all there.
  t[38].percussion.noise_air_hz = 528.189f;
  t[38].amp_env.attack_ms = 0.500033f;
  t[38].amp_env.decay_ms = 163.222f;
  t[38].cutoff_hz = 2347.23f;
  t[38].drive = 0.809776f;
  t[38].percussion.contact = 0.56816f;
  t[38].percussion.mode_decay_s = 0.24388f;
  t[38].percussion.mode_ratios[0] = 16.9494f;
  t[38].percussion.mode_ratios[1] = 0.247722f;
  t[38].percussion.mode_ratios[2] = 0.689976f;
  t[38].percussion.mode_ratios[3] = 18.4f;
  t[38].percussion.mode_ratios[4] = 2.894f;
  t[38].percussion.noise_cutoff_hz = 227.746f;
  t[38].percussion.noise_decay_ms = 55.7203f;
  t[38].percussion.noise_gain = 1.60791f;
  t[38].percussion.noise_q = 0.573994f;
  t[38].percussion.num_modes = 1;
  t[38].percussion.pitch_drop = 5.88455f;
  t[38].percussion.pitch_drop_ms = 8.68602f;
  t[38].percussion.plate_gain = 3.97168f;
  t[38].percussion.shell_mix = 0.389182f;
  t[38].percussion.shell_num_modes = 0;
  t[38].percussion.shell_t60_s[0] = 0.111721f;
  t[38].percussion.shell_t60_s[1] = 2.05808f;
  t[38].percussion.shell_weight[0] = 3.00827f;
  t[38].percussion.shell_weight[1] = 3.23187f;
  t[38].percussion.strike_r = 0.551531f;
  t[38].percussion.strike_theta = 0.763044f;
  t[38].percussion.tone_gain = 2.48485f;
  t[38].percussion.wire_buzz = 2.43521f;
  t[38].percussion.wire_cutoff_hz = 5904.86f;
  t[38].percussion.wire_threshold = 2.82444f;
  t[38].amp_env.sustain = 0.261873f;
  t[38].percussion.contact_ms = 0.0703652f;
  t[38].percussion.plate_hf_ratio = 0.54548f;
  t[38].percussion.plate_low_hz = 500.044f;
  t[38].percussion.plate_t60_s = 0.727496f;
  t[38].percussion.shell_weight[2] = 1.11387f;
  t[38].percussion.shell_weight[3] = 1.28435f;
  t[38].percussion.tone_direct = 0.0695634f;
  t[38].resonance_q = 2.24364f;
  t[38].gain = 3.5606f;  // Acoustic Snare
  t[39].percussion.noise_air_hz = 1459.44f;
  t[39].gain = 2.5615f;  // Hand Clap
  t[39].amp_env.attack_ms = 3.87415f;
  t[39].amp_env.decay_ms = 145.203f;
  t[39].amp_env.sustain = 0.00762793f;
  t[39].percussion.noise_cutoff_hz = 2444.29f;
  t[39].percussion.noise_decay_ms = 720.0f;
  t[39].percussion.noise_gain = 1.8044f;
  t[39].percussion.noise_q = 2.92861f;
  t[40].percussion.noise_air_hz = 304.85f;
  t[40].amp_env.attack_ms = 0.277725f;
  t[40].amp_env.decay_ms = 282.62f;
  t[40].amp_env.sustain = 0.630063f;
  t[40].cutoff_hz = 3816.06f;
  t[40].drive = 0.230105f;
  t[40].percussion.contact = 0.23055f;
  t[40].percussion.mode_ratios[0] = 0.996289f;
  t[40].percussion.mode_ratios[1] = 1.14696f;
  t[40].percussion.mode_ratios[2] = 1.0103f;
  t[40].percussion.mode_ratios[3] = 0.509061f;
  t[40].percussion.mode_ratios[4] = 0.511987f;
  t[40].percussion.noise_gain = 2.98249f;
  t[40].percussion.noise_q = 0.731594f;
  t[40].percussion.num_modes = 1;
  t[40].percussion.pitch_drop = 0.0772591f;
  t[40].percussion.pitch_drop_ms = 15.4921f;
  t[40].percussion.plate_gain = 0.44394f;
  t[40].percussion.strike_r = 0.898037f;
  t[40].percussion.strike_theta = 0.329214f;
  t[40].percussion.tone_gain = 0.631376f;
  t[40].percussion.wire_cutoff_hz = 5660.92f;
  t[40].percussion.wire_threshold = 1.25178f;
  t[40].resonance_q = 0.9922f;
  t[40].stereo_spread = 0.81941f;
  t[40].gain = 4.0f;  // Electric Snare
  //
  // The cymbal ceilings are several times higher than the drums' because the
  // plate is behind them. A cymbal's ceiling bounds the wash, and the wash is
  // also what excites the plate, so a corner low enough to bound flat noise
  // hands the network nothing above it to resonate and the piece falls back to
  // its few loud ring modes: at a 1 kHz ceiling a crash held four fifths of its
  // band energy in twenty bins, against a reference that holds half. Above the
  // plate's own reach the ceiling does what it did before, which is why the
  // measured top edges still set where these sit.
  //
  // The cymbals have lost their ceilings: their wash is a band now, and a band
  // already has a top. What each one is bounded by instead is its plate ceiling
  // and its voice corner, beside the plate values above.
  t[49].amp_env.decay_ms = 2897.07f;
  t[49].percussion.mode_decay_s = 5.22347f;
  t[49].percussion.noise_decay_ms = 892.111f;
  t[49].resonance_q = 0.524952f;
  t[49].percussion.mode_ratios[0] = 4.2626f;
  t[49].gain = 0.6029f;  // Crash 1
  t[49].amp_env.sustain = 0.846107f;
  t[49].drive = 0.830042f;
  t[49].percussion.contact = 3.39062f;
  t[49].percussion.mode_ratios[1] = 4.09347f;
  t[49].percussion.mode_ratios[2] = 8.83488f;
  t[49].percussion.mode_ratios[3] = 0.684544f;
  t[49].percussion.noise_cutoff_hz = 3352.0f;
  t[49].percussion.noise_gain = 2.7538f;
  t[49].percussion.noise_q = 12.4719f;
  t[49].percussion.shimmer = 16.0f;
  t[49].percussion.shimmer_attack_ms = 118.295f;
  t[49].percussion.shimmer_cutoff_hz = 10002.1f;
  t[49].percussion.strike_r = 0.679358f;
  t[49].percussion.tone_direct = 0.146176f;
  t[49].percussion.tone_gain = 0.0669442f;
  t[49].percussion.wire_buzz = 0.819144f;
  t[49].stereo_spread = 0.568385f;
  t[49].percussion.contact_ms = 0.0678432f;
  t[49].percussion.wire_cutoff_hz = 1573.94f;
  t[49].percussion.wire_threshold = 1.28622f;
  t[51].drift_cents = 14.9294f;
  t[51].drift_rate_hz = 0.731264f;
  t[51].key_track = 0.64f;
  t[51].percussion.mode_decay_s = 7.27851f;
  t[51].percussion.noise_cutoff_hz = 2263.25f;
  t[51].resonance_q = 6.59868f;
  t[51].percussion.mode_ratios[2] = 1.87896f;
  t[51].gain = 0.7432f;  // Ride 1
  t[51].amp_env.attack_ms = 0.228377f;
  t[51].amp_env.decay_ms = 2449.86f;
  t[51].percussion.contact = 0.233992f;
  t[51].percussion.mode_ratios[0] = 6.20648f;
  t[51].percussion.mode_ratios[3] = 10.8139f;
  t[51].percussion.noise_decay_ms = 2065.68f;
  t[51].percussion.noise_gain = 2.95739f;
  t[51].percussion.shimmer_cutoff_hz = 3844.0f;
  t[51].percussion.strike_r = 0.138772f;
  t[51].percussion.tone_direct = 0.902066f;
  t[51].percussion.tone_gain = 0.624849f;
  t[51].percussion.wire_buzz = 0.0804194f;
  t[51].stereo_spread = 0.255923f;
  t[52].amp_env.attack_ms = 52.5749f;
  t[52].percussion.base_freq_hz = 4160.0f;
  t[52].percussion.mode_decay_s = 1.18457f;
  t[52].percussion.noise_cutoff_hz = 2492.32f;
  t[52].percussion.noise_decay_ms = 451.04f;
  t[52].percussion.noise_gain = 0.838898f;
  t[52].percussion.phisem_beans = 0.362458f;
  t[52].percussion.phisem_sound_ms = 3.0697f;
  t[52].percussion.shimmer = 1.42327f;
  t[52].percussion.mode_ratios[2] = 1.43935f;
  t[52].gain = 4.0f;  // China
  t[52].amp_env.decay_ms = 543.272f;
  t[52].amp_env.sustain = 0.389133f;
  t[52].drive = 0.674902f;
  t[52].percussion.contact = 0.190899f;
  t[52].percussion.mode_ratios[0] = 0.488153f;
  t[52].percussion.mode_ratios[1] = 0.464146f;
  t[52].percussion.mode_ratios[3] = 0.20025f;
  t[52].percussion.phisem_energy_ms = 7.20449f;
  t[52].percussion.shimmer_attack_ms = 333.305f;
  t[52].percussion.shimmer_cutoff_hz = 497.718f;
  t[52].percussion.strike_r = 0.420051f;
  t[52].percussion.tone_direct = 0.171123f;
  t[52].percussion.tone_gain = 0.105829f;
  t[52].percussion.wire_buzz = 0.581431f;
  t[52].stereo_spread = 0.387829f;
  t[52].percussion.contact_ms = 0.767142f;
  t[52].percussion.num_modes = 3;
  t[52].percussion.strike_theta = 0.587903f;
  t[52].percussion.wire_cutoff_hz = 983.168f;
  t[52].percussion.wire_threshold = 1.10934f;
  t[55].amp_env.decay_ms = 3358.34f;
  t[55].percussion.base_freq_hz = 2080.0f;
  t[55].percussion.noise_decay_ms = 541.604f;
  t[55].percussion.noise_gain = 3.27706f;
  t[55].percussion.phisem_sound_ms = 4.69501f;
  t[55].percussion.plate_hf_ratio = 0.716924f;
  t[55].percussion.tone_gain = 1.02085f;
  t[55].percussion.mode_ratios[0] = 1.52381f;
  t[55].percussion.mode_ratios[3] = 2.62026f;
  t[55].gain = 0.1629f;  // Splash
  t[55].amp_env.sustain = 0.301793f;
  t[55].drive = 0.21861f;
  t[55].percussion.contact = 1.60247f;
  t[55].percussion.mode_decay_s = 0.0858076f;
  t[55].percussion.mode_ratios[1] = 4.23922f;
  t[55].percussion.mode_ratios[2] = 11.2842f;
  t[55].percussion.num_modes = 1;
  t[55].percussion.phisem_energy_ms = 114.513f;
  t[55].percussion.shimmer = 15.74f;
  t[55].percussion.shimmer_cutoff_hz = 7310.5f;
  t[55].percussion.strike_r = 0.767934f;
  t[55].percussion.wire_buzz = 3.96496f;
  t[55].resonance_q = 5.805f;
  t[55].stereo_spread = 0.665173f;
  t[55].amp_env.attack_ms = 0.0906749f;
  t[55].percussion.contact_ms = 0.453639f;
  t[55].percussion.tone_direct = 0.7377f;
  t[55].percussion.wire_cutoff_hz = 1197.07f;
  t[55].percussion.wire_threshold = 2.58096f;
  t[57].amp_env.decay_ms = 3670.55f;
  t[57].percussion.mode_decay_s = 0.514154f;
  t[57].percussion.noise_decay_ms = 1198.48f;
  t[57].percussion.shimmer = 2.00457f;
  t[57].percussion.tone_gain = 1.68419f;
  t[57].resonance_q = 4.13277f;
  t[57].percussion.mode_ratios[1] = 0.211778f;
  t[57].gain = 2.0872f;  // Crash 2
  t[57].drive = 0.447548f;
  t[57].percussion.contact = 0.734647f;
  t[57].percussion.mode_ratios[0] = 26.935f;
  t[57].percussion.mode_ratios[2] = 0.511553f;
  t[57].percussion.mode_ratios[3] = 0.748455f;
  t[57].percussion.noise_cutoff_hz = 2777.44f;
  t[57].percussion.noise_gain = 0.214828f;
  t[57].percussion.num_modes = 1;
  t[57].percussion.shimmer_attack_ms = 14.1957f;
  t[57].percussion.strike_r = 0.917643f;
  t[57].percussion.tone_direct = 0.6968f;
  t[57].percussion.wire_buzz = 3.68241f;
  t[57].stereo_spread = 0.333327f;
  t[57].amp_env.attack_ms = 0.409376f;
  t[57].percussion.contact_ms = 0.218496f;
  t[57].amp_env.sustain = 0.0228357f;
  t[59].drift_cents = 0.472376f;
  t[59].percussion.mode_decay_s = 5.22852f;
  t[59].percussion.phisem_beans = 1.01776f;
  t[59].percussion.strike_r = 0.723574f;
  t[59].pitch_offset_cents = 16.0f;
  t[59].percussion.mode_ratios[1] = 0.363682f;
  t[59].gain = 0.8842f;  // Ride 2
  t[59].amp_env.attack_ms = 0.227998f;
  t[59].amp_env.decay_ms = 3013.33f;
  t[59].drift_rate_hz = 0.0499312f;
  t[59].drive = 0.286114f;
  t[59].percussion.contact = 2.47087f;
  t[59].percussion.mode_ratios[2] = 1.74408f;
  t[59].percussion.noise_cutoff_hz = 1578.79f;
  t[59].percussion.noise_decay_ms = 2207.52f;
  t[59].percussion.num_modes = 1;
  t[59].percussion.phisem_energy_ms = 193.374f;
  t[59].percussion.phisem_sound_ms = 7.98949f;
  t[59].percussion.shimmer = 0.0785283f;
  t[59].percussion.strike_theta = 0.110685f;
  t[59].percussion.tone_direct = 0.623407f;
  t[59].percussion.tone_gain = 2.80816f;
  t[59].percussion.wire_cutoff_hz = 11795.5f;
  t[59].percussion.wire_threshold = 3.10915f;
  t[59].resonance_q = 0.530935f;
  t[59].stereo_spread = 0.967415f;
  t[59].percussion.contact_ms = 5.84916f;
  t[59].percussion.mode_ratios[3] = 8.14817f;

  // --- PhISEM shakers + scrapers ---
  t[54] = make_shaker(32.0f, 120.0f, 2500.0f, 2.0f, 0.2267f);  // Tambourine
  t[58] = make_shaker(24.0f, 400.0f, 2500.0f, 3.0f, 0.2912f);  // Vibraslap
  t[58].amp_env.decay_ms = 4441.28f;
  t[58].amp_env.sustain = 0.807187f;
  t[58].cutoff_hz = 22000.0f;
  t[58].drive = 0.0107823f;
  t[58].percussion.noise_gain = 1.11535f;
  t[58].percussion.phisem_beans = 1.93965f;
  t[58].percussion.phisem_energy_ms = 74.8633f;
  t[58].percussion.phisem_res_hz = 1103.15f;
  t[58].percussion.phisem_res_q = 4.54526f;
  t[58].percussion.phisem_sound_ms = 1.29538f;
  t[58].percussion.plate_gain = 3.34809f;
  t[58].stereo_spread = 0.0521467f;
  t[58].percussion.contact = 1.88036f;
  t[58].percussion.noise_cutoff_hz = 1398.92f;
  t[58].percussion.noise_decay_ms = 106.394f;
  t[58].percussion.noise_q = 1.27322f;
  t[58].percussion.plate_hf_ratio = 0.784063f;
  t[58].percussion.plate_low_hz = 366.777f;
  t[58].percussion.plate_t60_s = 2.07916f;
  t[58].resonance_q = 1.61861f;
  t[69] = make_shaker(24.0f, 90.0f, 4000.0f, 1.0f, 0.3232f);  // Cabasa
  t[69].amp_env.decay_ms = 245.626f;
  t[69].amp_env.sustain = 0.00114147f;
  t[69].cutoff_hz = 3687.04f;
  t[69].drive = 0.261671f;
  t[69].percussion.contact = 1.83336f;
  t[69].percussion.phisem_beans = 4.0173f;
  t[69].percussion.phisem_energy_ms = 31.5817f;
  t[69].percussion.phisem_res_hz = 527.314f;
  t[69].percussion.phisem_res_q = 5.55687f;
  t[69].percussion.phisem_sound_ms = 0.557216f;
  t[69].percussion.plate_gain = 0.516648f;
  t[69].resonance_q = 2.07741f;
  t[69].stereo_spread = 0.162217f;
  t[69].amp_env.attack_ms = 0.0718473f;
  t[69].percussion.contact_ms = 0.521817f;
  t[69].percussion.noise_gain = 1.01606f;
  t[69].percussion.plate_hf_ratio = 0.341536f;
  t[69].percussion.plate_low_hz = 190.613f;
  t[69].percussion.plate_t60_s = 0.443926f;
  t[70] = make_shaker(20.0f, 90.0f, 3200.0f, 1.5f, 0.2173f);  // Maracas
  t[82] = make_shaker(28.0f, 110.0f, 6000.0f, 1.0f, 0.5f);    // Shaker
  // Guiro (mute group 5): ratchet ridge train.
  t[73] = make_scrape(8.0f, 120.0f, 150.0f, 2500.0f, 3.0f, 0.0f, 0.2065f);  // Short Guiro
  t[74] = make_scrape(8.0f, 500.0f, 120.0f, 2500.0f, 3.0f, 0.0f, 0.1437f);  // Long Guiro
  t[73].percussion.exclusive_class = 5;
  t[74].percussion.exclusive_class = 5;
  t[74].amp_env.decay_ms = 271.48f;
  t[74].amp_env.sustain = 0.449402f;
  t[74].cutoff_hz = 3721.74f;
  t[74].drive = 0.435648f;
  t[74].percussion.contact = 3.98175f;
  t[74].percussion.noise_gain = 1.81074f;
  t[74].percussion.phisem_beans = 21.9642f;
  t[74].percussion.phisem_energy_ms = 74.0698f;
  t[74].percussion.phisem_res_hz = 5480.54f;
  t[74].percussion.phisem_res_q = 0.757159f;
  t[74].percussion.phisem_scrape_hz = 23.308f;
  t[74].percussion.phisem_sound_ms = 1.07784f;
  t[74].percussion.plate_gain = 0.259111f;
  t[74].stereo_spread = 0.269538f;
  t[74].gain = 0.3224f;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 1.7655f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);     // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  t[78].amp_env.decay_ms = 112.279f;
  t[78].cutoff_hz = 2689.01f;
  t[78].drive = 0.178906f;
  t[78].percussion.contact = 0.177884f;
  t[78].percussion.noise_gain = 0.0793007f;
  t[78].percussion.phisem_beans = 1.89136f;
  t[78].percussion.phisem_energy_ms = 360.379f;
  t[78].percussion.phisem_res_hz = 531.459f;
  t[78].percussion.phisem_res_q = 1.21424f;
  t[78].percussion.phisem_scrape_hz = 60.9789f;
  t[78].percussion.phisem_sound_ms = 6.01102f;
  t[78].percussion.plate_gain = 1.11689f;
  t[78].resonance_q = 0.531205f;
  t[78].stereo_spread = 0.283496f;
  t[79].percussion.exclusive_class = 2;
  t[79].amp_env.decay_ms = 145.805f;
  t[79].amp_env.sustain = 0.0534685f;
  t[79].cutoff_hz = 6205.98f;
  t[79].drive = 0.610675f;
  t[79].percussion.contact = 0.623269f;
  t[79].percussion.noise_gain = 0.59946f;
  t[79].percussion.phisem_beans = 2.9651f;
  t[79].percussion.phisem_energy_ms = 36.2995f;
  t[79].percussion.phisem_pitch_glide = 0.224625f;
  t[79].percussion.phisem_res_hz = 3318.51f;
  t[79].percussion.phisem_res_q = 13.9804f;
  t[79].percussion.phisem_scrape_hz = 32.457f;
  t[79].percussion.phisem_sound_ms = 7.25467f;
  t[79].percussion.plate_gain = 0.0471479f;
  t[79].stereo_spread = 0.531757f;

  // The body the collisions happen inside, radiating alongside the bright band
  // above rather than through it. Fitted against the measured kit on the bands
  // that reference can still resolve: a tambourine's frame and head are one
  // broad low shelf, a maraca's gourd is a little narrower, and a guiro's is a
  // narrow peak that stands 23 dB over the third of an octave beneath it. The
  // shakers left without one are the ones the measurement says have none - the
  // cabasa is flat from 125 Hz to 1 kHz, and the long guiro's and vibraslap's
  // errors are in the midrange, where a body cannot reach them.
  t[54].percussion.phisem_body_hz = 200.0f;  // Tambourine
  t[54].percussion.phisem_body_q = 0.830588f;
  t[54].percussion.phisem_body_gain = 3.31481f;
  t[54].amp_env.attack_ms = 6.20575f;
  t[54].amp_env.decay_ms = 69.8854f;
  t[54].amp_env.sustain = 0.21115f;
  t[54].cutoff_hz = 4250.53f;
  t[54].percussion.noise_gain = 1.36039f;
  t[54].percussion.phisem_beans = 59.3119f;
  t[54].percussion.phisem_body_hz = 154.259f;
  t[54].percussion.phisem_energy_ms = 113.151f;
  t[54].percussion.phisem_res_hz = 2571.98f;
  t[54].percussion.phisem_res_q = 2.94322f;
  t[54].percussion.phisem_sound_ms = 0.875903f;
  t[54].percussion.plate_gain = 0.113041f;
  t[54].stereo_spread = 0.0216719f;
  t[54].drive = 0.0547653f;
  t[54].percussion.noise_cutoff_hz = 5904.53f;
  t[54].percussion.noise_decay_ms = 516.393f;
  t[54].percussion.noise_q = 2.19088f;
  t[54].percussion.plate_hf_ratio = 0.911664f;
  t[54].percussion.plate_low_hz = 180.348f;
  t[54].percussion.plate_t60_s = 10.0008f;
  t[54].resonance_q = 2.22992f;
  t[70].percussion.phisem_body_hz = 210.0f;  // Maracas
  t[70].percussion.phisem_body_q = 0.820548f;
  t[70].percussion.phisem_body_gain = 2.76066f;
  t[70].amp_env.attack_ms = 2.08564f;
  t[70].amp_env.decay_ms = 418.327f;
  t[70].amp_env.sustain = 0.22385f;
  t[70].cutoff_hz = 2322.54f;
  t[70].drive = 0.636897f;
  t[70].percussion.contact = 2.41447f;
  t[70].percussion.noise_gain = 2.96672f;
  t[70].percussion.phisem_beans = 23.273f;
  t[70].percussion.phisem_body_hz = 126.113f;
  t[70].percussion.phisem_energy_ms = 26.8213f;
  t[70].percussion.phisem_res_hz = 8104.07f;
  t[70].percussion.phisem_res_q = 2.78973f;
  t[70].percussion.phisem_sound_ms = 37.6052f;
  t[70].percussion.plate_gain = 2.22728f;
  t[70].resonance_q = 2.83986f;
  t[70].percussion.contact_ms = 0.136443f;
  t[70].percussion.noise_cutoff_hz = 15167.5f;
  t[70].percussion.noise_decay_ms = 57.7567f;
  t[70].percussion.noise_q = 0.683642f;
  t[70].percussion.plate_hf_ratio = 0.217629f;
  t[70].percussion.plate_low_hz = 183.773f;
  t[70].percussion.plate_t60_s = 0.29673f;
  t[70].stereo_spread = 0.292999f;
  t[73].percussion.phisem_body_hz = 285.0f;  // Short Guiro
  t[73].percussion.phisem_body_q = 2.0653f;
  t[73].percussion.phisem_body_gain = 0.625303f;
  t[73].amp_env.decay_ms = 105.405f;
  t[73].amp_env.sustain = 0.000171761f;
  t[73].percussion.noise_gain = 1.03971f;
  t[73].percussion.phisem_beans = 2.75465f;
  t[73].percussion.phisem_body_hz = 311.563f;
  t[73].percussion.phisem_energy_ms = 679.812f;
  t[73].percussion.phisem_res_hz = 2683.0f;
  t[73].percussion.phisem_res_q = 6.64092f;
  t[73].percussion.phisem_scrape_hz = 51.5885f;
  t[73].percussion.phisem_sound_ms = 67.8975f;
  t[73].percussion.noise_cutoff_hz = 10282.8f;
  t[73].percussion.noise_decay_ms = 19.1165f;
  t[73].percussion.noise_q = 1.80927f;

  for (auto& p : t) p = clamp_synth_patch(p);

    // Development-only per-note voicing override, keyed `d000`..`d127` by drum
    // note (`SONARE_TUNING_OVERRIDES=d038.percussion.wire_buzz=0.5`). See
    // gm_fallback_programs.cpp for why this is compiled out rather than gated.
#if defined(SONARE_TUNING) && SONARE_TUNING
  for (int n = 0; n < 128; ++n) {
    char key[8] = {'d', static_cast<char>('0' + n / 100), static_cast<char>('0' + (n / 10) % 10),
                   static_cast<char>('0' + n % 10), '\0'};
    apply_patch_tuning(t[static_cast<size_t>(n)], key);
  }
#endif
  for (auto& p : t) p = strip_unvoiced_sections(p);
  return t;
}

}  // namespace

const std::array<NativeSynthPatch, 128>& drum_note_table() noexcept {
#if defined(SONARE_TUNING) && SONARE_TUNING
  // The tuning build reads overrides from the environment, so the table can
  // only be built once the process is running.
  static const std::array<NativeSynthPatch, 128> kTable = build_drum_note_table();
#else
  static constexpr std::array<NativeSynthPatch, 128> kTable = build_drum_note_table();
#endif
  return kTable;
}

}  // namespace sonare::midi::synth::detail
