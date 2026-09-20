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
  // Held rather than decaying, so what a tom's ring is comes from the head's
  // own damping. A 400 ms envelope decay was cutting the modes off at a
  // quarter of the length the module holds them for, and no per-key
  // `mode_decay_s` could be read back through it.
  d.tom.amp_env = fallback_env(0.5f, 0.0f, 1.0f, 120.0f);
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
// takes the level with it, so the balance is re-measured after any fit. What
// holds a key short of the range is that rule and not the knob: the bank's raw
// voices span 33 dB before any gain is applied, and `gain` is the only lever
// that moves one without its timbre, so its clamp carries the whole spread.
// The lever is exact — applied after the drive, the filter and the envelope,
// doubling it moves the rendered peak +6.02 dB at every velocity and moves no
// other dimension of the comparison.
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
  // The module reads a 13.47 ms attack against the fitted 0.65: a windowed RMS
  // needs most of a cycle of a sub-100 Hz fundamental before it reads arrival.
  t[35].amp_env.attack_ms = 13.47f;
  t[35].amp_env.decay_ms = 73.3945f;
  t[35].amp_env.sustain = 0.876601f;
  t[35].cutoff_hz = 22000.0f;
  t[35].drive = 0.133416f;
  t[35].percussion.contact = 0.454382f;
  t[35].percussion.mode_decay_s = 0.0391114f;
  t[35].percussion.mode_ratios[0] = 0.857872f;
  t[35].percussion.mode_ratios[1] = 5.5857f;
  t[35].percussion.noise_cutoff_hz = 297.277f;
  // The beater thud lost energy faster than the octaves the module resolves a
  // rate in: -218 dB/s measured against -140 modelled, so it hangs on longer.
  t[35].percussion.noise_decay_ms = 70.0f;
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
  t[35].stereo_spread = 0.187262f;
  t[35].gain = 2.7519f;
  // Not a strainer: a high-passed noise layer the fit reached for on a piece
  // with no wires, and it carries 8 dB of this kick's level. Converted to the
  // fractional threshold rather than deleted, because deleting it is a
  // re-voicing and the piece was heard as it stands.
  t[35].percussion.wire_buzz = 1.5f;
  t[35].percussion.wire_threshold = 0.25f;
  t[36] = d.kick;
  t[36].gain = 5.891f;
  // -318 dB/s measured against -509 modelled in every matched octave: the
  // beater thud outlasts the membrane fundamental it sits under.
  t[36].percussion.noise_decay_ms = 54.0f;
  // The module's own attack reads 9.98 ms rather than the base envelope's
  // 0.5 ms floor.
  t[36].amp_env.attack_ms = 9.98f;
  // The six toms are six drums. Each takes the geometry the name on its key
  // carries — 18, 16, 14, 13, 12 and 10 inch, with the depth a drum that size
  // is built to — and a head frequency and ring read off the module rather than
  // off a recording of somebody's kit: how a set is pitched and how far its
  // members sit apart is a relation between them, which is the module's.
  //
  // Key-tracking spans 1.68:1 and the module spans 2.04:1, so the pitch is
  // pinned; the GS per-note pitch NRPN still multiplies on top. Both are needed
  // together, because the frequency is what sets the ring: the module's low
  // bands lose 47-65 dB/s under the floor tom and 97-106 under the high tom.
  //
  // `air_spring` is the one scalar here a fit still has to land. Its starting
  // value scales the 1.43 the reference snare's measured 1.56 split implies by
  // 1 / (depth * f01^2) — the cavity's stiffness against the head's — which
  // puts the floor tom's axisymmetric split at 1.73 and the high tom's at 1.40.
  struct TomSpec {
    int note;
    float base_hz;
    float diameter_m;
    float depth_m;
    float air_spring;
    float mode_decay_s;
    float mallet_ms;
    float pitch_drop;
    float pitch_drop_ms;
    float gain;
  };
  // Four of the seven columns are read off the module's own recordings rather
  // than derived: `mode_decay_s` is 60 dB over the slowest rate it loses in the
  // three bands the head sounds in, `air_spring` is its measured axisymmetric
  // split squared less one, and the pitch drop is the descending strike it
  // measures at onset. The upper modes die as 1 / ratio, which is the shape the
  // module has too: a tom's bands run -50 to -100 dB/s at the head and -260 to
  // -400 at the top of the range.
  constexpr TomSpec kToms[] = {
      {41, 79.7f, 0.457f, 0.406f, 0.77f, 1.28f, 2.26f, 0.607f, 60.0f, 3.58f},
      {43, 94.5f, 0.406f, 0.406f, 1.02f, 1.03f, 2.18f, 0.586f, 50.0f, 3.67f},
      {45, 102.9f, 0.356f, 0.356f, 0.32f, 1.09f, 2.09f, 0.414f, 35.0f, 2.63f},
      {47, 120.4f, 0.330f, 0.279f, 0.37f, 1.09f, 2.02f, 0.414f, 30.0f, 2.32f},
      {48, 137.7f, 0.305f, 0.254f, 0.37f, 0.69f, 1.85f, 0.379f, 25.0f, 1.96f},
      {50, 162.3f, 0.254f, 0.203f, 0.39f, 0.59f, 1.77f, 0.379f, 25.0f, 1.60f},
  };
  // The first eight circular-membrane modes as alpha_mn / alpha_01, which is
  // what a ratio is for. Two of the eight are axisymmetric, so the air spring
  // spawns two partners and the voice places ten of its twelve slots.
  constexpr std::array<float, kMaxPercussionModes> kMembraneRatios = {
      1.0f,     1.59335f, 2.13556f, 2.29545f, 2.65311f, 2.91733f,
      3.15547f, 3.50017f, 0.0f,     0.0f,     0.0f,     0.0f};
  // Each note is assigned on its own line rather than in a loop over the table:
  // `writeback.py` anchors a fitted drum value on the last line that starts
  // with `t[<note>]`, so a loop leaves every tom with nowhere for a fit to write
  // its result back to and the calibration round reports instead of adopting.
  auto tom_patch = [&](const TomSpec& tom) {
    NativeSynthPatch p = d.tom;
    p.percussion.base_freq_hz = tom.base_hz;
    p.percussion.head_diameter_m = tom.diameter_m;
    p.percussion.shell_depth_m = tom.depth_m;
    p.percussion.air_spring = tom.air_spring;
    p.percussion.mode_decay_s = tom.mode_decay_s;
    // Damping rises with frequency, and by well under the proportional law the
    // engine assumed: the module's tom bands lose about 1.45x the rate of the
    // band below rather than 2x, and a fit over the whole spectrum settles
    // shallower still once the dense region is carried by the noise layer.
    p.percussion.mode_decay_exp = 0.21f;
    p.percussion.mallet_ms = tom.mallet_ms;
    p.percussion.mallet_vel_exp = 0.33f;
    // How far the kit's own members sit apart in level is the module's to say,
    // and it puts 7.8 dB between the floor tom and the high one where the
    // voice's own radiation produces 3.
    p.gain = tom.gain;
    p.percussion.num_modes = 8;
    p.percussion.mode_ratios = kMembraneRatios;
    // Where a stick lands: far enough out to excite the m >= 1 modes the drum's
    // pitch is heard in, short of the rim that kills the fundamental.
    p.percussion.strike_r = 0.45f;
    // The head tightens under the stick, and by a lot: the module measures the
    // drop at 38% on the high tom and 61% on the floor tom, over 25 to 60 ms.
    // Bigger drums both drop further and take longer to come back.
    p.percussion.pitch_drop = tom.pitch_drop;
    p.percussion.pitch_drop_ms = tom.pitch_drop_ms;
    // The stick's own radiation, which every resonator here is too slow to
    // supply in the first milliseconds.
    p.percussion.contact = 0.51f;
    p.percussion.contact_ms = 0.88f;
    // A membrane's modes run far past the twelve the engine can place, and what
    // is above them is dense enough to be a band rather than a set of lines.
    p.percussion.noise_gain = 0.61f;
    p.percussion.noise_cutoff_hz = 1180.0f;
    p.percussion.noise_q = 0.70f;
    p.percussion.shell_mix = 0.26f;
    return p;
  };
  t[41] = tom_patch(kToms[0]);
  t[43] = tom_patch(kToms[1]);
  t[45] = tom_patch(kToms[2]);
  t[47] = tom_patch(kToms[3]);
  t[48] = tom_patch(kToms[4]);
  t[50] = tom_patch(kToms[5]);
  // --- cymbals ---
  //
  // Seven keys, seven plates, one table. They cannot share a patch the way the
  // toms do - a cymbal pins `base_freq_hz`, so one patch would render the same
  // plate at the same pitch on every key - but they do share a structure, and
  // what separates them is six numbers each.
  //
  // Every behavioural column is read off the module's own recordings, which is
  // where `policy.json` puts a kit's ring, its damping and how its members sit
  // against one another. Three of those readings contradicted what was here,
  // and each had reached the tree as a fitted value:
  //
  // - the ring. A cymbal loses 17 to 30 dB/s in the module, flat across every
  //   octave it sounds in, which is a plate ringing 2.1 to 3.2 seconds. The
  //   plates here held 0.08 to 1.6, under an amplitude envelope that had
  //   already decayed to nothing - so the envelope was the only ring being
  //   heard and no plate value could be read back through it.
  // - the damping's colour. Flat means `plate_hf_ratio` near one; the fitted
  //   values ran to 0.15, which is a top dying six times faster than a bottom.
  // - the band. The module's cymbals peak between 3.1 and 8 kHz and centre
  //   between 4.4 and 7.3; the voice carried a low-pass at 368 Hz on one crash
  //   and 515 on the china, and a plate ceiling of 6 Hz on the second ride.
  //   A cymbal below 400 Hz is not a dark cymbal, it is not a cymbal.
  //
  // What stays fitted is the wash's own shape, which is colour and belongs to
  // the sampled kit rather than here.
  struct CymbalSpec {
    int note;
    float base_hz;       // the lowest partial the ring modes place
    float mode_decay_s;  // those modes' t60
    float tone_gain;     // how much of the piece is those modes rather than the plate
    float wash_hz;       // the band the wash is centred on: the module's own peak band
    float wash_ms;       // its one-pole decay, which runs about as long as the piece
    float shimmer;       // the post-strike bloom, and the family's brightness control
    float plate_low_hz;  // the lowest partial the network places: the diameter as a pitch
    float
        plate_t60_s;  // the plate's ring, set by how long the module's own hit takes to fall 20 dB
    float plate_gain;
    float contact;  // the stick's own radiation, which is the strike's peak
    float gain;
  };
  // `wash_ms` is `plate_t60_s / 6.9`, the one-pole time constant that loses 60
  // dB over the same span: the wash is what keeps re-exciting the plate, and
  // one that stops early leaves the partials ringing on undisturbed, which
  // reads as a bell rather than as a cymbal.
  //
  // `shimmer` is the one knob the family's brightness hangs on, which is not
  // what its name suggests. It is a high-passed wash driven by the squared
  // modal energy, and nothing bounds it above, so every unit of it lands in the
  // top third-octave band there is. Taking it off note 49 alone moves that
  // piece's centroid from 57 % over the module to 4 %, its peak band from
  // 12.5 kHz to 3.15, and its crest from 4.2 dB under the module to 4.0 over;
  // the plate and the mode bank each move the centroid by under 5 %. On the
  // open hi-hat the same knob carried the level and the attack as well - at 4.0
  // the piece peaked 58 ms late and 7.3 dB loud, and at 0.5 it peaks at the
  // strike and within 0.1 dB.
  //
  // `plate_t60_s` is set against the module's `decay_ms` - peak to -20 dB - and
  // not against its per-octave rate, which the two rides and the splash make
  // the difference visible on: they lose 22 to 67 dB/s per octave and still
  // fall their first 20 dB in 220 to 287 ms, because what follows is a long
  // quiet tail rather than the same slope continuing.
  constexpr CymbalSpec kCymbals[] = {
      // Crashes. Wash-dominated, a dense field from 3.4 kHz up, 16 and 18 inch.
      // 49 peaks 61 ms after the strike in the module and the voice peaked at
      // it: `contact` at 12 put the stick's own radiation over the bloom, and
      // the bloom is what a crash is. Below about 4 the contact sits under the
      // plate's own peak and stops being audible at all, so it is set there and
      // the level it was carrying is paid back in `gain`. Both already placed
      // their own `tone_f0_hz` within 0.4 doublings of the module before this
      // round; raising `plate_low_hz` to the field floor moved each a full
      // doubling further out, so it is left at the unmeasured value.
      {49, 3413.0f, 1.10f, 0.20f, 4000.0f, 348.0f, 2.5f, 220.0f, 2.40f, 0.80f, 2.0f, 0.320f},
      {57, 3569.0f, 1.55f, 0.22f, 8000.0f, 406.0f, 0.8f, 174.0f, 2.80f, 0.80f, 8.5f, 0.223f},
      // Rides. Played on the shoulder with the tip, so a defined ping over a
      // wash that stays out of its way - the module reads them 11 dB more
      // peaked than the crashes. Both hold a low body pair near 360 Hz that no
      // crash does, which is what `base_freq_hz` reaches down to here; that pair
      // is a body and not the ping, so `tone_gain` stays at 0.12 rather than the
      // 0.5 that once made it the loudest thing in either piece. `plate_low_hz`
      // was left inside that same body register (116-141 Hz) rather than at the
      // field the module actually reads above the pair - its next partial sits
      // at 2351 and 2491 Hz, so the network's own floor now starts there instead.
      // Raising the floor rang 59 a doubling long against its own 2.00 s
      // `plate_t60_s` (51's shorter 1.20 s needed no change); 0.45 s lands the
      // ring back on the module, but that is a fit rather than its 287 ms
      // decay reading read straight - 51 needs the same reading scaled up
      // 5.2x at the same floor, so the floor's mode count is not what was
      // setting the multiplier and this is left as an open question.
      {51, 352.0f, 2.20f, 0.12f, 6300.0f, 174.0f, 0.3f, 2351.0f, 1.20f, 0.60f, 16.0f, 0.149f},
      {59, 373.0f, 2.80f, 0.12f, 6300.0f, 290.0f, 0.2f, 2491.0f, 0.45f, 0.55f, 16.0f, 0.124f},
      // The ride bell, struck on the cup. The module reads one partial at
      // 3052 Hz standing 13 dB over its neighbours, which is the one cymbal
      // here that is a tone rather than a field. Its `tone_f0_hz` already sits
      // on the module's own reading before this round; raising `plate_low_hz`
      // to the field's faint 1459 Hz floor underneath moved nothing measurable
      // here, so it is left at the unmeasured value rather than carrying a
      // change with no reading behind it.
      {53, 3052.0f, 2.22f, 1.40f, 3150.0f, 322.0f, 0.0f, 569.0f, 2.22f, 0.30f, 9.8f, 0.146f},
      // The china's upturned flange concentrates the wash into one harsh band
      // instead of spreading it up the spectrum, and stiffens the plate, which
      // takes its lowest partial up rather than down for its size. Its own
      // `tone_f0_hz` sat within 0.3 doublings of the module before this round;
      // reading `plate_low_hz` up to the field floor moved it to 0.8 out, so it
      // is left at the unmeasured value.
      {52, 1576.0f, 0.35f, 0.70f, 4000.0f, 246.0f, 0.5f, 260.0f, 1.70f, 0.70f, 7.5f, 0.238f},
      // A 10 inch splash: the one piece whose damping is not flat, losing
      // 100 dB/s at the bottom against 45 in the middle, because a plate that
      // small barely supports its lowest modes at all.
      // It is also the only piece whose first 20 dB and whose per-octave rate
      // say the same thing - 282 ms against 67 dB/s - where every other cymbal
      // here falls fast and then trails, so its plate is set short and its wash
      // with it. Its contact comes down with the crash's: the module takes
      // 21 ms to reach its peak and a contact over the plate's own peak puts
      // the model's at the strike. The module's own field starts at 2196 Hz;
      // moving `plate_low_hz` there roughly halved the piece's decay (292 ms
      // to 156, against a reference of 282), so it is left at the unmeasured
      // 563 Hz this round could not improve on without breaking it.
      {55, 3663.0f, 0.30f, 0.30f, 4000.0f, 94.0f, 0.3f, 563.0f, 0.65f, 0.75f, 2.0f, 0.760f},
      // The open hi-hat: two small cymbals held apart, so it is a plate like
      // the rest and not the dark band the kit had here. The module reads its
      // centre of gravity at 8.6 kHz, higher than any other piece, and its
      // damping flat at 31 to 43 dB/s - where the voice carried a low-pass at
      // 2.9 kHz and a top dying a hundred times faster than its bottom.
      {46, 3770.0f, 1.94f, 0.25f, 10000.0f, 167.0f, 0.5f, 700.0f, 1.15f, 0.80f, 4.0f, 0.200f},
      // The two closed hats, which are the same pair held shut: the module
      // reads them at 9.4 and 9.5 kHz with almost no peak structure at all,
      // which is a plate stopped early rather than a noise band. The pedal
      // hat also holds a partial at 380 Hz that the stick-closed one does
      // not - the two cymbals meeting each other rather than a stick.
      // Their gate is 60 ms, which is the whole piece and also the shimmer's own
      // buildup, so nothing measures a bloom on either of them. They take the
      // open hat's value because the three are one mechanism at three openings,
      // not because a reading asked for it.
      {42, 3745.0f, 0.30f, 0.25f, 9500.0f, 43.0f, 0.5f, 900.0f, 0.30f, 0.80f, 4.0f, 0.150f},
      {44, 3768.0f, 0.35f, 0.25f, 9500.0f, 51.0f, 0.5f, 380.0f, 0.35f, 0.80f, 4.0f, 0.104f},
  };
  auto cymbal_patch = [&](const CymbalSpec& c) {
    NativeSynthPatch p = d.cymbal;
    // Held, so how long a plate rings is the plate's answer. A one-shot takes
    // no note-off, so the slot is reclaimed on the level reading instead.
    p.amp_env = fallback_env(0.5f, 0.0f, 1.0f, 120.0f);
    p.percussion.base_freq_hz = c.base_hz;
    p.percussion.mode_decay_s = c.mode_decay_s;
    // Flat across every octave the piece sounds in, which is what the module
    // measures and what a plate of metal does.
    p.percussion.mode_decay_exp = 0.05f;
    p.percussion.tone_gain = c.tone_gain;
    p.percussion.noise_cutoff_hz = c.wash_hz;
    p.percussion.noise_decay_ms = c.wash_ms;
    p.percussion.shimmer = c.shimmer;
    p.percussion.plate_low_hz = c.plate_low_hz;
    p.percussion.plate_t60_s = c.plate_t60_s;
    p.percussion.plate_gain = c.plate_gain;
    p.percussion.plate_hf_ratio = 0.95f;
    // The tip of the stick, which reaches the listener without passing through
    // the plate. Nothing else here peaks at the strike - the wash drives the
    // network and the network takes time to fill - and the module reads a
    // ride 31 dB over its own held level against the 11 the plate alone gives.
    p.percussion.contact = c.contact;
    p.percussion.contact_ms = 0.3f;
    p.gain = c.gain;
    return p;
  };
  // One line per key, as the toms are: `writeback.py` anchors a fitted value on
  // the last line that starts with `t[<note>]`, and a loop leaves a fit with
  // nowhere to put its result.
  t[49] = cymbal_patch(kCymbals[0]);
  t[57] = cymbal_patch(kCymbals[1]);
  t[51] = cymbal_patch(kCymbals[2]);
  t[59] = cymbal_patch(kCymbals[3]);
  t[53] = cymbal_patch(kCymbals[4]);
  t[52] = cymbal_patch(kCymbals[5]);
  t[55] = cymbal_patch(kCymbals[6]);
  t[46] = cymbal_patch(kCymbals[7]);
  t[42] = cymbal_patch(kCymbals[8]);
  t[44] = cymbal_patch(kCymbals[9]);
  // The three hats mute each other: a stick on the open pair stops the closed
  // one, and the foot stops both.
  t[46].percussion.exclusive_class = 1;
  t[46].amp_env.release_ms = 40.0f;
  t[42].percussion.exclusive_class = 1;
  t[44].percussion.exclusive_class = 1;
  // The china's wash is a band rather than a corner, and a narrow one: that is
  // what trashy is, and it is a different filter rather than a different
  // corner. Its partials are pulled off the plate ratios the others share until
  // nothing in the sound reads as a pitch.
  t[52].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[52].percussion.noise_q = 3.0091f;
  t[52].percussion.mode_ratios = {1.0f, 1.19f, 1.51f, 1.83f, 0.0f, 0.0f};
  // The bell is the one piece carried by its modes, so its ring is narrow
  // rather than dense and its wash is only the stick.
  t[53].percussion.mode_ratios = {1.0f, 1.40f, 2.76f, 3.27f, 0.0f, 0.0f};
  t[53].percussion.num_modes = 4;
  t[53].percussion.noise_gain = 0.35f;

  // --- snares ---
  // Both take the snare archetype here and are voiced below, where the drum's
  // structure is written out. Splitting them across two places is how the
  // electric snare ended up carrying a base frequency set here and a mode count
  // set there that contradicted it.
  t[38] = d.snare;  // Acoustic Snare
  t[40] = d.snare;  // Electric Snare

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
  t[75].stereo_spread = 0.681734f;
  t[75].percussion.contact_ms = 0.114987f;
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
  t[76].resonance_q = 2.80532f;
  t[76].stereo_spread = 0.899155f;
  t[76].percussion.contact_ms = 5.24755f;
  t[76].percussion.plate_hf_ratio = 0.542482f;
  t[76].percussion.plate_low_hz = 162.88f;
  t[76].percussion.plate_t60_s = 0.218749f;
  t[76].percussion.tone_direct = 0.847765f;
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
  t[77].resonance_q = 2.19365f;
  t[77].stereo_spread = 0.165385f;
  t[77].percussion.contact_ms = 0.283157f;
  t[77].percussion.plate_hf_ratio = 0.547711f;
  t[77].percussion.plate_low_hz = 439.51f;
  t[77].percussion.plate_t60_s = 0.061714f;
  t[77].percussion.tone_direct = 0.316542f;
  t[77].gain = 1.0003f;
  t[85] = make_wood(1800.0f, 0.0f, 0.02f, 0.5f);  // Castanets

  // --- metal idiophones + bells ---
  t[34] =
      make_metal(1500.0f, {1.0f, 2.8f, 5.4f, 0.0f, 0.0f, 0.0f}, 3, 0.3f, 0.4f);  // Metronome Bell
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
  t[68].stereo_spread = 0.763887f;
  t[68].percussion.plate_hf_ratio = 0.549104f;
  t[68].percussion.plate_low_hz = 311.489f;
  t[68].percussion.plate_t60_s = 0.218884f;
  t[68].percussion.tone_direct = 0.829661f;
  t[68].resonance_q = 12.1564f;
  t[68].gain = 0.2071f;
  t[83] =
      make_metal(2500.0f, {1.0f, 1.7f, 2.4f, 0.0f, 0.0f, 0.0f}, 3, 0.40f, 0.35f);  // Jingle Bell
  t[84] = make_metal(3000.0f, {1.0f, 1.6f, 2.3f, 3.1f, 0.0f, 0.0f}, 4, 1.50f, 0.30f);  // Belltree

  // Triangle: high inharmonic modes; mute short, open long (mute group 3).
  // The module reads one bar under both keys, with partials at 1692, 3112,
  // 5494 and 8552 Hz - the 5.5 kHz one standing 7 dB over the rest, which is
  // what makes a triangle a pitch rather than a shimmer. Written against 3112
  // because that is the lowest of the set the ear can hear as a fundamental.
  // Written loudest first, because mode index is the only amplitude control the
  // modal core has: its weight is 1/(k+1) in the index, so slot 0 is the
  // strongest partial and the set has to be ordered by level rather than by
  // frequency. In rising frequency the same four are 1693, 3112, 5494 and 8552
  // Hz, and the module reads 5494 as the loudest with 1693 the quietest of the
  // four - written in frequency order the voice peaked at 1.6 kHz against a
  // module peaking at 5.
  const std::array<float, kMaxPercussionModes> triangle_ratios = {1.765f, 2.748f, 1.0f,
                                                                  0.544f, 0.0f,   0.0f};
  // Held open it loses 27 to 36 dB/s, flat across every octave: a bar of metal
  // damps the same at the top of its range as at the bottom. Held by the hand
  // it stops in a tenth of that.
  t[80] = make_metal(3112.0f, triangle_ratios, 4, 0.22f, 1.610f);  // Mute Triangle
  t[81] = make_metal(3112.0f, triangle_ratios, 4, 2.20f, 0.742f);  // Open Triangle
  t[80].percussion.exclusive_class = 3;
  t[81].percussion.exclusive_class = 3;
  // Not a strainer: a high-passed noise layer on a piece with no wires, and it
  // is the only thing here that fills the gaps between the four partials. What
  // it costs is measured, so it is set by that rather than deleted. At 3.7 it
  // WAS the spectrum - 8.6 dB of flatness against the module's 40.8, a bar
  // reading as a noise band - and at 0 the same four partials read 70, so the
  // engine overshoots the module's sparseness rather than failing to reach it
  // and the layer's job is to come back part of the way. It is gated on the
  // membrane's swing and shuts off as a cliff, which is what the per-octave
  // decay charges: 168 dB/s at 3.7 against the module's 32, and 98 at 0.25.
  t[81].percussion.wire_buzz = 0.25f;
  // The beater is a metal rod on a metal bar, so its own radiation reaches the
  // listener without the modes: one pulse rather than a band, which is why it
  // buys the strike back without touching a single third-octave cell. The piece
  // measured 7.5 dB less peaked than the module with no contact at all.
  t[81].percussion.contact = 3.0f;

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
  t[60].stereo_spread = 0.202579f;
  t[60].percussion.contact_ms = 0.634576f;
  t[60].percussion.plate_hf_ratio = 1.0f;
  t[60].percussion.plate_low_hz = 290.222f;
  t[60].percussion.plate_t60_s = 0.419356f;
  t[60].percussion.tone_direct = 0.873151f;
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
  t[61].stereo_spread = 0.418339f;
  t[61].percussion.contact_ms = 0.109856f;
  t[61].percussion.num_modes = 3;
  t[61].percussion.plate_hf_ratio = 0.695523f;
  t[61].percussion.plate_low_hz = 295.289f;
  t[61].percussion.plate_t60_s = 0.306712f;
  t[61].percussion.tone_direct = 0.915486f;
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
  t[62].stereo_spread = 0.148505f;
  t[62].amp_env.sustain = 0.498882f;
  t[62].percussion.contact_ms = 2.07134f;
  t[62].percussion.num_modes = 5;
  t[62].percussion.plate_hf_ratio = 0.873691f;
  t[62].percussion.plate_low_hz = 459.58f;
  t[62].percussion.plate_t60_s = 0.0709381f;
  t[62].percussion.tone_direct = 0.204074f;
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
  t[63].gain = 1.1392f;
  // Not a strainer: a high-passed noise layer the fit reached for on a piece
  // with no wires, and it carries 5 dB of this conga's tonality. Converted to the
  // fractional threshold rather than deleted, because deleting it is a
  // re-voicing and the piece was heard as it stands.
  t[63].percussion.wire_buzz = 3.0f;
  t[63].percussion.wire_threshold = 0.4f;
  t[64] = make_membrane(130.0f, 0.30f, 0.35f, 0.0f, 1.4014f);  // Low Conga
  // The module reads a 6.98 ms attack against the fitted envelope's 0.1.
  t[64].amp_env.attack_ms = 6.98f;
  t[64].amp_env.decay_ms = 92.8306f;
  t[64].amp_env.sustain = 0.466948f;
  t[64].cutoff_hz = 11671.9f;
  t[64].drive = 0.895751f;
  t[64].percussion.contact = 0.346301f;
  // -204 dB/s averaged over the module's octaves is a 0.29 s t60, against the
  // fitted 0.69; shortened toward it.
  t[64].percussion.mode_decay_s = 0.22f;
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
  t[64].stereo_spread = 0.881839f;
  t[64].percussion.contact_ms = 0.072924f;
  t[64].percussion.plate_gain = 0.00894634f;
  t[64].resonance_q = 0.639576f;
  t[64].gain = 1.286f;
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 1.1120f);  // High Timbale
  t[65].amp_env.attack_ms = 0.0703342f;
  // A 2.16 s decay held the piece loud across the whole window, so the strike
  // had no fall to stand above: 22.49 dB of crest measured, 10.7 short.
  t[65].amp_env.decay_ms = 130.0f;
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
  t[65].stereo_spread = 0.260304f;
  t[65].percussion.contact_ms = 0.314127f;
  t[65].percussion.plate_hf_ratio = 0.905f;
  t[65].percussion.plate_low_hz = 294.406f;
  t[65].percussion.plate_t60_s = 0.661604f;
  t[65].percussion.tone_direct = 0.907509f;
  t[65].resonance_q = 1.33548f;
  t[65].percussion.shell_num_modes = 2;
  t[65].gain = 1.356f;
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
  t[66].resonance_q = 6.79381f;
  t[66].stereo_spread = 0.771765f;
  t[66].percussion.contact_ms = 0.153855f;
  t[66].percussion.plate_hf_ratio = 0.618336f;
  t[66].percussion.plate_low_hz = 1556.17f;
  t[66].percussion.plate_t60_s = 0.243625f;
  t[66].percussion.shell_num_modes = 3;
  t[66].percussion.tone_direct = 0.747727f;
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
  t[71].stereo_spread = 0.739068f;
  t[71].percussion.num_modes = 0;
  t[71].percussion.plate_hf_ratio = 0.518624f;
  t[71].percussion.plate_low_hz = 392.37f;
  t[71].percussion.plate_t60_s = 0.141835f;
  t[71].resonance_q = 0.869943f;
  t[71].percussion.contact_ms = 0.147553f;
  t[71].gain = 0.6823f;
  t[72].percussion.exclusive_class = 4;
  t[72].amp_env.attack_ms = 69.341f;
  // Zero sustain, as the acoustic snare above: a one-shot that sustains
  // plateaus rather than falls, and never loses 20 dB inside the window.
  t[72].amp_env.decay_ms = 350.0f;
  t[72].amp_env.sustain = 0.0f;
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
  t[72].stereo_spread = 0.362584f;
  t[72].amp_env.release_ms = 53.3108f;
  t[72].percussion.plate_hf_ratio = 0.609399f;
  t[72].percussion.plate_low_hz = 194.547f;
  t[72].percussion.plate_t60_s = 13.5899f;
  t[72].resonance_q = 2.82361f;
  t[72].percussion.contact_ms = 0.0743891f;
  t[72].gain = 0.0873f;
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
  // Acoustic Snare, voiced from the drum. The batter head's (0,1) radiates
  // poorly against the air the shell encloses while the (1,1) above it radiates
  // well, which is why the reference peaks in the 250 Hz band and dips at 200;
  // the shell fills 315-430, the stick on the head carries the 600-1200 valley,
  // and everything from 1.5 kHz up is the strainer.
  //
  // It replaces a fit that had reached the reference's numbers by abandoning all
  // of that - one mode at 16.9x the base, a six-fold pitch drop, a 228 Hz noise
  // "crack", a plate carrying the ring, and the shell and the wires both
  // switched off. The wires were the measured finding: 30 of the 33 kit notes
  // carrying a fitted `wire_buzz` rendered no rattle whatsoever, both snares
  // among them.
  // The batter and resonant heads are coupled through the air the shell
  // encloses, so the (0,1) arrives as a pair - the two heads in phase at 160 Hz
  // and out of phase at 250, which is why the reference peaks in the 250 Hz band
  // and dips at 200. Above them the ordinary membrane ladder, and the shell
  // filling 165-400 over the top.
  t[38].percussion.num_modes = 5;
  // Re-based on the 160 Hz partner rather than the 250 Hz one: `damping`
  // clamps any ratio under 1 to 1, so the two heads' pair used to share the
  // 250 Hz mode's decay instead of each ringing on its own. Absolute
  // frequencies are unchanged (160, 250, 397.5, 535, 575 Hz).
  t[38].percussion.base_freq_hz = 160.0f;
  t[38].percussion.mode_ratios = {1.5625f, 1.0f, 2.4844f, 3.3438f, 3.5938f, 0.0f};
  t[38].percussion.mode_m = {0, 0, 1, 2, 0, 0};
  t[38].percussion.mode_alpha = {2.4048f, 2.4048f, 3.8317f, 5.1356f, 5.5201f, 0.0f};
  // The 160 Hz mode carries this figure directly (module: 67.96 dB/s, a 0.88 s
  // t60).
  t[38].percussion.mode_decay_s = 0.85f;
  // The module's octave bands fall at close to one common rate from 63 Hz to
  // 8 kHz rather than doubling per octave, which is the membrane-in-air
  // exponent this engine documents (0.5) rather than the default law's 1.
  t[38].percussion.mode_decay_exp = 0.5f;
  t[38].percussion.tone_gain = 1.0f;
  t[38].percussion.pitch_drop = 0.25f;
  t[38].percussion.pitch_drop_ms = 20.0f;
  // Off-centre, where a snare is played.
  t[38].percussion.strike_r = 0.5f;
  t[38].percussion.strike_theta = 0.0f;
  t[38].percussion.shell_mix = 0.8f;
  t[38].percussion.shell_num_modes = 4;
  t[38].percussion.shell_freq_hz = {165.0f, 205.0f, 330.0f, 400.0f};
  t[38].percussion.shell_t60_s = {0.12f, 0.1f, 0.08f, 0.07f};
  t[38].percussion.shell_weight = {1.0f, 4.0f, 5.0f, 3.0f};
  t[38].percussion.noise_gain = 0.55f;
  t[38].percussion.noise_decay_ms = 120.0f;
  t[38].percussion.noise_cutoff_hz = 1500.0f;
  t[38].percussion.noise_q = 0.8f;
  t[38].percussion.contact = 0.3f;
  t[38].percussion.contact_ms = 0.07f;
  // The strainer: high-passed into its own band, bounded above by the radiated
  // ceiling so the rattle sits where a wire bed sits instead of running to
  // Nyquist, and ringing 60 ms past the head that started it.
  t[38].percussion.wire_buzz = 0.15f;
  t[38].percussion.wire_threshold = 0.12f;
  t[38].percussion.wire_cutoff_hz = 1500.0f;
  t[38].percussion.wire_decay_ms = 400.0f;
  t[38].percussion.noise_air_hz = 6000.0f;
  t[38].percussion.plate_gain = 0.0f;
  t[38].percussion.tone_direct = 1.0f;
  t[38].cutoff_hz = 9000.0f;
  t[38].resonance_q = 0.7f;
  t[38].drive = 0.0f;
  // Zero sustain, because a one-shot voice ignores note-off and a patch that
  // sustains never frees its slot. The fit it replaces held 0.26.
  t[38].amp_env = fallback_env(0.5f, 600.0f, 0.0f, 80.0f);
  // Level read 2.18 dB quiet against the module; gain is linear in level, so
  // this is the exact correction (x10^(2.18/20)) rather than a further search.
  t[38].gain = 2.665f;  // Acoustic Snare
  t[39].percussion.noise_air_hz = 1459.44f;
  t[39].gain = 4.20f;  // Hand Clap
  t[39].amp_env.attack_ms = 3.87415f;
  t[39].amp_env.decay_ms = 145.203f;
  t[39].amp_env.sustain = 0.00762793f;
  t[39].percussion.noise_cutoff_hz = 2444.29f;
  t[39].percussion.noise_decay_ms = 720.0f;
  t[39].percussion.noise_gain = 1.8044f;
  t[39].percussion.noise_q = 2.92861f;
  // Electric Snare: one decaying pitched click under a broad noise band, which
  // is what a drum machine has instead of a drum and is the one place a single
  // mode is the structure rather than a fit that lost the others. Its numbers
  // are left where they were measured; what changes is the strainer, whose gate
  // never opened - the threshold read 1.25 against a head that swings a tenth
  // of that - so the wires below are the first rattle this piece has had.
  t[40].percussion.noise_air_hz = 304.85f;
  t[40].amp_env = fallback_env(0.277725f, 282.62f, 0.630063f, 60.0f);
  t[40].cutoff_hz = 3816.06f;
  t[40].drive = 0.230105f;
  t[40].percussion.contact = 0.23055f;
  t[40].percussion.mode_ratios[0] = 0.996289f;
  t[40].percussion.noise_gain = 2.98249f;
  t[40].percussion.noise_q = 0.731594f;
  t[40].percussion.num_modes = 1;
  t[40].percussion.pitch_drop = 0.0772591f;
  t[40].percussion.pitch_drop_ms = 15.4921f;
  t[40].percussion.plate_gain = 0.44394f;
  // Left at the engine's 2 s default while every other decay here was set
  // short, so the plate never lost 20 dB inside a window the module fills in
  // 78.3 ms; read off that recording.
  t[40].percussion.plate_t60_s = 0.0783f;
  t[40].percussion.strike_r = 0.898037f;
  t[40].percussion.strike_theta = 0.329214f;
  t[40].percussion.tone_gain = 0.631376f;
  t[40].percussion.base_freq_hz = 220.0f;
  t[40].percussion.mode_decay_s = 0.258761f;
  t[40].percussion.noise_decay_ms = 99.8719f;
  t[40].percussion.noise_cutoff_hz = 5105.77f;
  t[40].percussion.shell_mix = 0.420333f;
  // Tighter and shorter-lived than the acoustic snare's wires, and voiced
  // inside the reference's own bandwidth rather than above it: the fit had put
  // the corner at 5661 Hz against a capture that ends at 5 kHz, which is why
  // nothing it did to the rattle could be measured.
  t[40].percussion.wire_buzz = 1.0f;
  t[40].percussion.wire_threshold = 0.1f;
  t[40].percussion.wire_cutoff_hz = 1800.0f;
  t[40].percussion.wire_decay_ms = 35.0f;
  t[40].resonance_q = 0.9922f;
  t[40].stereo_spread = 0.81941f;
  t[40].gain = 7.7635f;  // Electric Snare

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
  t[74].gain = 0.2773f;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 1.7655f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);     // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  // The module falls at 289 to 553 dB/s in half the octaves it resolves, a
  // rate one envelope stage cannot reach without flattening the strike; 95 ms
  // is the closest it comes while crest and ring hold.
  t[78].amp_env.decay_ms = 95.0f;
  t[78].amp_env.attack_ms = 45.4f;
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
  t[78].gain = 1.250f;
  t[79].percussion.exclusive_class = 2;
  // The module reads a 30.43 ms attack against the fitted near-zero rise.
  t[79].amp_env.attack_ms = 30.43f;
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
  t[79].gain = 0.804f;

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
  // Measured 10.9 dB quiet against the module; `level` is exact in `gain`.
  t[54].gain = 0.7954f;
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
