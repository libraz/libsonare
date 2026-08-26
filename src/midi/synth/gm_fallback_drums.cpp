#include "midi/synth/gm_fallback_data.h"
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
// of the two — a value some real kit actually uses, taking no side on which. Of
// the 47 mapped keys, 29 already sit inside that range and are left alone.
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
  t[36] = d.kick;
  t[46] = d.open_hat;
  // One tom patch voices every tom size because the toms are key-tracked: the
  // struck key sets the head frequency, so six keys are six drums.
  t[41] = t[43] = t[45] = t[47] = t[48] = t[50] = d.tom;

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
  t[52].percussion.noise_q = 1.4f;
  t[52].percussion.mode_ratios = {1.0f, 1.19f, 1.51f, 1.83f, 0.0f, 0.0f};

  // Each plate's own partial field. `plate_low_hz` is the lowest partial the
  // network places, so it is the piece's diameter read as a frequency - a 20
  // inch ride reaches lower than a 16 inch crash and far lower than an 8 inch
  // splash - and `plate_t60_s` is how long the plate itself rings, which is
  // bounded by the amplitude envelope's length above and so tracks it. The
  // china is the exception in both: its flange stiffens the plate, which takes
  // its lowest partial up rather than down for its size, and stops it early.
  t[49].percussion.plate_low_hz = 140.0f;
  t[49].percussion.plate_t60_s = 0.60f;
  t[49].percussion.plate_gain = 2.0f;
  t[49].percussion.plate_hf_ratio = 0.5f;
  t[57].percussion.plate_low_hz = 145.0f;
  t[57].percussion.plate_t60_s = 0.56f;
  t[57].percussion.plate_gain = 2.0f;
  t[57].percussion.plate_hf_ratio = 0.5f;
  t[57].percussion.shimmer_cutoff_hz = 3600.0f;
  t[51].percussion.plate_low_hz = 150.0f;
  t[51].percussion.plate_t60_s = 0.32f;
  t[51].percussion.plate_gain = 1.6f;
  t[51].percussion.plate_hf_ratio = 0.55f;
  t[51].percussion.mode_ratios[1] = 3.35f;
  t[51].percussion.noise_q = 0.4f;
  t[51].amp_env.sustain = 0.1f;
  t[51].drive = 0.1f;
  t[59].percussion.plate_low_hz = 145.0f;
  t[59].percussion.plate_t60_s = 2.25f;
  t[59].percussion.plate_gain = 1.6f;
  t[59].percussion.plate_hf_ratio = 0.55f;
  t[59].percussion.mode_ratios[0] = 0.6f;
  t[59].percussion.noise_gain = 0.72f;
  t[59].percussion.noise_q = 0.4f;
  t[59].percussion.wire_buzz = 1.0f;
  t[59].amp_env.sustain = 0.1f;
  t[55].percussion.plate_low_hz = 248.0f;
  t[55].percussion.plate_t60_s = 0.56f;
  t[55].percussion.plate_gain = 1.6f;
  t[55].percussion.phisem_beans = 0.4f;
  // The china's plate is at the top of its clamp. The search asked for more
  // than the range allows, which is a result and not a setting: either the
  // bound is wrong for a piece this small and stiff, or what it wants is not
  // more of this plate.
  t[52].percussion.plate_low_hz = 152.0f;
  t[52].percussion.plate_t60_s = 0.2f;
  t[52].percussion.plate_gain = 4.0f;
  t[52].percussion.plate_hf_ratio = 0.95f;
  t[52].key_track = 1.0f;
  t[52].resonance_q = 0.2828f;
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
  t[49].percussion.plate_air_hz = 4500.0f;
  t[49].percussion.noise_air_hz = 5000.0f;
  t[49].cutoff_hz = 5000.0f;
  t[57].percussion.plate_air_hz = 5250.0f;
  t[57].percussion.noise_air_hz = 6400.0f;
  t[57].percussion.noise_q = 0.88f;
  t[57].cutoff_hz = 1600.0f;
  t[51].percussion.plate_air_hz = 3500.0f;
  t[51].percussion.noise_air_hz = 1600.0f;
  t[51].cutoff_hz = 1875.0f;
  t[59].percussion.plate_air_hz = 3200.0f;
  t[59].percussion.noise_air_hz = 4000.0f;
  t[59].cutoff_hz = 1360.0f;
  t[55].percussion.plate_air_hz = 6000.0f;
  t[55].percussion.noise_air_hz = 6000.0f;
  t[55].percussion.noise_cutoff_hz = 3500.0f;
  t[55].percussion.noise_q = 2.0f;
  t[55].cutoff_hz = 5000.0f;
  t[52].percussion.plate_air_hz = 12500.0f;
  t[52].percussion.noise_air_hz = 5000.0f;
  t[52].cutoff_hz = 2400.0f;

  // --- snares ---
  t[38] = d.snare;  // Acoustic Snare, the archetype
  // Electric Snare: tuned higher and gated shorter, with the shell body and the
  // wire rattle mostly gone - what a drum machine has instead of a snare is a
  // tight noise crack over a short pitched click.
  t[40] = d.snare;
  t[40].amp_env = fallback_env(0.5f, 170.0f, 0.0f, 60.0f);
  t[40].percussion.base_freq_hz = 220.0f;
  t[40].percussion.mode_decay_s = 0.07f;
  t[40].percussion.noise_decay_ms = 110.0f;
  t[40].percussion.noise_cutoff_hz = 2600.0f;
  t[40].percussion.shell_mix = 0.08f;
  t[40].percussion.wire_buzz = 0.35f;

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
  t[44].amp_env.delay_ms = 1.0f;
  t[44].cutoff_hz = 3000.0f;
  t[44].drive = 0.06f;
  t[44].key_track = 0.1f;
  t[44].resonance_q = 1.7675f;
  t[44].percussion.base_freq_hz = 224.0f;
  t[44].percussion.mode_decay_s = 0.46875f;
  t[44].percussion.mode_ratios[1] = 0.536f;
  t[44].percussion.mode_ratios[2] = 2.15f;
  t[44].percussion.tone_gain = 1.5f;
  t[44].percussion.pitch_drop = 0.1f;
  t[44].percussion.pitch_drop_ms = 50.0f;
  t[44].percussion.strike_r = 0.1f;
  t[44].percussion.wire_buzz = 0.24f;
  t[44].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[44].percussion.noise_cutoff_hz = 2600.0f;
  t[44].percussion.noise_decay_ms = 64.3328f;
  t[44].percussion.noise_gain = 1.4f;
  t[44].percussion.noise_q = 1.4f;
  t[44].percussion.plate_gain = 0.32f;
  t[44].percussion.plate_low_hz = 500.0f;
  t[44].percussion.plate_t60_s = 0.25f;
  t[44].percussion.plate_air_hz = 4000.0f;
  t[44].gain = 1.4587f;

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
  t[42].amp_env.attack_ms = 1.81637f;
  t[42].amp_env.decay_ms = 174.0f;
  t[42].amp_env.delay_ms = 1.0f;
  t[42].cutoff_hz = 3500.0f;
  t[42].drive = 0.375f;
  t[42].percussion.base_freq_hz = 393.75f;
  t[42].percussion.mode_decay_s = 0.16f;
  t[42].percussion.mode_ratios[1] = 1.072f;
  t[42].percussion.mode_ratios[2] = 1.032f;
  t[42].percussion.tone_gain = 4.0f;
  t[42].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[42].percussion.noise_cutoff_hz = 3800.0f;
  t[42].percussion.noise_decay_ms = 600.0f;
  t[42].percussion.noise_gain = 2.54656f;
  t[42].percussion.noise_q = 1.0f;
  t[42].percussion.plate_low_hz = 200.0f;
  t[42].percussion.plate_t60_s = 2.4f;
  t[42].percussion.plate_air_hz = 7000.0f;
  t[42].resonance_q = 2.0f;
  t[42].stereo_spread = 0.875561f;
  // Re-gain, in the same change and for the same reason as the ceilings above:
  // the fit is not offered the output gain, because no term it minimises can
  // see one, so the values it chose left the peak 10 dB down and nothing in its
  // report was entitled to notice. Measured across three velocities.
  t[42].gain = 1.387f;
  t[44].percussion.exclusive_class = 1;
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
  t[46].amp_env.attack_ms = 0.535176f;
  t[46].amp_env.decay_ms = 1312.5f;
  t[46].cutoff_hz = 3000.0f;
  t[46].drive = 0.375f;
  t[46].percussion.base_freq_hz = 315.0f;
  t[46].percussion.mode_decay_s = 0.12f;
  t[46].percussion.mode_ratios[0] = 1.5f;
  t[46].percussion.mode_ratios[2] = 1.032f;
  t[46].percussion.tone_gain = 1.5f;
  t[46].percussion.pitch_drop = 0.1f;
  t[46].percussion.pitch_drop_ms = 24.0f;
  t[46].percussion.strike_r = 0.4f;
  t[46].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[46].percussion.noise_cutoff_hz = 3200.0f;
  t[46].percussion.noise_decay_ms = 3213.18f;
  t[46].percussion.noise_gain = 0.392099f;
  t[46].percussion.noise_q = 2.5f;
  t[46].percussion.plate_gain = 0.48f;
  t[46].percussion.plate_low_hz = 400.0f;
  t[46].percussion.plate_air_hz = 7000.0f;
  t[46].resonance_q = 0.795984f;
  t[46].stereo_spread = 0.565048f;
  // Re-gained with the corner: the piece was 17 dB under its reference, which
  // every normalised metric in the comparison reads as correct.
  t[46].gain = 2.5777f;

  // --- wooden idiophones + clicks ---
  t[31] = make_wood(1000.0f, 0.0f, 0.03f, 0.6f);      // Sticks
  t[32] = make_wood(1000.0f, 0.0f, 0.02f, 0.5f);      // Square Click
  t[33] = make_wood(1200.0f, 0.0f, 0.02f, 0.5f);      // Metronome Click
  t[37] = make_wood(820.0f, 0.0f, 0.05f, 0.7f);       // Side Stick
  t[75] = make_wood(2500.0f, 0.0f, 0.025f, 1.3248f);  // Claves (2500 Hz, ~25 ms)
  t[76] = make_wood(1200.0f, 0.0f, 0.06f, 0.6f);      // Hi Wood Block
  t[77] = make_wood(800.0f, 0.0f, 0.07f, 0.6f);       // Low Wood Block
  t[85] = make_wood(1800.0f, 0.0f, 0.02f, 0.5f);      // Castanets

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
  t[60] = make_membrane(260.0f, 0.18f, 0.30f, 0.0f, 0.70f);      // Hi Bongo
  t[61] = make_membrane(180.0f, 0.20f, 0.30f, 0.0f, 0.70f);      // Low Bongo
  t[62] = make_membrane(220.0f, 0.08f, 0.20f, 0.0f, 0.70f);      // Mute Hi Conga
  t[63] = make_membrane(200.0f, 0.25f, 0.30f, 0.0f, 1.1847f);    // Open Hi Conga
  t[64] = make_membrane(130.0f, 0.30f, 0.35f, 0.0f, 1.4014f);    // Low Conga
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 1.1120f);  // High Timbale
  t[66] = make_membrane(200.0f, 0.26f, 0.20f, 550.0f, 0.70f);    // Low Timbale
  t[86] = make_membrane(95.0f, 0.12f, 0.40f, 0.0f, 0.80f);       // Mute Surdo
  t[87] = make_membrane(80.0f, 0.40f, 0.50f, 0.0f, 0.85f);       // Open Surdo
  t[86].percussion.exclusive_class = 6;
  t[87].percussion.exclusive_class = 6;

  // --- whistles (mute group 4) + hand clap ---
  t[71] = make_whistle(1400.0f, 0.12f, 0.7473f);  // Short Whistle
  t[72] = make_whistle(1400.0f, 0.50f, 0.5f);     // Long Whistle
  t[71].percussion.exclusive_class = 4;
  t[72].percussion.exclusive_class = 4;
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
  t[38].percussion.noise_air_hz = 2032.0f;
  t[38].gain = 2.931f;  // Acoustic Snare
  t[39].percussion.noise_air_hz = 3125.0f;
  t[39].gain = 2.5615f;  // Hand Clap
  t[40].percussion.noise_air_hz = 1984.0f;
  t[40].gain = 1.828f;  // Electric Snare
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
  // The cymbals keep their gains and have lost their ceilings: their wash is a
  // band now, and a band already has a top. What each one is bounded by instead
  // is its plate ceiling and its voice corner, beside the plate values above.
  t[49].gain = 1.2698f;  // Crash 1
  t[51].gain = 2.0397f;  // Ride 1
  t[52].gain = 3.7653f;  // China
  t[55].gain = 1.279f;   // Splash
  t[57].gain = 4.0f;     // Crash 2, at the clamp
  t[59].gain = 4.0f;     // Ride 2, at the clamp

  // --- PhISEM shakers + scrapers ---
  t[54] = make_shaker(32.0f, 120.0f, 2500.0f, 2.0f, 0.2267f);  // Tambourine
  t[58] = make_shaker(24.0f, 400.0f, 2500.0f, 3.0f, 0.2912f);  // Vibraslap
  t[69] = make_shaker(24.0f, 90.0f, 4000.0f, 1.0f, 0.3232f);   // Cabasa
  t[70] = make_shaker(20.0f, 90.0f, 3200.0f, 1.5f, 0.2173f);   // Maracas
  t[82] = make_shaker(28.0f, 110.0f, 6000.0f, 1.0f, 0.5f);     // Shaker
  // Guiro (mute group 5): ratchet ridge train.
  t[73] = make_scrape(8.0f, 120.0f, 150.0f, 2500.0f, 3.0f, 0.0f, 0.2065f);  // Short Guiro
  t[74] = make_scrape(8.0f, 500.0f, 120.0f, 2500.0f, 3.0f, 0.0f, 0.1437f);  // Long Guiro
  t[73].percussion.exclusive_class = 5;
  t[74].percussion.exclusive_class = 5;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 1.7655f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);     // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  t[79].percussion.exclusive_class = 2;

  // The body the collisions happen inside, radiating alongside the bright band
  // above rather than through it. Fitted against the measured kit on the bands
  // that reference can still resolve: a tambourine's frame and head are one
  // broad low shelf, a maraca's gourd is a little narrower, and a guiro's is a
  // narrow peak that stands 23 dB over the third of an octave beneath it. The
  // shakers left without one are the ones the measurement says have none - the
  // cabasa is flat from 125 Hz to 1 kHz, and the long guiro's and vibraslap's
  // errors are in the midrange, where a body cannot reach them.
  t[54].percussion.phisem_body_hz = 200.0f;  // Tambourine
  t[54].percussion.phisem_body_q = 0.65f;
  t[54].percussion.phisem_body_gain = 3.2f;
  t[70].percussion.phisem_body_hz = 210.0f;  // Maracas
  t[70].percussion.phisem_body_q = 1.0f;
  t[70].percussion.phisem_body_gain = 2.1f;
  t[73].percussion.phisem_body_hz = 285.0f;  // Short Guiro
  t[73].percussion.phisem_body_q = 5.5f;
  t[73].percussion.phisem_body_gain = 0.4f;

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
