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
  // toms, GM pins one head frequency per key. A non-zero `mallet_ms` reads each
  // mode's share of the strike off the contact force's spectrum, with the toms'
  // velocity exponent; 0 keeps the mode-number law.
  auto make_membrane = [&](float base_hz, float decay_s, float drop, float shell_hz, float gain,
                           float mallet_ms = 0.0f) {
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
    if (mallet_ms > 0.0f) {
      p.percussion.mallet_ms = mallet_ms;
      p.percussion.mallet_vel_exp = 0.33f;
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
  // high-Q modes with a longer ring and only a trace of strike noise. A non-zero
  // `mallet_ms` takes the contact-force law with the Hertzian mallet-on-bar
  // exponent 0.2 (Chaigne & Doutaut 1997); 0 keeps the mode-number law.
  auto make_metal = [&](float base_hz, std::array<float, kMaxPercussionModes> ratios, int nmodes,
                        float decay_s, float gain, float mallet_ms = 0.0f) {
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
    if (mallet_ms > 0.0f) {
      p.percussion.mallet_ms = mallet_ms;
      p.percussion.mallet_vel_exp = 0.2f;
    }
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
  t[35].amp_env.attack_ms = 31.8018f;
  t[35].amp_env.decay_ms = 218.299f;
  t[35].amp_env.sustain = 0.64771f;
  t[35].cutoff_hz = 12038.3f;
  t[35].drive = 0.20266f;
  t[35].percussion.contact = 0.698173f;
  t[35].percussion.mode_decay_s = 0.0391114f;
  t[35].percussion.mode_ratios[0] = 0.857872f;
  t[35].percussion.mode_ratios[1] = 4.62845f;
  t[35].percussion.noise_cutoff_hz = 485.682f;
  // The beater thud lost energy faster than the octaves the module resolves a
  // rate in: -218 dB/s measured against -140 modelled, so it hangs on longer.
  t[35].percussion.noise_decay_ms = 46.3802f;
  t[35].percussion.noise_gain = 1.42316f;
  t[35].percussion.noise_q = 0.789256f;
  t[35].percussion.pitch_drop = 2.55066f;
  t[35].percussion.pitch_drop_ms = 7.83614f;
  t[35].percussion.shell_mix = 0.972464f;
  t[35].percussion.shell_t60_s[0] = 0.369462f;
  t[35].percussion.shell_weight[0] = 3.89137f;
  t[35].percussion.strike_r = 0.369391f;
  t[35].percussion.strike_theta = 0.623657f;
  t[35].percussion.tone_gain = 1.71565f;
  t[35].stereo_spread = 0.187262f;
  t[35].gain = 2.7519f;
  // Not a strainer: a high-passed noise layer the fit reached for on a piece
  // with no wires, and it carries 8 dB of this kick's level. Converted to the
  // fractional threshold rather than deleted, because deleting it is a
  // re-voicing and the piece was heard as it stands.
  t[35].percussion.wire_buzz = 2.64919f;
  t[35].percussion.wire_threshold = 0.25f;
  // Felt beater: the contact law took the reference's velocity range from 3.8 dB
  // off to 2.5 on velocities no fit saw, with the kit's gates unchanged.
  t[35].percussion.mallet_ms = 6.0f;
  t[35].percussion.mallet_vel_exp = 0.317f;
  t[35].percussion.contact_ms = 0.318985f;
  t[35].percussion.mode_decay_exp = 0.265576f;
  t[35].percussion.mode_ratios[2] = 2.32245f;
  t[35].percussion.num_modes = 3;
  t[35].percussion.wire_cutoff_hz = 5063.42f;
  t[35].resonance_q = 1.31639f;
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
  t[41].amp_env.attack_ms = 0.117931f;
  t[41].amp_env.sustain = 0.537297f;
  t[41].cutoff_hz = 14822.1f;
  t[41].percussion.air_spring = 1.76601f;
  t[41].percussion.contact = 0.610986f;
  t[41].percussion.contact_ms = 0.203711f;
  t[41].percussion.mallet_vel_exp = 0.612343f;
  t[41].percussion.mode_decay_exp = 0.159512f;
  t[41].percussion.mode_ratios[1] = 1.24657f;
  t[41].percussion.mode_ratios[2] = 2.4144f;
  t[41].percussion.mode_ratios[3] = 2.20344f;
  t[41].percussion.mode_ratios[4] = 3.75206f;
  t[41].percussion.mode_ratios[5] = 2.68815f;
  t[41].percussion.mode_ratios[6] = 3.81595f;
  t[41].percussion.mode_ratios[7] = 4.12242f;
  t[41].percussion.noise_gain = 0.61731f;
  t[41].percussion.noise_q = 1.34583f;
  t[41].percussion.pitch_drop = 0.47489f;
  t[41].percussion.shell_mix = 0.193529f;
  t[41].percussion.shell_t60_s[0] = 0.228948f;
  t[41].percussion.shell_t60_s[1] = 0.0582867f;
  t[41].percussion.shell_weight[0] = 1.32295f;
  t[41].percussion.shell_weight[1] = 1.70147f;
  t[41].percussion.strike_theta = 0.0250092f;
  t[41].resonance_q = 2.03979f;
  t[43] = tom_patch(kToms[1]);
  t[43].amp_env.attack_ms = 0.435895f;
  t[43].amp_env.sustain = 0.710042f;
  t[43].cutoff_hz = 19477.3f;
  t[43].drive = 0.0434405f;
  t[43].percussion.contact = 1.33257f;
  t[43].percussion.contact_ms = 0.177273f;
  t[43].percussion.mallet_ms = 1.50856f;
  t[43].percussion.mallet_vel_exp = 0.817258f;
  t[43].percussion.mode_decay_exp = 0.0935292f;
  t[43].percussion.mode_ratios[1] = 1.87661f;
  t[43].percussion.mode_ratios[2] = 2.09232f;
  t[43].percussion.mode_ratios[3] = 2.13565f;
  t[43].percussion.mode_ratios[4] = 3.17122f;
  t[43].percussion.mode_ratios[6] = 3.87163f;
  t[43].percussion.mode_ratios[7] = 3.87707f;
  t[43].percussion.noise_cutoff_hz = 1508.26f;
  t[43].percussion.noise_gain = 0.616137f;
  t[43].percussion.noise_q = 1.64951f;
  t[43].percussion.pitch_drop = 0.623084f;
  t[43].percussion.pitch_drop_ms = 47.0242f;
  t[43].percussion.shell_mix = 0.407506f;
  t[43].percussion.shell_t60_s[0] = 0.163089f;
  t[43].percussion.shell_t60_s[1] = 0.427032f;
  t[43].percussion.shell_weight[0] = 2.2918f;
  t[43].percussion.shell_weight[1] = 2.56622f;
  t[43].resonance_q = 1.51901f;
  t[45] = tom_patch(kToms[2]);
  t[45].amp_env.attack_ms = 0.423447f;
  t[45].amp_env.sustain = 0.592008f;
  t[45].cutoff_hz = 17235.4f;
  t[45].percussion.contact = 0.911993f;
  t[45].percussion.contact_ms = 0.139192f;
  t[45].percussion.mallet_ms = 1.63513f;
  t[45].percussion.mallet_vel_exp = 0.685292f;
  t[45].percussion.mode_decay_exp = 0.159512f;
  t[45].percussion.mode_ratios[1] = 1.46818f;
  t[45].percussion.mode_ratios[2] = 1.85279f;
  t[45].percussion.mode_ratios[4] = 4.1021f;
  t[45].percussion.mode_ratios[6] = 2.62493f;
  t[45].percussion.mode_ratios[7] = 4.20761f;
  t[45].percussion.noise_gain = 0.61731f;
  t[45].percussion.noise_q = 1.49772f;
  t[45].percussion.pitch_drop = 0.67638f;
  t[45].percussion.shell_mix = 0.231277f;
  t[45].percussion.shell_t60_s[0] = 0.0734498f;
  t[45].percussion.shell_t60_s[1] = 0.22253f;
  t[45].percussion.shell_weight[0] = 2.09017f;
  t[45].percussion.shell_weight[1] = 2.7572f;
  t[45].resonance_q = 1.89887f;
  t[47] = tom_patch(kToms[3]);
  t[47].amp_env.attack_ms = 0.691815f;
  t[47].amp_env.sustain = 0.651025f;
  t[47].cutoff_hz = 16094.5f;
  t[47].drive = 0.132788f;
  t[47].percussion.air_spring = 1.10895f;
  t[47].percussion.contact = 1.73405f;
  t[47].percussion.contact_ms = 0.235609f;
  t[47].percussion.mallet_ms = 2.14783f;
  t[47].percussion.mallet_vel_exp = 0.667055f;
  t[47].percussion.mode_decay_exp = 0.338011f;
  t[47].percussion.mode_ratios[1] = 1.35284f;
  t[47].percussion.mode_ratios[2] = 2.94138f;
  t[47].percussion.mode_ratios[3] = 1.94897f;
  t[47].percussion.mode_ratios[4] = 2.39519f;
  t[47].percussion.mode_ratios[6] = 2.39275f;
  t[47].percussion.mode_ratios[7] = 2.3785f;
  t[47].percussion.noise_gain = 0.61731f;
  t[47].percussion.noise_q = 2.18205f;
  t[47].percussion.pitch_drop_ms = 36.0635f;
  t[47].percussion.shell_mix = 0.371031f;
  t[47].percussion.shell_t60_s[0] = 0.362126f;
  t[47].percussion.shell_t60_s[1] = 0.328785f;
  t[47].percussion.shell_weight[0] = 2.2918f;
  t[47].percussion.shell_weight[1] = 0.347845f;
  t[47].percussion.strike_theta = 0.221314f;
  t[47].resonance_q = 0.801075f;
  t[48] = tom_patch(kToms[4]);
  t[48].amp_env.attack_ms = 0.115536f;
  t[48].amp_env.sustain = 0.710042f;
  t[48].cutoff_hz = 10887.8f;
  t[48].drive = 0.140576f;
  t[48].percussion.air_spring = 0.604494f;
  t[48].percussion.contact = 1.33257f;
  t[48].percussion.contact_ms = 0.203711f;
  t[48].percussion.mallet_vel_exp = 0.846767f;
  t[48].percussion.mode_decay_exp = 0.052781f;
  t[48].percussion.mode_decay_s = 0.733665f;
  t[48].percussion.mode_ratios[1] = 1.72919f;
  t[48].percussion.mode_ratios[2] = 2.17969f;
  t[48].percussion.mode_ratios[3] = 2.70353f;
  t[48].percussion.mode_ratios[4] = 3.06151f;
  t[48].percussion.mode_ratios[5] = 3.13562f;
  t[48].percussion.mode_ratios[6] = 2.44777f;
  t[48].percussion.mode_ratios[7] = 4.12242f;
  t[48].percussion.noise_cutoff_hz = 1254.67f;
  t[48].percussion.noise_gain = 0.61731f;
  t[48].percussion.noise_q = 1.51037f;
  t[48].percussion.shell_mix = 0.257303f;
  t[48].percussion.shell_t60_s[0] = 0.181112f;
  t[48].percussion.shell_t60_s[1] = 0.297308f;
  t[48].percussion.shell_weight[0] = 1.32295f;
  t[48].percussion.shell_weight[1] = 2.20147f;
  t[48].percussion.strike_theta = 0.248161f;
  t[48].resonance_q = 0.658159f;
  t[48].stereo_spread = 0.190983f;
  t[50] = tom_patch(kToms[5]);
  t[50].amp_env.attack_ms = 2.48205f;
  t[50].amp_env.sustain = 0.863729f;
  t[50].cutoff_hz = 7365.58f;
  t[50].drive = 0.118034f;
  t[50].percussion.air_spring = 0.498494f;
  t[50].percussion.contact = 1.73405f;
  t[50].percussion.contact_ms = 0.177273f;
  t[50].percussion.mallet_ms = 1.56558f;
  t[50].percussion.mallet_vel_exp = 0.117297f;
  t[50].percussion.mode_decay_exp = 0.159512f;
  t[50].percussion.mode_ratios[1] = 1.76492f;
  t[50].percussion.mode_ratios[2] = 2.00846f;
  t[50].percussion.mode_ratios[3] = 2.70353f;
  t[50].percussion.mode_ratios[4] = 2.35806f;
  t[50].percussion.mode_ratios[5] = 2.68815f;
  t[50].percussion.mode_ratios[6] = 2.80455f;
  t[50].percussion.mode_ratios[7] = 3.35988f;
  t[50].percussion.noise_decay_ms = 28.2145f;
  t[50].percussion.noise_q = 1.76191f;
  t[50].percussion.pitch_drop = 0.498674f;
  t[50].percussion.pitch_drop_ms = 15.302f;
  t[50].percussion.shell_mix = 0.341523f;
  t[50].percussion.shell_t60_s[0] = 0.0525006f;
  t[50].percussion.shell_t60_s[1] = 0.0718808f;
  t[50].percussion.shell_weight[0] = 1.11803f;
  t[50].percussion.shell_weight[1] = 0.861648f;
  t[50].percussion.strike_theta = 0.145704f;
  t[50].resonance_q = 0.745735f;
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
  t[49].amp_env.attack_ms = 0.115536f;
  t[49].amp_env.sustain = 0.60328f;
  t[49].cutoff_hz = 20206.8f;
  t[49].percussion.contact_ms = 0.899146f;
  t[49].percussion.mode_decay_exp = 0.538197f;
  t[49].percussion.mode_decay_s = 0.67329f;
  t[49].percussion.mode_ratios[1] = 1.23473f;
  t[49].percussion.mode_ratios[2] = 1.65106f;
  t[49].percussion.mode_ratios[3] = 1.9811f;
  t[49].percussion.noise_cutoff_hz = 3587.45f;
  t[49].percussion.noise_gain = 2.2154f;
  t[49].percussion.noise_q = 1.10642f;
  t[49].percussion.plate_gain = 0.918034f;
  t[49].percussion.plate_hf_ratio = 0.373081f;
  t[49].percussion.plate_low_hz = 180.863f;
  t[49].percussion.shimmer_attack_ms = 17.707f;
  t[49].percussion.shimmer_cutoff_hz = 3850.09f;
  t[49].percussion.tone_direct = 0.588525f;
  t[49].percussion.tone_gain = 0.171885f;
  t[49].percussion.wire_buzz = 0.34017f;
  t[49].percussion.wire_cutoff_hz = 3311.22f;
  t[49].resonance_q = 1.10288f;
  t[49].stereo_spread = 0.337703f;
  t[57] = cymbal_patch(kCymbals[1]);
  // The stick rings the plate and its low band hands energy up until it is 10 dB
  // down: strike-to-body tilt change -20.9 -> -5.5 dB against the module, band shape
  // 10.9 -> 9.4 dB, crest +5.9 -> +3.6 dB.
  t[57].percussion.plate_contact = 5.0f;
  t[57].percussion.contact_ms = 1.0f;
  t[57].percussion.plate_cascade = 150.0f;
  t[57].percussion.plate_cascade_hz = 1500.0f;
  t[57].percussion.plate_cascade_drop_db = 10.0f;
  t[57].percussion.plate_t60_s = 3.4f;
  t[57].gain = 0.2032f;
  t[51] = cymbal_patch(kCymbals[2]);
  t[51].amp_env.attack_ms = 0.312637f;
  t[51].amp_env.sustain = 0.929712f;
  t[51].cutoff_hz = 8628.02f;
  t[51].drive = 0.242212f;
  t[51].percussion.contact = 14.6585f;
  t[51].percussion.contact_ms = 0.282145f;
  t[51].percussion.mode_decay_exp = 0.144098f;
  t[51].percussion.mode_decay_s = 3.82175f;
  t[51].percussion.mode_ratios[1] = 1.19947f;
  t[51].percussion.mode_ratios[2] = 2.53402f;
  t[51].percussion.mode_ratios[3] = 1.9811f;
  t[51].percussion.noise_cutoff_hz = 6006.4f;
  t[51].percussion.noise_decay_ms = 33.6489f;
  t[51].percussion.noise_gain = 0.584442f;
  t[51].percussion.noise_q = 2.26114f;
  t[51].percussion.plate_gain = 0.613777f;
  t[51].percussion.plate_hf_ratio = 0.941989f;
  t[51].percussion.plate_t60_s = 1.53383f;
  t[51].percussion.shimmer = 0.220738f;
  t[51].percussion.shimmer_attack_ms = 103.874f;
  t[51].percussion.shimmer_cutoff_hz = 4879.66f;
  t[51].percussion.tone_direct = 0.922746f;
  t[51].resonance_q = 0.658159f;
  t[51].stereo_spread = 0.154508f;
  t[59] = cymbal_patch(kCymbals[3]);
  t[53] = cymbal_patch(kCymbals[4]);
  t[52] = cymbal_patch(kCymbals[5]);
  t[55] = cymbal_patch(kCymbals[6]);
  // The module's 500 Hz strike lasts under 5 ms, so it is the stick's click rather
  // than the plate: strike-to-body tilt change -11.3 -> -0.4 dB.
  t[55].percussion.contact_ms = 0.979331f;
  t[55].percussion.contact = 0.33365f;
  t[55].cutoff_hz = 16640.6f;
  t[55].percussion.mode_ratios[3] = 2.53222f;
  t[55].percussion.noise_q = 0.816884f;
  t[55].percussion.plate_gain = 0.649187f;
  t[55].percussion.plate_hf_ratio = 0.791829f;
  t[46] = cymbal_patch(kCymbals[7]);
  t[42] = cymbal_patch(kCymbals[8]);
  t[44] = cymbal_patch(kCymbals[9]);
  // The three hats mute each other: a stick on the open pair stops the closed
  // one, and the foot stops both.
  t[46].percussion.exclusive_class = 1;
  t[46].amp_env.release_ms = 40.0f;
  t[42].percussion.exclusive_class = 1;
  t[44].percussion.exclusive_class = 1;
  // The module's open pair rings at 525, 1550 and 1731 Hz within 8 dB of its loudest
  // partial, where the voice had nothing under 3.7 kHz; all three hats take 140 Hz plate
  // spacing. Open: tilt +12.3 -> +3.1 dB, band shape 12.7 -> 9.0 dB, centroid +7.7 -> +4.0%.
  t[46].percussion.noise_gain = 2.22492f;
  t[46].percussion.tone_gain = 0.25f;
  t[46].gain = 0.1683f;
  t[46].percussion.contact = 6.02484f;
  t[46].percussion.contact_ms = 0.170637f;
  t[46].percussion.plate_low_hz = 140.0f;
  t[46].percussion.plate_hf_ratio = 0.508427f;
  t[46].percussion.base_freq_hz = 525.0f;
  t[46].percussion.num_modes = 7;
  t[46].percussion.mode_ratios = {1.0f, 2.952f, 3.297f, 7.18f, 9.62f, 12.35f, 15.44f};
  t[46].amp_env.attack_ms = 1.84809f;
  t[46].percussion.mode_decay_exp = 0.096944f;
  t[46].percussion.mode_decay_s = 0.404728f;
  t[46].percussion.mode_ratios[2] = 3.88314f;
  t[46].percussion.mode_ratios[3] = 7.56983f;
  t[46].percussion.mode_ratios[4] = 6.96423f;
  t[46].percussion.mode_ratios[5] = 7.44146f;
  t[46].percussion.mode_ratios[6] = 7.96224f;
  t[46].percussion.noise_cutoff_hz = 9038.17f;
  t[46].percussion.noise_decay_ms = 130.654f;
  t[46].percussion.noise_q = 0.816884f;
  t[46].percussion.plate_t60_s = 1.87883f;
  t[46].percussion.shimmer = 1.49858f;
  t[46].percussion.shimmer_attack_ms = 19.9507f;
  t[46].percussion.shimmer_cutoff_hz = 10666.1f;
  t[46].percussion.tone_direct = 0.632788f;
  t[46].resonance_q = 1.06454f;
  t[46].stereo_spread = 0.503483f;
  // Closed: partials at 390, 436, 826 and 1500 Hz, the stick driving the plate, and
  // the wash on the module's 12.5 kHz peak band: tilt +19.8 -> -3.7 dB, band shape
  // 15.4 -> 6.8 dB, centroid -14.5 -> -2.0%, tonality -6.2 -> -2.3 dB.
  t[42].percussion.noise_gain = 2.25149f;
  t[42].percussion.tone_gain = 0.145077f;
  t[42].gain = 0.2701f;
  t[42].percussion.contact = 0.216282f;
  t[42].percussion.contact_ms = 1.50399f;
  t[42].percussion.plate_contact = 1.4191f;
  t[42].percussion.mode_decay_s = 0.258368f;
  t[42].percussion.noise_air_hz = 12207.3f;
  t[42].percussion.noise_cutoff_hz = 10270.8f;
  t[42].percussion.noise_decay_ms = 26.0493f;
  t[42].percussion.noise_q = 1.61597f;
  t[42].percussion.plate_air_hz = 14808.3f;
  t[42].percussion.plate_gain = 0.623974f;
  t[42].percussion.plate_hf_ratio = 0.342367f;
  t[42].percussion.plate_low_hz = 140.0f;
  t[42].percussion.plate_t60_s = 0.151031f;
  t[42].percussion.base_freq_hz = 390.0f;
  t[42].percussion.num_modes = 6;
  t[42].percussion.mode_ratios = {1.0f, 1.118f, 3.846f, 2.118f, 9.603f, 12.87f, 16.51f, 20.65f};
  t[42].amp_env.attack_ms = 1.86077f;
  t[42].cutoff_hz = 13229.8f;
  t[42].drive = 0.0434405f;
  t[42].percussion.mode_decay_exp = 0.435739f;
  t[42].percussion.mode_ratios[2] = 4.52974f;
  t[42].percussion.mode_ratios[3] = 3.18123f;
  t[42].percussion.mode_ratios[4] = 7.61936f;
  t[42].percussion.shimmer = 0.699513f;
  t[42].percussion.shimmer_attack_ms = 26.3402f;
  t[42].percussion.shimmer_cutoff_hz = 8821.37f;
  t[42].percussion.tone_direct = 0.420085f;
  t[42].percussion.wire_buzz = 1.2918f;
  t[42].percussion.wire_cutoff_hz = 2365.81f;
  t[42].resonance_q = 2.09215f;
  t[42].stereo_spread = 0.197949f;
  // Pedal: the two plates meeting hold partials from 248 to 1353 Hz, and the rims'
  // longer contact drives the plate: tilt +23.4 -> -2.9 dB, band shape 16.8 -> 9.4 dB.
  t[44].percussion.noise_gain = 2.75016f;
  t[44].percussion.tone_gain = 0.19306f;
  t[44].gain = 0.1167f;
  t[44].percussion.contact = 1.14963f;
  t[44].percussion.contact_ms = 0.823679f;
  t[44].percussion.plate_contact = 0.676952f;
  t[44].percussion.noise_cutoff_hz = 12500.0f;
  t[44].percussion.plate_low_hz = 140.0f;
  t[44].percussion.plate_hf_ratio = 0.242527f;
  t[44].percussion.base_freq_hz = 380.0f;
  t[44].percussion.num_modes = 10;
  t[44].percussion.mode_ratios = {1.0f,   0.821f, 0.653f, 1.368f, 2.418f,
                                  3.561f, 9.916f, 13.29f, 17.06f, 21.33f};
  t[44].amp_env.attack_ms = 0.119244f;
  t[44].amp_env.sustain = 0.904508f;
  t[44].cutoff_hz = 16619.2f;
  t[44].drive = 0.069466f;
  t[44].percussion.mode_decay_exp = 0.978851f;
  t[44].percussion.mode_decay_s = 0.152604f;
  t[44].percussion.mode_ratios[1] = 0.56943f;
  t[44].percussion.mode_ratios[4] = 2.46515f;
  t[44].percussion.mode_ratios[5] = 2.91889f;
  t[44].percussion.mode_ratios[6] = 7.59864f;
  t[44].percussion.mode_ratios[7] = 7.78896f;
  t[44].percussion.mode_ratios[8] = 5.6248f;
  t[44].percussion.mode_ratios[9] = 5.6248f;
  t[44].percussion.noise_q = 0.849056f;
  t[44].percussion.plate_gain = 0.687539f;
  t[44].percussion.plate_t60_s = 0.571819f;
  t[44].percussion.shimmer = 1.49858f;
  t[44].percussion.shimmer_attack_ms = 11.6031f;
  t[44].percussion.shimmer_cutoff_hz = 14897.5f;
  t[44].percussion.tone_direct = 0.895898f;
  t[44].resonance_q = 0.694219f;
  t[44].stereo_spread = 0.0477458f;
  // The china's wash is a band rather than a corner, and a narrow one: that is
  // what trashy is, and it is a different filter rather than a different
  // corner. Its partials are pulled off the plate ratios the others share until
  // nothing in the sound reads as a pitch.
  t[52].percussion.noise_output = SynthFilterOutput::kBandpass;
  t[52].percussion.noise_q = 3.572f;
  t[52].percussion.mode_ratios = {1.0f, 1.19f, 1.51f, 1.83f, 0.0f, 0.0f};
  // Fitted against the module's china: a denser, longer plate carrying more of
  // the tail than the strike layer took decay -1.97 -> -1.18 dB/s, ring +0.18 ->
  // +0.03 doublings and crest +3.2 -> +0.4 dB.
  t[52].percussion.mode_decay_s = 0.44853f;
  t[52].percussion.noise_decay_ms = 141.112f;
  t[52].percussion.plate_gain = 1.13524f;
  t[52].percussion.plate_hf_ratio = 0.522551f;
  t[52].percussion.plate_t60_s = 2.04106f;
  t[52].percussion.shimmer = 1.7534f;
  t[52].amp_env.attack_ms = 0.978638f;
  t[52].cutoff_hz = 8296.24f;
  t[52].percussion.mode_decay_exp = 0.340248f;
  t[52].percussion.mode_ratios[1] = 1.40156f;
  t[52].percussion.mode_ratios[2] = 2.27899f;
  t[52].percussion.mode_ratios[3] = 1.55377f;
  t[52].percussion.noise_gain = 0.725735f;
  t[52].percussion.shimmer_attack_ms = 56.429f;
  t[52].percussion.shimmer_cutoff_hz = 9057.56f;
  t[52].percussion.tone_direct = 0.985246f;
  t[52].percussion.tone_gain = 0.972291f;
  t[52].resonance_q = 0.893437f;
  // The bell is the one piece carried by its modes, so its ring is narrow
  // rather than dense and its wash is only the stick.
  t[53].percussion.mode_ratios = {1.0f, 1.40f, 2.76f, 3.27f, 0.0f, 0.0f};
  t[53].percussion.num_modes = 4;
  t[53].percussion.noise_gain = 0.324671f;
  t[53].amp_env.attack_ms = 0.565742f;
  t[53].cutoff_hz = 11891.6f;
  t[53].drive = 0.118034f;
  t[53].percussion.contact = 10.523f;
  t[53].percussion.contact_ms = 0.14366f;
  t[53].percussion.mode_decay_exp = 0.358939f;
  t[53].percussion.mode_decay_s = 1.35882f;
  t[53].percussion.mode_ratios[1] = 1.64889f;
  t[53].percussion.noise_cutoff_hz = 913.57f;
  t[53].percussion.noise_q = 0.579566f;
  t[53].percussion.plate_gain = 0.308204f;
  t[53].percussion.plate_hf_ratio = 0.975711f;
  t[53].percussion.tone_direct = 0.779508f;
  t[53].resonance_q = 0.837311f;
  t[53].stereo_spread = 0.473975f;

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
  t[37].amp_env.attack_ms = 4.4977f;
  t[37].amp_env.decay_ms = 106.475f;
  t[37].amp_env.sustain = 0.691805f;
  t[37].percussion.mode_decay_s = 0.005641f;
  t[37].percussion.noise_cutoff_hz = 495.595f;
  t[37].percussion.noise_decay_ms = 19.5866f;
  t[37].percussion.noise_gain = 1.1431f;
  t[37].percussion.noise_q = 0.545305f;
  t[37].percussion.strike_r = 0.702125f;
  t[37].gain = 4.0f;
  t[37].cutoff_hz = 18919.5f;
  t[37].percussion.tone_gain = 0.111877f;
  t[37].percussion.wire_buzz = 1.40983f;
  t[37].percussion.wire_cutoff_hz = 2554.95f;
  t[37].percussion.wire_threshold = 0.0490133f;
  t[37].resonance_q = 0.982264f;
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
  t[76].amp_env.attack_ms = 0.165453f;
  t[76].amp_env.decay_ms = 308.461f;
  t[76].amp_env.sustain = 0.0442745f;
  t[76].cutoff_hz = 3129.44f;
  t[76].drive = 0.38476f;
  t[76].percussion.contact = 0.124193f;
  t[76].percussion.mode_decay_s = 0.0514219f;
  t[76].percussion.mode_ratios[0] = 1.39607f;
  t[76].percussion.noise_cutoff_hz = 188.807f;
  t[76].percussion.noise_decay_ms = 89.1424f;
  t[76].percussion.noise_gain = 1.65706f;
  t[76].percussion.noise_q = 2.93393f;
  t[76].percussion.num_modes = 4;
  t[76].percussion.plate_gain = 1.36317f;
  t[76].percussion.strike_r = 0.351027f;
  t[76].percussion.tone_gain = 2.67532f;
  t[76].resonance_q = 2.80532f;
  t[76].stereo_spread = 0.899155f;
  t[76].percussion.contact_ms = 3.04281f;
  t[76].percussion.plate_hf_ratio = 0.63617f;
  t[76].percussion.plate_low_hz = 306.297f;
  t[76].percussion.plate_t60_s = 0.218749f;
  t[76].percussion.tone_direct = 0.750868f;
  t[76].gain = 0.4939f;
  // A fit with the contact law free beat one holding the mode-number law on the
  // reference's velocity range it never trained on (3.0 dB off against 3.4) and on
  // the kit's gates (one bound over, 1.77 against two, 1.83).
  t[76].percussion.mallet_ms = 0.2972f;
  t[76].percussion.mallet_vel_exp = 0.6667f;
  t[76].percussion.mode_decay_exp = 0.236068f;
  t[77] = make_wood(800.0f, 0.0f, 0.07f, 0.6f);  // Low Wood Block
  t[77].amp_env.attack_ms = 0.467705f;
  t[77].amp_env.decay_ms = 16.8591f;
  t[77].amp_env.sustain = 0.16418f;
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
  t[56].amp_env.attack_ms = 0.354235f;
  t[56].amp_env.decay_ms = 78.6096f;
  t[56].cutoff_hz = 11697.7f;
  t[56].drive = 0.118034f;
  t[56].percussion.mode_decay_exp = 0.506966f;
  t[56].percussion.mode_decay_s = 0.162704f;
  t[56].percussion.noise_cutoff_hz = 2659.96f;
  t[56].percussion.noise_decay_ms = 21.2081f;
  t[56].percussion.noise_gain = 2.12852f;
  t[56].percussion.noise_q = 1.15262f;
  t[56].percussion.strike_r = 0.52172f;
  t[56].percussion.strike_theta = 0.348975f;
  t[56].percussion.tone_gain = 0.836881f;
  t[56].resonance_q = 0.555911f;
  t[56].stereo_spread = 0.118034f;
  // The contact law took the reference's velocity range from 1.0 dB off to 0.1 on
  // velocities no fit saw, with the kit's gates unchanged.
  t[67] = make_metal(1200.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.25f, 0.45f,
                     0.4f);  // High Agogo
  t[67].percussion.mallet_vel_exp = 0.2566f;
  t[67].gain = 0.6823f;
  t[67].amp_env.attack_ms = 1.98631f;
  t[67].amp_env.decay_ms = 2206.33f;
  t[67].amp_env.sustain = 0.289958f;
  t[67].cutoff_hz = 13502.7f;
  t[67].percussion.mode_decay_exp = 0.437306f;
  t[67].percussion.mode_decay_s = 0.183948f;
  t[67].percussion.mode_ratios[1] = 1.87267f;
  t[67].percussion.noise_cutoff_hz = 6570.03f;
  t[67].percussion.noise_decay_ms = 27.1079f;
  t[67].percussion.noise_gain = 0.152786f;
  t[67].percussion.noise_q = 2.33292f;
  t[67].percussion.strike_r = 0.253483f;
  t[67].resonance_q = 0.719516f;
  t[67].stereo_spread = 0.113729f;
  t[68] = make_metal(900.0f, {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f}, 2, 0.30f, 0.45f);  // Low Agogo
  t[68].amp_env.attack_ms = 0.625483f;
  t[68].amp_env.decay_ms = 1036.07f;
  t[68].amp_env.sustain = 0.78805f;
  t[68].cutoff_hz = 4136.67f;
  t[68].drive = 0.430173f;
  t[68].percussion.contact = 3.62282f;
  t[68].percussion.mode_decay_s = 5.92148f;
  t[68].percussion.mode_ratios[0] = 2.07048f;
  t[68].percussion.mode_ratios[1] = 16.7402f;
  t[68].percussion.noise_cutoff_hz = 3977.62f;
  t[68].percussion.noise_decay_ms = 23.8539f;
  t[68].percussion.noise_gain = 0.940689f;
  t[68].percussion.noise_q = 4.88898f;
  t[68].percussion.num_modes = 1;
  t[68].percussion.plate_gain = 0.527484f;
  t[68].percussion.strike_r = 0.435546f;
  t[68].percussion.tone_gain = 1.57692f;
  t[68].stereo_spread = 0.515549f;
  t[68].percussion.plate_hf_ratio = 0.592924f;
  t[68].percussion.plate_low_hz = 774.578f;
  t[68].percussion.plate_t60_s = 0.130148f;
  t[68].percussion.tone_direct = 0.743953f;
  t[68].resonance_q = 19.0001f;
  t[68].gain = 0.2071f;
  t[68].percussion.contact_ms = 0.234707f;
  t[68].percussion.mode_decay_exp = 0.206559f;
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
  t[80].amp_env.attack_ms = 0.220738f;
  t[80].amp_env.decay_ms = 134.542f;
  t[80].amp_env.sustain = 0.308195f;
  t[80].drive = 0.119873f;
  t[80].percussion.mode_ratios[1] = 2.04158f;
  t[80].percussion.mode_ratios[2] = 0.959918f;
  t[80].percussion.mode_ratios[3] = 0.453051f;
  t[80].percussion.noise_cutoff_hz = 8500.62f;
  t[80].percussion.noise_decay_ms = 13.0702f;
  t[80].percussion.noise_gain = 0.154102f;
  t[80].percussion.noise_q = 0.905879f;
  t[80].resonance_q = 0.732763f;
  t[80].stereo_spread = 0.235246f;
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
  t[60].amp_env.attack_ms = 3.09012f;
  t[60].amp_env.decay_ms = 34.713f;
  t[60].amp_env.sustain = 0.307505f;
  t[60].cutoff_hz = 1721.43f;
  t[60].drive = 0.314691f;
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
  t[61].amp_env.attack_ms = 3.80371f;
  t[61].amp_env.decay_ms = 18.6134f;
  t[61].amp_env.sustain = 0.297213f;
  t[61].cutoff_hz = 13643.0f;
  t[61].drive = 0.167789f;
  t[61].percussion.contact = 1.42293f;
  t[61].percussion.mode_decay_s = 0.0541491f;
  t[61].percussion.mode_ratios[0] = 0.613928f;
  t[61].percussion.mode_ratios[1] = 4.10079f;
  t[61].percussion.mode_ratios[2] = 1.65349f;
  t[61].percussion.mode_ratios[3] = 21.6188f;
  t[61].percussion.mode_ratios[4] = 0.536416f;
  t[61].percussion.noise_cutoff_hz = 29.5156f;
  t[61].percussion.noise_decay_ms = 16.6137f;
  t[61].percussion.noise_gain = 2.09272f;
  t[61].percussion.noise_q = 2.51731f;
  t[61].percussion.pitch_drop = 2.63273f;
  t[61].percussion.pitch_drop_ms = 4.12633f;
  t[61].percussion.plate_gain = 2.2338f;
  t[61].percussion.strike_r = 0.692402f;
  t[61].percussion.strike_theta = 1.34672f;
  t[61].percussion.tone_gain = 2.23551f;
  t[61].stereo_spread = 0.418339f;
  t[61].percussion.contact_ms = 0.257848f;
  t[61].percussion.num_modes = 3;
  t[61].percussion.plate_hf_ratio = 0.414808f;
  t[61].percussion.plate_low_hz = 295.289f;
  t[61].percussion.plate_t60_s = 0.306712f;
  t[61].percussion.tone_direct = 0.915486f;
  t[61].resonance_q = 1.1349f;
  t[61].gain = 1.0607f;
  t[61].percussion.mode_decay_exp = 0.185661f;
  // base_hz and the five ratios are read from the reference's measured
  // modal_hz/modal_ratio (drums_module_hit.json note 62): the old 220 Hz base
  // put every mode at or below 45 Hz, where the reference has no energy.
  t[62] = make_membrane(117.5f, 0.08f, 0.20f, 0.0f, 0.70f);  // Mute Hi Conga
  t[62].amp_env.attack_ms = 0.376547f;
  t[62].amp_env.decay_ms = 889.115f;
  t[62].cutoff_hz = 7754.29f;
  t[62].drive = 0.843336f;
  t[62].percussion.contact = 2.09208f;
  // mode_decay_exp 0.5 is the membrane-in-air value already used for note 38.
  // mode_decay_s matches the reference's own extrapolated decay rate (fitted
  // on n062_v100.wav, R^2 0.88): -60 dB at 171-174 ms against this render's
  // 195.6 ms, -100 dB at 330-338 ms against 362.2 ms.
  t[62].percussion.mode_decay_exp = 0.729102f;
  t[62].percussion.mode_decay_s = 0.155959f;
  t[62].percussion.mode_ratios[0] = 1.0f;
  t[62].percussion.mode_ratios[1] = 1.51519f;
  t[62].percussion.mode_ratios[2] = 1.60684f;
  t[62].percussion.mode_ratios[3] = 1.79254f;
  t[62].percussion.mode_ratios[4] = 7.4515f;
  t[62].percussion.noise_cutoff_hz = 785.897f;
  t[62].percussion.noise_decay_ms = 22.2714f;
  t[62].percussion.noise_gain = 1.26024f;
  t[62].percussion.noise_q = 2.75839f;
  t[62].percussion.pitch_drop = 0.462799f;
  t[62].percussion.pitch_drop_ms = 6.73865f;
  t[62].percussion.plate_gain = 0.747417f;
  t[62].percussion.strike_r = 0.518031f;
  t[62].percussion.strike_theta = 1.39523f;
  t[62].percussion.tone_gain = 1.55524f;
  t[62].stereo_spread = 0.148505f;
  t[62].amp_env.sustain = 0.280067f;
  // Lengthened from 2.07 (this is the period of a one-cycle sine burst, so
  // longer = lower-frequency): band_tilt read +9.1 dB there and now +0.3,
  // moving the tilt fix off mode_decay_s so the tail stays at the rate above.
  t[62].percussion.contact_ms = 13.0067f;
  t[62].percussion.num_modes = 5;
  t[62].percussion.plate_hf_ratio = 0.314786f;
  t[62].percussion.plate_low_hz = 459.58f;
  t[62].percussion.plate_t60_s = 0.0499273f;
  t[62].percussion.tone_direct = 0.18932f;
  t[62].resonance_q = 2.51457f;
  t[62].gain = 0.4184f;
  t[63] = make_membrane(200.0f, 0.25f, 0.30f, 0.0f, 1.1847f);  // Open Hi Conga
  t[63].amp_env.attack_ms = 1.57572f;
  t[63].amp_env.decay_ms = 109.313f;
  t[63].amp_env.sustain = 0.570564f;
  t[63].percussion.mode_decay_s = 0.166094f;
  t[63].percussion.noise_cutoff_hz = 227.798f;
  t[63].percussion.noise_decay_ms = 1.76001f;
  t[63].percussion.noise_gain = 1.26895f;
  t[63].percussion.noise_q = 14.2647f;
  t[63].percussion.pitch_drop = 0.106815f;
  t[63].percussion.pitch_drop_ms = 641.174f;
  t[63].percussion.strike_r = 0.789245f;
  t[63].percussion.strike_theta = 0.803419f;
  t[63].cutoff_hz = 1770.38f;
  t[63].drive = 0.190302f;
  t[63].percussion.contact = 1.67052f;
  t[63].percussion.mode_ratios[0] = 2.24933f;
  t[63].percussion.mode_ratios[1] = 11.1108f;
  t[63].percussion.mode_ratios[2] = 6.93756f;
  t[63].percussion.mode_ratios[3] = 3.82169f;
  t[63].percussion.mode_ratios[4] = 4.69038f;
  t[63].percussion.num_modes = 3;
  t[63].percussion.plate_gain = 0.789905f;
  t[63].percussion.tone_gain = 3.43486f;
  t[63].resonance_q = 0.61283f;
  t[63].stereo_spread = 0.475025f;
  t[63].percussion.plate_hf_ratio = 0.319414f;
  t[63].percussion.plate_low_hz = 140.0f;
  t[63].percussion.plate_t60_s = 0.395604f;
  t[63].gain = 1.1392f;
  // Not a strainer: a high-passed noise layer the fit reached for on a piece
  // with no wires, and it carries 5 dB of this conga's tonality. Converted to the
  // fractional threshold rather than deleted, because deleting it is a
  // re-voicing and the piece was heard as it stands.
  t[63].percussion.wire_buzz = 2.8541f;
  t[63].percussion.wire_threshold = 0.4f;
  t[63].percussion.contact_ms = 0.183625f;
  t[63].percussion.mode_decay_exp = 0.300407f;
  t[63].percussion.tone_direct = 0.816805f;
  t[63].percussion.wire_cutoff_hz = 5346.64f;
  // base_hz is the reference's loudest partial (222.11 Hz): with contact > 0
  // the excitation is monotonic in frequency, so the lowest placed mode is
  // always loudest, and its quieter 167.84/182.1/204.85 Hz partials (35-37 dB
  // under peak) cannot be placed without becoming the loudest. Left out.
  t[64] = make_membrane(222.11f, 0.30f, 0.35f, 0.0f, 1.4014f);  // Low Conga
  // The module reads a 6.98 ms attack against the fitted envelope's 0.1.
  t[64].amp_env.attack_ms = 21.4615f;
  t[64].amp_env.decay_ms = 70.3119f;
  // Lowered from 0.467 measuring decay_ms on drums_module_hit's 360.77 ms
  // window (module: 169.6 ms): 0.18 reads 166.1 ms against the same target.
  t[64].amp_env.sustain = 0.194754f;
  t[64].cutoff_hz = 6691.74f;
  t[64].drive = 0.571386f;
  t[64].percussion.contact = 1.27999f;
  // mode_decay_exp 0.5 is the membrane-in-air value already used for note 38.
  // mode_decay_s measured against band_tilt on drums_module_hit: 0.45 with
  // the sustain above reads +2.1 dB there (pre-re-base: +8.4).
  t[64].percussion.mode_decay_exp = 0.560661f;
  t[64].percussion.mode_decay_s = 0.508757f;
  t[64].percussion.mode_ratios[0] = 1.0f;
  t[64].percussion.mode_ratios[1] = 1.1346f;
  t[64].percussion.mode_ratios[2] = 1.3865f;
  t[64].percussion.mode_ratios[3] = 1.5115f;
  t[64].percussion.mode_ratios[4] = 2.07313f;
  t[64].percussion.noise_cutoff_hz = 400.224f;
  // This is the RC time constant `noise_level_` decays by, not a t60.
  t[64].percussion.noise_decay_ms = 45.0f;
  t[64].percussion.noise_gain = 2.44799f;
  t[64].percussion.noise_q = 8.15195f;
  // All 5 slots now carry a real measured partial rather than 2 of them going
  // unused: see the ratio comment above.
  t[64].percussion.num_modes = 5;
  t[64].percussion.pitch_drop = 4.21119f;
  t[64].percussion.pitch_drop_ms = 33.5273f;
  t[64].percussion.strike_r = 0.504196f;
  t[64].percussion.strike_theta = 0.108611f;
  t[64].percussion.tone_gain = 3.14836f;
  t[64].stereo_spread = 0.914831f;
  t[64].percussion.contact_ms = 0.129169f;
  t[64].percussion.plate_gain = 0.00894634f;
  t[64].resonance_q = 0.855344f;
  t[64].gain = 1.2860f;
  t[64].percussion.plate_hf_ratio = 0.250958f;
  t[64].percussion.plate_low_hz = 347.789f;
  t[64].percussion.plate_t60_s = 0.36498f;
  t[64].percussion.tone_direct = 0.964856f;
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 1.1120f);  // High Timbale
  t[65].amp_env.attack_ms = 0.0703342f;
  // A 2.16 s decay held the piece loud across the whole window, so the strike
  // had no fall to stand above: 22.49 dB of crest measured, 10.7 short.
  t[65].amp_env.decay_ms = 212.39f;
  t[65].amp_env.sustain = 0.233903f;
  t[65].cutoff_hz = 2871.3f;
  t[65].drive = 0.822996f;
  t[65].percussion.contact = 2.76404f;
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
  t[71].amp_env.attack_ms = 53.9539f;
  t[71].amp_env.decay_ms = 41.2269f;
  t[71].amp_env.sustain = 0.0f;
  t[71].cutoff_hz = 4069.82f;
  t[71].drive = 0.719438f;
  t[71].percussion.contact = 0.430935f;
  t[71].percussion.mode_decay_s = 0.634916f;
  t[71].percussion.mode_ratios[0] = 38.2286f;
  t[71].percussion.noise_cutoff_hz = 4917.37f;
  t[71].percussion.noise_decay_ms = 23.7007f;
  t[71].percussion.noise_gain = 1.03481f;
  t[71].percussion.noise_q = 15.4492f;
  t[71].percussion.plate_gain = 1.67889f;
  t[71].percussion.strike_r = 0.435508f;
  t[71].percussion.tone_gain = 2.31126f;
  t[71].stereo_spread = 0.739068f;
  t[71].percussion.num_modes = 0;
  t[71].percussion.plate_hf_ratio = 0.518624f;
  t[71].percussion.plate_low_hz = 692.016f;
  t[71].percussion.plate_t60_s = 0.141835f;
  t[71].resonance_q = 2.0616f;
  t[71].percussion.contact_ms = 0.215003f;
  t[71].gain = 0.6823f;
  t[72].percussion.exclusive_class = 4;
  t[72].amp_env.attack_ms = 51.0206f;
  // Zero sustain, as the acoustic snare above: a one-shot that sustains
  // plateaus rather than falls, and never loses 20 dB inside the window.
  t[72].amp_env.decay_ms = 453.894f;
  t[72].amp_env.sustain = 0.0147542f;
  t[72].cutoff_hz = 6664.31f;
  t[72].drive = 0.0275842f;
  t[72].percussion.contact = 0.0f;
  t[72].percussion.mode_decay_s = 0.254392f;
  t[72].percussion.mode_ratios[0] = 7.0373f;
  t[72].percussion.noise_cutoff_hz = 7264.88f;
  t[72].percussion.noise_decay_ms = 51.857f;
  t[72].percussion.noise_gain = 0.718909f;
  t[72].percussion.noise_q = 17.2851f;
  t[72].percussion.num_modes = 0;
  t[72].percussion.plate_gain = 0.339149f;
  t[72].percussion.strike_r = 0.0568307f;
  t[72].percussion.tone_gain = 1.92962f;
  t[72].stereo_spread = 0.84215f;
  t[72].amp_env.release_ms = 53.3108f;
  t[72].percussion.plate_hf_ratio = 0.648638f;
  t[72].percussion.plate_low_hz = 220.375f;
  t[72].percussion.plate_t60_s = 9.06089f;
  t[72].resonance_q = 2.28507f;
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
  t[38].percussion.mode_decay_exp = 0.385255f;
  t[38].percussion.tone_gain = 1.0f;
  t[38].percussion.pitch_drop = 0.25f;
  t[38].percussion.pitch_drop_ms = 20.0f;
  // Off-centre, where a snare is played.
  t[38].percussion.strike_r = 0.5f;
  t[38].percussion.strike_theta = 0.139754f;
  t[38].percussion.shell_mix = 0.770492f;
  t[38].percussion.shell_num_modes = 4;
  t[38].percussion.shell_freq_hz = {165.0f, 205.0f, 330.0f, 400.0f};
  t[38].percussion.shell_t60_s = {0.12f, 0.1f, 0.08f, 0.07f};
  t[38].percussion.shell_weight = {1.0f, 4.0f, 5.0f, 3.0f};
  // The head's upper modes and the wires together sat 9-14 dB under the module
  // across 1-8 kHz for the whole hit, which left the centroid near 300 Hz where
  // the module's runs 1.3-1.8 kHz.
  t[38].percussion.noise_gain = 1.27805f;
  t[38].percussion.noise_decay_ms = 283.312f;
  t[38].percussion.noise_cutoff_hz = 860.627f;
  t[38].percussion.noise_q = 0.69239f;
  // The stick's contact, summed outside the amp envelope. At 0.07 ms it spoke at
  // 14 kHz, 23 dB over the module there, and left the first 15 ms 11 dB darker
  // than the module's strike; 0.237 ms puts it in the 1-4 kHz the strike fills.
  t[38].percussion.contact = 6.14042f;
  t[38].percussion.contact_ms = 0.342485f;
  // The strainer: high-passed into its own band, bounded above by the radiated
  // ceiling so the rattle sits where a wire bed sits instead of running to
  // Nyquist, and ringing 60 ms past the head that started it.
  t[38].percussion.wire_buzz = 0.418f;
  t[38].percussion.wire_threshold = 0.131237f;
  t[38].percussion.wire_cutoff_hz = 1917.28f;
  t[38].percussion.wire_decay_ms = 107.948f;
  t[38].percussion.noise_air_hz = 6000.0f;
  t[38].percussion.plate_gain = 0.0f;
  t[38].percussion.tone_direct = 1.0f;
  t[38].cutoff_hz = 11709.9f;
  t[38].resonance_q = 0.777325f;
  t[38].drive = 0.118034f;
  // Zero sustain, because a one-shot voice ignores note-off and a patch that
  // sustains never frees its slot. The fit it replaces held 0.26.
  t[38].amp_env = fallback_env(0.5f, 600.0f, 0.0f, 80.0f);
  // Level against the module, corrected exactly since gain is linear in it; the
  // brighter voicing above read 0.71 dB loud at 1.3827.
  t[38].gain = 1.2742f;  // Acoustic Snare
  t[38].amp_env.attack_ms = 0.992917f;
  t[38].amp_env.decay_ms = 867.05f;
  t[38].percussion.mode_ratios[2] = 3.04825f;
  t[38].percussion.mode_ratios[3] = 3.93826f;
  t[38].percussion.mode_ratios[4] = 2.49259f;
  t[38].percussion.shell_t60_s[0] = 0.064967f;
  t[38].percussion.shell_weight[1] = 2.90983f;
  t[38].percussion.shell_weight[3] = 3.2918f;
  t[39].percussion.noise_air_hz = 1459.44f;
  t[39].gain = 4.20f;  // Hand Clap
  t[39].amp_env.attack_ms = 1.04815f;
  t[39].amp_env.decay_ms = 237.228f;
  t[39].amp_env.sustain = 0.00762793f;
  t[39].percussion.noise_cutoff_hz = 3993.41f;
  t[39].percussion.noise_decay_ms = 240.228f;
  t[39].percussion.noise_gain = 2.35125f;
  t[39].percussion.noise_q = 1.19511f;
  t[39].resonance_q = 1.64558f;
  // Electric Snare: one decaying pitched click under a broad noise band, which
  // is what a drum machine has instead of a drum and is the one place a single
  // mode is the structure rather than a fit that lost the others. Its numbers
  // are left where they were measured; what changes is the strainer, whose gate
  // never opened - the threshold read 1.25 against a head that swings a tenth
  // of that - so the wires below are the first rattle this piece has had.
  t[40].percussion.noise_air_hz = 859.303f;
  t[40].amp_env = fallback_env(0.277725f, 282.62f, 0.630063f, 60.0f);
  t[40].cutoff_hz = 14304.2f;
  t[40].drive = 0.142846f;
  t[40].percussion.contact = 0.376666f;
  t[40].percussion.mode_ratios[0] = 0.996289f;
  t[40].percussion.noise_gain = 3.89059f;
  t[40].percussion.noise_q = 2.05189f;
  t[40].percussion.num_modes = 3;
  t[40].percussion.pitch_drop = 0.0108548f;
  t[40].percussion.pitch_drop_ms = 3.92848f;
  t[40].percussion.plate_gain = 0.321183f;
  // Left at the engine's 2 s default while every other decay here was set
  // short, so the plate never lost 20 dB inside a window the module fills in
  // 78.3 ms; read off that recording.
  t[40].percussion.plate_t60_s = 0.0531682f;
  t[40].percussion.strike_r = 0.898037f;
  t[40].percussion.strike_theta = 0.314642f;
  t[40].percussion.tone_gain = 0.619237f;
  t[40].percussion.base_freq_hz = 220.0f;
  t[40].percussion.mode_decay_s = 0.330746f;
  t[40].percussion.noise_decay_ms = 88.3376f;
  t[40].percussion.noise_cutoff_hz = 13740.6f;
  t[40].percussion.shell_mix = 0.186578f;
  // Tighter and shorter-lived than the acoustic snare's wires, and voiced
  // inside the reference's own bandwidth rather than above it: the fit had put
  // the corner at 5661 Hz against a capture that ends at 5 kHz, which is why
  // nothing it did to the rattle could be measured.
  t[40].percussion.wire_buzz = 0.652476f;
  t[40].percussion.wire_threshold = 0.0826238f;
  t[40].percussion.wire_cutoff_hz = 2940.78f;
  t[40].percussion.wire_decay_ms = 23.8717f;
  t[40].resonance_q = 2.52748f;
  t[40].stereo_spread = 0.81941f;
  t[40].gain = 7.7635f;  // Electric Snare
  t[40].amp_env.attack_ms = 1.17749f;
  t[40].amp_env.decay_ms = 172.987f;
  t[40].amp_env.sustain = 0.462349f;
  t[40].percussion.contact_ms = 0.106066f;
  t[40].percussion.mode_decay_exp = 0.835237f;
  t[40].percussion.mode_ratios[1] = 1.90919f;
  t[40].percussion.mode_ratios[2] = 1.97189f;
  t[40].percussion.plate_hf_ratio = 0.891697f;
  t[40].percussion.plate_low_hz = 173.915f;
  t[40].percussion.shell_t60_s[0] = 0.0989962f;
  t[40].percussion.shell_t60_s[1] = 0.22966f;
  t[40].percussion.shell_weight[0] = 0.22949f;
  t[40].percussion.shell_weight[1] = 0.377709f;
  t[40].percussion.tone_direct = 0.816805f;

  // --- PhISEM shakers + scrapers ---
  t[54] = make_shaker(32.0f, 120.0f, 2500.0f, 2.0f, 0.2267f);  // Tambourine
  t[58] = make_shaker(24.0f, 400.0f, 2500.0f, 3.0f, 0.2912f);  // Vibraslap
  t[58].amp_env.decay_ms = 6653.04f;
  t[58].amp_env.sustain = 0.470738f;
  t[58].cutoff_hz = 11570.5f;
  t[58].drive = 0.0107823f;
  t[58].percussion.noise_gain = 0.735434f;
  t[58].percussion.phisem_beans = 1.13582f;
  t[58].percussion.phisem_energy_ms = 24.9781f;
  t[58].percussion.phisem_res_hz = 1103.15f;
  t[58].percussion.phisem_res_q = 12.0314f;
  t[58].percussion.phisem_sound_ms = 0.41229f;
  t[58].percussion.plate_gain = 2.36103f;
  t[58].stereo_spread = 0.130344f;
  t[58].percussion.contact = 5.63572f;
  t[58].percussion.noise_cutoff_hz = 4192.78f;
  t[58].percussion.noise_decay_ms = 35.4984f;
  t[58].percussion.noise_q = 0.830981f;
  t[58].percussion.plate_hf_ratio = 0.728446f;
  t[58].percussion.plate_low_hz = 366.777f;
  t[58].percussion.plate_t60_s = 2.07916f;
  t[58].resonance_q = 1.45797f;
  t[58].amp_env.attack_ms = 0.583895f;
  t[58].percussion.contact_ms = 0.234707f;
  t[69] = make_shaker(24.0f, 90.0f, 4000.0f, 1.0f, 0.3232f);  // Cabasa
  t[69].amp_env.decay_ms = 290.031f;
  t[69].amp_env.sustain = 0.00114147f;
  t[69].cutoff_hz = 14234.2f;
  t[69].drive = 0.149417f;
  t[69].percussion.contact = 0.382481f;
  t[69].percussion.phisem_beans = 6.95486f;
  t[69].percussion.phisem_energy_ms = 20.5539f;
  t[69].percussion.phisem_res_hz = 1205.27f;
  t[69].percussion.phisem_res_q = 5.81349f;
  t[69].percussion.phisem_sound_ms = 0.583329f;
  t[69].percussion.plate_gain = 0.579308f;
  t[69].resonance_q = 2.41545f;
  t[69].stereo_spread = 0.162217f;
  t[69].amp_env.attack_ms = 0.141107f;
  t[69].percussion.contact_ms = 0.163742f;
  t[69].percussion.noise_gain = 1.07508f;
  t[69].percussion.plate_hf_ratio = 0.716797f;
  t[69].percussion.plate_low_hz = 184.014f;
  t[69].percussion.plate_t60_s = 0.199929f;
  t[69].percussion.noise_cutoff_hz = 7492.88f;
  t[69].percussion.noise_decay_ms = 41.6328f;
  t[69].percussion.noise_q = 0.816884f;
  t[70] = make_shaker(20.0f, 90.0f, 3200.0f, 1.5f, 0.2173f);  // Maracas
  t[82] = make_shaker(28.0f, 110.0f, 6000.0f, 1.0f, 0.5f);    // Shaker
  // Guiro (mute group 5): ratchet ridge train.
  t[73] = make_scrape(8.0f, 120.0f, 150.0f, 2500.0f, 3.0f, 0.0f, 0.2065f);  // Short Guiro
  t[74] = make_scrape(8.0f, 500.0f, 120.0f, 2500.0f, 3.0f, 0.0f, 0.1437f);  // Long Guiro
  t[73].percussion.exclusive_class = 5;
  t[74].percussion.exclusive_class = 5;
  t[74].amp_env.decay_ms = 166.168f;
  t[74].amp_env.sustain = 0.224123f;
  t[74].cutoff_hz = 9174.35f;
  t[74].drive = 0.357386f;
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
  t[74].amp_env.attack_ms = 0.816884f;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 1.7655f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);     // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  // The module falls at 289 to 553 dB/s in half the octaves it resolves, a
  // rate one envelope stage cannot reach without flattening the strike; 95 ms
  // is the closest it comes while crest and ring hold.
  t[78].amp_env.decay_ms = 64.7947f;
  t[78].amp_env.attack_ms = 128.411f;
  t[78].cutoff_hz = 2103.76f;
  t[78].drive = 0.167311f;
  t[78].percussion.contact = 0.0638085f;
  t[78].percussion.noise_gain = 0.0793007f;
  t[78].percussion.phisem_beans = 1.30882f;
  t[78].percussion.phisem_energy_ms = 91.6977f;
  t[78].percussion.phisem_res_hz = 868.281f;
  t[78].percussion.phisem_res_q = 4.29353f;
  t[78].percussion.phisem_scrape_hz = 37.3241f;
  t[78].percussion.phisem_sound_ms = 5.3168f;
  t[78].percussion.plate_gain = 0.984924f;
  t[78].resonance_q = 1.64834f;
  t[78].stereo_spread = 0.283496f;
  t[78].gain = 1.250f;
  t[78].percussion.contact_ms = 0.62648f;
  t[78].percussion.noise_cutoff_hz = 2140.8f;
  t[78].percussion.noise_decay_ms = 1067.58f;
  t[78].percussion.noise_q = 1.36758f;
  t[78].percussion.plate_hf_ratio = 0.94522f;
  t[78].percussion.plate_t60_s = 0.508896f;
  t[79].percussion.exclusive_class = 2;
  // The module reads a 30.43 ms attack against the fitted near-zero rise.
  t[79].amp_env.attack_ms = 103.112f;
  t[79].amp_env.decay_ms = 128.966f;
  t[79].amp_env.sustain = 0.0534685f;
  t[79].cutoff_hz = 1202.58f;
  t[79].drive = 0.561334f;
  t[79].percussion.contact = 0.238538f;
  t[79].percussion.noise_gain = 0.874871f;
  t[79].percussion.phisem_beans = 0.989307f;
  t[79].percussion.phisem_energy_ms = 25.1193f;
  t[79].percussion.phisem_pitch_glide = 0.388877f;
  t[79].percussion.phisem_res_hz = 8283.32f;
  t[79].percussion.phisem_res_q = 16.8651f;
  t[79].percussion.phisem_scrape_hz = 28.7085f;
  t[79].percussion.phisem_sound_ms = 6.62807f;
  t[79].percussion.plate_gain = 0.0471479f;
  t[79].stereo_spread = 0.546511f;
  t[79].gain = 0.804f;
  t[79].percussion.contact_ms = 1.89666f;
  t[79].percussion.noise_cutoff_hz = 943.037f;
  t[79].percussion.noise_decay_ms = 449.573f;
  t[79].percussion.noise_q = 1.15262f;
  t[79].percussion.plate_hf_ratio = 0.355839f;
  t[79].percussion.plate_t60_s = 0.386769f;
  t[79].resonance_q = 0.898862f;

  // The body the collisions happen inside, radiating alongside the bright band
  // above rather than through it. Fitted against the measured kit on the bands
  // that reference can still resolve: a tambourine's frame and head are one
  // broad low shelf, a maraca's gourd is a little narrower, and a guiro's is a
  // narrow peak that stands 23 dB over the third of an octave beneath it. The
  // shakers left without one are the ones the measurement says have none - the
  // cabasa is flat from 125 Hz to 1 kHz, and the long guiro's and vibraslap's
  // errors are in the midrange, where a body cannot reach them.
  t[54].percussion.phisem_body_hz = 252.024f;  // Tambourine
  t[54].percussion.phisem_body_q = 1.52187f;
  t[54].percussion.phisem_body_gain = 3.89162f;
  t[54].amp_env.attack_ms = 4.1684f;
  t[54].amp_env.decay_ms = 74.308f;
  t[54].amp_env.sustain = 0.0552075f;
  t[54].cutoff_hz = 8615.66f;
  t[54].percussion.noise_gain = 2.33104f;
  t[54].percussion.phisem_beans = 59.3119f;
  t[54].percussion.phisem_body_hz = 154.259f;
  t[54].percussion.phisem_energy_ms = 78.3007f;
  t[54].percussion.phisem_res_hz = 1502.7f;
  t[54].percussion.phisem_res_q = 1.5553f;
  t[54].percussion.phisem_sound_ms = 1.08046f;
  t[54].percussion.plate_gain = 0.113041f;
  t[54].stereo_spread = 0.398522f;
  t[54].drive = 0.144093f;
  t[54].percussion.noise_cutoff_hz = 7903.05f;
  t[54].percussion.noise_decay_ms = 937.655f;
  t[54].percussion.noise_q = 5.13818f;
  t[54].percussion.plate_hf_ratio = 0.575388f;
  t[54].percussion.plate_low_hz = 180.348f;
  t[54].percussion.plate_t60_s = 3.12184f;
  t[54].resonance_q = 3.05997f;
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
