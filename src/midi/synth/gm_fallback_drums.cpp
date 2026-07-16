#include "midi/synth/gm_fallback_data.h"

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

DrumPatches build_drum_patches() noexcept {
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

  // Hi-hats: high-passed noise shimmer, closed short / open ringing.
  d.closed_hat = piece;
  d.closed_hat.amp_env = fallback_env(0.5f, 90.0f, 0.0f, 40.0f);
  d.closed_hat.percussion.noise_gain = 1.0f;
  d.closed_hat.percussion.noise_decay_ms = 35.0f;
  d.closed_hat.percussion.noise_cutoff_hz = 7500.0f;
  d.closed_hat.percussion.noise_output = SynthFilterOutput::kHighpass;
  d.closed_hat.gain = 0.5f;
  d.open_hat = d.closed_hat;
  d.open_hat.amp_env = fallback_env(0.5f, 550.0f, 0.0f, 150.0f);
  d.open_hat.percussion.noise_decay_ms = 350.0f;

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
  d.cymbal.percussion.noise_cutoff_hz = 5500.0f;
  d.cymbal.percussion.noise_output = SynthFilterOutput::kHighpass;
  // Nonlinear shimmer: the inharmonic modes pump a high wash that swells after
  // the crash and rides the long ring -- the cymbal "bloom" a static bank
  // lacks.
  d.cymbal.percussion.shimmer = 6.0f;
  d.cymbal.percussion.shimmer_attack_ms = 60.0f;
  d.cymbal.percussion.shimmer_cutoff_hz = 9000.0f;
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

const DrumPatches& drum_patches() noexcept {
  static const DrumPatches kTable = build_drum_patches();
  return kTable;
}

// Per-note GM/GS drum map (keys 27..87): each key is a distinct instrument
// built from a mechanism archetype (fixed-pitch membrane / struck wood / struck
// metal / whistle / noise) plus a fixed tuning, on top of the shared kit
// archetypes above. Unmapped keys fall back to the generic short burst so every
// drum key stays audible. The stochastic (PhISEM) shakers and scrapers
// (maracas, cabasa, guiro, cuica, tambourine, vibraslap) are a separate
// archetype not yet built — they resolve to the generic burst here until it
// lands.
std::array<NativeSynthPatch, 128> build_drum_note_table() noexcept {
  const DrumPatches& d = drum_patches();
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

  // Shaker (PhISEM): a burst of stochastic bead collisions rung through a gourd
  // resonance — maracas, cabasa, shaker, tambourine, vibraslap.
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

  // --- existing kit archetypes (unchanged voicings) ---
  t[35] = d.kick;
  t[36] = d.kick;
  t[38] = d.snare;
  t[40] = d.snare;
  t[42] = d.closed_hat;  // closed
  t[44] = d.closed_hat;  // pedal
  t[46] = d.open_hat;
  t[41] = t[43] = t[45] = t[47] = t[48] = t[50] = d.tom;
  t[49] = t[52] = t[55] = t[57] = d.cymbal;  // crash 1 / china / splash / crash 2
  t[51] = t[59] = d.cymbal;                  // ride 1 / ride 2

  // Hi-hats share mute group 1; the open hat gets a snappy choke fade (release
  // is unused by one-shot voices in normal play, so this stays bit-identical
  // there — it only governs how fast a closed/pedal strike cuts the open hat).
  t[42].percussion.exclusive_class = 1;
  t[44].percussion.exclusive_class = 1;
  t[46].percussion.exclusive_class = 1;
  t[46].amp_env.release_ms = 40.0f;

  // --- wooden idiophones + clicks ---
  t[31] = make_wood(1000.0f, 0.0f, 0.03f, 0.6f);   // Sticks
  t[32] = make_wood(1000.0f, 0.0f, 0.02f, 0.5f);   // Square Click
  t[33] = make_wood(1200.0f, 0.0f, 0.02f, 0.5f);   // Metronome Click
  t[37] = make_wood(820.0f, 0.0f, 0.05f, 0.7f);    // Side Stick
  t[75] = make_wood(2500.0f, 0.0f, 0.025f, 0.6f);  // Claves (2500 Hz, ~25 ms)
  t[76] = make_wood(1200.0f, 0.0f, 0.06f, 0.6f);   // Hi Wood Block
  t[77] = make_wood(800.0f, 0.0f, 0.07f, 0.6f);    // Low Wood Block
  t[85] = make_wood(1800.0f, 0.0f, 0.02f, 0.5f);   // Castanets

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
  t[60] = make_membrane(260.0f, 0.18f, 0.30f, 0.0f, 0.70f);    // Hi Bongo
  t[61] = make_membrane(180.0f, 0.20f, 0.30f, 0.0f, 0.70f);    // Low Bongo
  t[62] = make_membrane(220.0f, 0.08f, 0.20f, 0.0f, 0.70f);    // Mute Hi Conga
  t[63] = make_membrane(200.0f, 0.25f, 0.30f, 0.0f, 0.70f);    // Open Hi Conga
  t[64] = make_membrane(130.0f, 0.30f, 0.35f, 0.0f, 0.75f);    // Low Conga
  t[65] = make_membrane(250.0f, 0.22f, 0.20f, 700.0f, 0.70f);  // High Timbale
  t[66] = make_membrane(200.0f, 0.26f, 0.20f, 550.0f, 0.70f);  // Low Timbale
  t[86] = make_membrane(95.0f, 0.12f, 0.40f, 0.0f, 0.80f);     // Mute Surdo
  t[87] = make_membrane(80.0f, 0.40f, 0.50f, 0.0f, 0.85f);     // Open Surdo
  t[86].percussion.exclusive_class = 6;
  t[87].percussion.exclusive_class = 6;

  // --- whistles (mute group 4) + hand clap ---
  t[71] = make_whistle(1400.0f, 0.12f, 0.5f);  // Short Whistle
  t[72] = make_whistle(1400.0f, 0.50f, 0.5f);  // Long Whistle
  t[71].percussion.exclusive_class = 4;
  t[72].percussion.exclusive_class = 4;
  t[39] = clap;  // Hand Clap

  // --- PhISEM shakers + scrapers ---
  t[54] = make_shaker(32.0f, 120.0f, 2500.0f, 2.0f, 0.5f);   // Tambourine
  t[58] = make_shaker(24.0f, 400.0f, 2500.0f, 3.0f, 0.45f);  // Vibraslap
  t[69] = make_shaker(24.0f, 90.0f, 4000.0f, 1.0f, 0.5f);    // Cabasa
  t[70] = make_shaker(20.0f, 90.0f, 3200.0f, 1.5f, 0.5f);    // Maracas
  t[82] = make_shaker(28.0f, 110.0f, 6000.0f, 1.0f, 0.5f);   // Shaker
  // Guiro (mute group 5): ratchet ridge train.
  t[73] = make_scrape(8.0f, 120.0f, 150.0f, 2500.0f, 3.0f, 0.0f, 0.5f);  // Short Guiro
  t[74] = make_scrape(8.0f, 500.0f, 120.0f, 2500.0f, 3.0f, 0.0f, 0.5f);  // Long Guiro
  t[73].percussion.exclusive_class = 5;
  t[74].percussion.exclusive_class = 5;
  // Cuica (mute group 2): friction drum with a resonance pitch glide.
  t[78] = make_scrape(6.0f, 120.0f, 40.0f, 400.0f, 3.0f, -0.3f, 0.55f);  // Mute Cuica (down)
  t[79] = make_scrape(6.0f, 250.0f, 40.0f, 500.0f, 3.0f, 0.5f, 0.55f);   // Open Cuica (up)
  t[78].percussion.exclusive_class = 2;
  t[79].percussion.exclusive_class = 2;

  for (auto& p : t) p = clamp_synth_patch(p);
  return t;
}

}  // namespace

const std::array<NativeSynthPatch, 128>& drum_note_table() noexcept {
  static const std::array<NativeSynthPatch, 128> kTable = build_drum_note_table();
  return kTable;
}

}  // namespace sonare::midi::synth::detail
