/// @file presets.cpp
/// @brief Implementation of built-in mastering chain presets.
/// @details Each factory below configures only modules that the current
/// MasteringChain class supports (repair.declick, repair.dereverb,
/// repair.denoise, eq.tilt, dynamics.deesser, dynamics.transientShaper,
/// dynamics.compressor, dynamics.multibandComp, saturation.tape,
/// saturation.exciter, spectral.airBand, stereo.imager, stereo.monoMaker,
/// maximizer.truePeakLimiter, loudness). The numbers below are conservative
/// starting points intended to be refined per program material.
///
/// Every number below is one of three kinds and its comment says which: a
/// standard specifies it, a published recommendation bounds it, or it is an
/// original choice with no source, named for the effect it aims at. Sources,
/// by the short names used below:
///
/// ITU-R BS.1770-5 (2023), Algorithms to measure audio programme loudness and true-peak audio level
/// EBU R 128 (2020), Loudness normalisation and permitted maximum level of audio signals
/// ATSC A/85 (2013), Techniques for Establishing and Maintaining Audio Loudness
///     for Digital Television
/// AES TD1004.1.15-10 (2015), Recommendation for Loudness of Audio Streaming
///     and Network File Playback

#include "mastering/api/presets.h"

#include <string>
#include <utility>

#include "mastering/api/chain.h"
#include "util/exception.h"

namespace sonare::mastering::api {

void enable_loudness(MasteringChainConfig& cfg, float target_lufs, float ceiling_db) {
  // The loudness stage runs its own true-peak limiter at this same ceiling after
  // applying the normalization gain, so it alone guarantees the ceiling. Enabling
  // the standalone maximizer limiter as well would limit the signal twice around
  // the loudness gain (limit -> re-amplify -> limit), pumping transient-heavy
  // material for no ceiling benefit. Presets therefore rely on the loudness
  // stage's built-in limiter only.
  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = target_lufs;
  cfg.loudness.ceiling_db = ceiling_db;
  // BS.1770 Annex 2 (true-peak metering) specifies at least 4x oversampling.
  cfg.loudness.true_peak_oversample = 4;
}

namespace {

// ---------------------------------------------------------------------------
// Per-preset factories. Each starts from a default MasteringChainConfig and
// flips on / configures the stages relevant to the preset.
//
// The voicing stages (tilt, compressor, transient shaper, tape, exciter, air
// band, imager) have no published source at any preset. Pestana & Reiss (2014),
// Intelligent Audio Production Strategies Informed by Best Practices, AES 53rd
// Conference, documents that per-genre conventions of this kind exist without
// specifying these values, so each is an original choice and its comment names
// the effect it aims at. Only the loudness targets and ceilings carry sources,
// cited at each enable_loudness call.
// ---------------------------------------------------------------------------

MasteringChainConfig make_pop() {
  MasteringChainConfig cfg;

  // Voicing: a bright, forward master that stays intelligible after lossy encoding.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.5f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -18.0f;
  cfg.dynamics.compressor.config.ratio = 2.5f;
  cfg.dynamics.compressor.config.attack_ms = 5.0f;
  cfg.dynamics.compressor.config.release_ms = 50.0f;

  cfg.dynamics.transient_shaper.enabled = true;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.0f;

  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.15f;

  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.1f;

  // No standard specifies -14 LUFS; it is the de facto streaming normalisation
  // level, louder than the -16 to -20 LUFS TD1004 recommends. The -1 dBTP
  // ceiling is R128's permitted maximum true peak.
  enable_loudness(cfg, -14.0f, -1.0f);

  return cfg;
}

MasteringChainConfig make_edm() {
  MasteringChainConfig cfg;

  // Voicing: hyped top end and a wide image, for club-oriented playback.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 1.0f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.dynamics.compressor.config.attack_ms = 3.0f;
  cfg.dynamics.compressor.config.release_ms = 40.0f;

  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.25f;

  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.3f;

  // No source for either: -12 LUFS trades normalisation headroom for density,
  // and -0.3 dBTP keeps 0.7 dB less headroom than R128's -1 dBTP maximum.
  enable_loudness(cfg, -12.0f, -0.3f);

  return cfg;
}

MasteringChainConfig make_acoustic() {
  MasteringChainConfig cfg;

  // Voicing: neutral tilt and slow, shallow compression that leaves the
  // performance dynamics audible.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.0f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -22.0f;
  cfg.dynamics.compressor.config.ratio = 1.5f;
  cfg.dynamics.compressor.config.attack_ms = 20.0f;
  cfg.dynamics.compressor.config.release_ms = 200.0f;

  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 0.95f;

  // -16 LUFS is the loud end of TD1004's -16 to -20 LUFS band. The -1.5 dBTP
  // ceiling has no source: 0.5 dB under R128's maximum, for encoder overshoot.
  enable_loudness(cfg, -16.0f, -1.5f);

  return cfg;
}

MasteringChainConfig make_hiphop() {
  MasteringChainConfig cfg;

  // Slight low-shelf-ish lean via negative tilt (low boost when pivot is mid).
  // Voicing: tape and exciter over that lean, for a dense, weighted bottom end.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = -0.5f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -18.0f;
  cfg.dynamics.compressor.config.ratio = 2.0f;

  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 2.0f;
  cfg.saturation.tape.config.saturation = 0.2f;

  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.10f;

  // No source for either: -13 LUFS is chosen to read as loud where nothing
  // normalises, and -0.5 dBTP gives up half of R128's -1 dBTP headroom for it.
  enable_loudness(cfg, -13.0f, -0.5f);

  return cfg;
}

MasteringChainConfig make_ai_music() {
  MasteringChainConfig cfg;

  // Light spectral repair for AI-generated artifacts.
  cfg.repair.declick.enabled = true;
  cfg.repair.dereverb.enabled = true;
  cfg.repair.denoise.enabled = true;

  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.3f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -18.0f;
  cfg.dynamics.compressor.config.ratio = 2.0f;

  // Voicing: an air lift above 14 kHz and a mono low end, chosen to open a dull
  // top and tighten a phase-loose bottom without touching the midrange.
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.6f;
  cfg.spectral.air_band.config.shelf_frequency_hz = 14000.0f;

  cfg.stereo.mono_maker.enabled = true;
  cfg.stereo.mono_maker.config.amount = 0.3f;

  // -14 LUFS is the de facto streaming level and carries no standard; -1 dBTP
  // is R128's permitted maximum true peak.
  enable_loudness(cfg, -14.0f, -1.0f);

  return cfg;
}

MasteringChainConfig make_speech() {
  MasteringChainConfig cfg;

  cfg.repair.denoise.enabled = true;

  // Positive tilt -> high-frequency emphasis for intelligibility.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 1.0f;

  // Tame sibilant high-frequency energy typical in vocal sources.
  cfg.dynamics.deesser.enabled = true;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -20.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.dynamics.compressor.config.attack_ms = 5.0f;
  cfg.dynamics.compressor.config.release_ms = 100.0f;

  // -16 LUFS is the loud end of TD1004's -16 to -20 LUFS band; -1 dBTP is
  // R128's permitted maximum true peak.
  enable_loudness(cfg, -16.0f, -1.0f);

  return cfg;
}

MasteringChainConfig make_streaming() {
  // INTENTIONAL ALIAS of make_pop(). The generic "streaming" target
  // (-14 LUFS, -1 dBTP) coincides exactly with the pop preset's loudness and
  // voicing, so this preset is deliberately identical rather than a
  // contrived variant with invented DSP deviations. Platform-specific masters
  // (youtube, broadcast, podcast, ...) carry their own distinct configs; this
  // entry exists as a discoverable, neutral default for the generic streaming
  // use case. If a genuinely distinct streaming voicing is ever desired,
  // override stages here instead of returning make_pop().
  return make_pop();
}

MasteringChainConfig make_youtube() {
  auto cfg = make_pop();
  // Voicing: pop with a harder front edge and a slightly wider image, chosen to
  // keep impact once a video platform's loudness normalisation pulls it down.
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.8f;
  cfg.saturation.exciter.config.amount = 0.2f;
  cfg.stereo.imager.config.width = 1.15f;
  // Same -14 LUFS / -1 dBTP as pop: de facto streaming level, R128 ceiling.
  enable_loudness(cfg, -14.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_broadcast() {
  MasteringChainConfig cfg;
  // Voicing: near-neutral, slow and shallow, so the loudness range R128 expects
  // to survive transmission is not compressed away before the loudness stage.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.2f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.4f;
  cfg.dynamics.compressor.config.attack_ms = 25.0f;
  cfg.dynamics.compressor.config.release_ms = 250.0f;
  // R128: -23 LUFS programme loudness, -1 dBTP permitted maximum true peak.
  enable_loudness(cfg, -23.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_podcast() {
  auto cfg = make_speech();
  // Voicing: speech held tighter, since a podcast is heard in noisy places.
  cfg.dynamics.compressor.config.threshold_db = -22.0f;
  cfg.dynamics.compressor.config.ratio = 3.5f;
  cfg.dynamics.deesser.enabled = true;
  // -16 LUFS is the loud end of TD1004's -16 to -20 LUFS band. The -1.5 dBTP
  // ceiling has no source: 0.5 dB under R128's maximum, for encoder overshoot.
  enable_loudness(cfg, -16.0f, -1.5f);
  return cfg;
}

MasteringChainConfig make_audiobook() {
  auto cfg = make_speech();
  // Voicing: declick for mouth noise, and a gentler tilt and ratio than podcast
  // because the listening lasts hours.
  cfg.repair.declick.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 2.2f;
  cfg.eq.tilt.tilt_db = 0.5f;
  // -18 LUFS sits inside TD1004's -16 to -20 LUFS band. The -3 dBTP ceiling has
  // no published source; audiobook delivery commonly asks for that much peak
  // headroom, and the pair keeps narration steady at low playback levels.
  enable_loudness(cfg, -18.0f, -3.0f);
  return cfg;
}

MasteringChainConfig make_cinema() {
  MasteringChainConfig cfg;
  // Voicing: barely any compression and a faintly dark tilt, so the dynamic
  // range a calibrated room is meant to reproduce reaches it intact.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = -0.2f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -28.0f;
  cfg.dynamics.compressor.config.ratio = 1.25f;
  cfg.dynamics.compressor.config.attack_ms = 30.0f;
  cfg.dynamics.compressor.config.release_ms = 300.0f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.05f;
  // -2 dBTP is A/85's maximum true peak. -27 LUFS has no source: theatrical
  // level is set by calibrated monitoring rather than an integrated target, so
  // the value is an original choice, low enough to leave that range unclipped.
  enable_loudness(cfg, -27.0f, -2.0f);
  return cfg;
}

MasteringChainConfig make_jpop() {
  auto cfg = make_pop();
  // Voicing: pop pushed brighter, harder and wider.
  cfg.eq.tilt.tilt_db = 0.8f;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.4f;
  cfg.saturation.exciter.config.amount = 0.22f;
  cfg.stereo.imager.config.width = 1.2f;
  // No source for either: -9 LUFS is a loudness-first target well above any
  // recommended level, and -0.5 dBTP gives up half of R128's headroom for it.
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_ambient() {
  MasteringChainConfig cfg;
  // Voicing: very slow, very shallow compression and a wide, airy image, so
  // slow swells keep their shape.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.1f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -26.0f;
  cfg.dynamics.compressor.config.ratio = 1.2f;
  cfg.dynamics.compressor.config.attack_ms = 40.0f;
  cfg.dynamics.compressor.config.release_ms = 400.0f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.3f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.35f;
  // -18 LUFS sits inside TD1004's -16 to -20 LUFS band; -1 dBTP is R128's
  // permitted maximum true peak.
  enable_loudness(cfg, -18.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_lofi() {
  MasteringChainConfig cfg;
  // Voicing: dark tilt and heavy tape, for the dulled, saturated character.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = -1.0f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -18.0f;
  cfg.dynamics.compressor.config.ratio = 2.0f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 4.0f;
  cfg.saturation.tape.config.saturation = 0.35f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 0.95f;
  // -11 LUFS has no source; it is chosen to sit loud without erasing the tape
  // stage's softening. -1 dBTP is R128's permitted maximum true peak.
  enable_loudness(cfg, -11.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_classical() {
  MasteringChainConfig cfg;
  // Voicing: the lightest touch of any preset, since the dynamic range is the
  // material.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.0f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -30.0f;
  cfg.dynamics.compressor.config.ratio = 1.15f;
  cfg.dynamics.compressor.config.attack_ms = 50.0f;
  cfg.dynamics.compressor.config.release_ms = 500.0f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.0f;
  // -23 LUFS is R128's programme loudness and -2 dBTP is A/85's maximum true
  // peak, but no standard pairs them: the extra headroom over R128's own
  // -1 dBTP is an original choice for wide-dynamic-range material.
  enable_loudness(cfg, -23.0f, -2.0f);

  return cfg;
}

MasteringChainConfig make_drum_and_bass() {
  auto cfg = make_edm();
  // Voicing: EDM with the transient edge restored, so fast breaks stay legible
  // under the heavier compression.
  cfg.eq.tilt.tilt_db = 0.7f;
  cfg.dynamics.compressor.config.threshold_db = -15.0f;
  cfg.dynamics.compressor.config.ratio = 3.5f;
  cfg.dynamics.transient_shaper.enabled = true;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.8f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 1.5f;
  cfg.stereo.imager.config.width = 1.25f;
  // No source for either: -8 LUFS is the loudest target here, chosen for club
  // playback, and -0.3 dBTP keeps 0.7 dB less headroom than R128 permits.
  enable_loudness(cfg, -8.0f, -0.3f);
  return cfg;
}

MasteringChainConfig make_techno() {
  auto cfg = make_edm();
  // Voicing: EDM with the hype pulled back and tape drive in its place, for a
  // flatter, more mechanical front.
  cfg.eq.tilt.tilt_db = 0.4f;
  cfg.dynamics.compressor.config.threshold_db = -17.0f;
  cfg.dynamics.compressor.config.ratio = 3.2f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 2.5f;
  cfg.saturation.exciter.config.amount = 0.18f;
  cfg.stereo.imager.config.width = 1.15f;
  // No source for either: -9 LUFS is a loudness-first club target, and
  // -0.4 dBTP trades most of R128's -1 dBTP headroom for it.
  enable_loudness(cfg, -9.0f, -0.4f);
  return cfg;
}

MasteringChainConfig make_metal() {
  MasteringChainConfig cfg;
  // Voicing: bright and hard, with the transient shaper keeping the attack of
  // fast picking and double kick from being swallowed.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.6f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 2.8f;
  cfg.dynamics.transient_shaper.enabled = true;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.0f;
  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.18f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.25f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.05f;
  // No source for either: -9 LUFS is a loudness-first target well above any
  // recommended level, and -0.5 dBTP gives up half of R128's headroom for it.
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_trap() {
  auto cfg = make_hiphop();
  // Voicing: hip hop leaned further down, for sub-weighted playback.
  cfg.eq.tilt.tilt_db = -0.8f;
  cfg.dynamics.compressor.config.threshold_db = -17.0f;
  cfg.dynamics.compressor.config.ratio = 2.4f;
  cfg.saturation.tape.config.drive_db = 2.8f;
  cfg.saturation.exciter.config.amount = 0.12f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.08f;
  // No source for either: -9 LUFS is a loudness-first target well above any
  // recommended level, and -0.5 dBTP gives up half of R128's headroom for it.
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_rnb() {
  MasteringChainConfig cfg;
  // Voicing: slow attack and warm tape, so vocal phrasing rides over a soft mix.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.2f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -20.0f;
  cfg.dynamics.compressor.config.ratio = 2.0f;
  cfg.dynamics.compressor.config.attack_ms = 15.0f;
  cfg.dynamics.compressor.config.release_ms = 120.0f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 1.2f;
  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.10f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.12f;
  // -12 LUFS has no source; it is chosen to sit above the streaming convention
  // without full loudness-first density. -1 dBTP is R128's permitted maximum.
  enable_loudness(cfg, -12.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_jazz() {
  auto cfg = make_acoustic();
  // Voicing: acoustic loosened further, with a trace of tape for warmth.
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.35f;
  cfg.dynamics.compressor.config.attack_ms = 30.0f;
  cfg.dynamics.compressor.config.release_ms = 250.0f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 0.8f;
  cfg.stereo.imager.config.width = 1.0f;
  // -18 LUFS sits inside TD1004's -16 to -20 LUFS band. The -1.5 dBTP ceiling
  // has no source: 0.5 dB under R128's maximum, for encoder overshoot.
  enable_loudness(cfg, -18.0f, -1.5f);
  return cfg;
}

MasteringChainConfig make_kpop() {
  auto cfg = make_jpop();
  // Voicing: J-pop taken further still, with an air lift on top.
  cfg.eq.tilt.tilt_db = 1.0f;
  cfg.dynamics.compressor.config.threshold_db = -15.0f;
  cfg.dynamics.compressor.config.ratio = 3.2f;
  cfg.saturation.exciter.config.amount = 0.28f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.35f;
  cfg.stereo.imager.config.width = 1.25f;
  // No source for either: -8 LUFS is the loudest target here, and -0.5 dBTP
  // gives up half of R128's -1 dBTP headroom for it.
  enable_loudness(cfg, -8.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_trance() {
  auto cfg = make_edm();
  // Voicing: EDM opened wider and higher, for long sustained pads and leads.
  cfg.eq.tilt.tilt_db = 0.9f;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.saturation.exciter.config.amount = 0.30f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.30f;
  cfg.stereo.imager.config.width = 1.35f;
  // No source for either: -8.5 LUFS is a loudness-first club target, and
  // -0.4 dBTP trades most of R128's -1 dBTP headroom for it.
  enable_loudness(cfg, -8.5f, -0.4f);
  return cfg;
}

MasteringChainConfig make_game_ost() {
  MasteringChainConfig cfg;
  // Voicing: gentle and wide, since the music sits under dialogue and effects
  // and is mixed again at playback.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.1f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.5f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.25f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.2f;
  // -16 LUFS is the loud end of TD1004's -16 to -20 LUFS band; -1 dBTP is
  // R128's permitted maximum true peak.
  enable_loudness(cfg, -16.0f, -1.0f);
  return cfg;
}

}  // namespace

std::vector<std::string> preset_names() {
  // Note: "streaming" is an intentional alias of "pop" (see make_streaming).
  // It is listed as its own discoverable name, not as an independent voicing.
  return {"pop",     "edm",       "acoustic",    "hipHop",    "aiMusic", "speech", "streaming",
          "youtube", "broadcast", "podcast",     "audiobook", "cinema",  "jpop",   "ambient",
          "lofi",    "classical", "drumAndBass", "techno",    "metal",   "trap",   "rnb",
          "jazz",    "kpop",      "trance",      "gameOst"};
}

Preset preset_from_string(const std::string& name) {
  if (name == "pop") return Preset::Pop;
  if (name == "edm") return Preset::EDM;
  if (name == "acoustic") return Preset::Acoustic;
  if (name == "hipHop") return Preset::HipHop;
  if (name == "aiMusic") return Preset::AIMusic;
  if (name == "speech") return Preset::Speech;
  if (name == "streaming") return Preset::Streaming;
  if (name == "youtube") return Preset::YouTube;
  if (name == "broadcast") return Preset::Broadcast;
  if (name == "podcast") return Preset::Podcast;
  if (name == "audiobook") return Preset::Audiobook;
  if (name == "cinema") return Preset::Cinema;
  if (name == "jpop") return Preset::JPop;
  if (name == "ambient") return Preset::Ambient;
  if (name == "lofi") return Preset::Lofi;
  if (name == "classical") return Preset::Classical;
  if (name == "drumAndBass") return Preset::DrumAndBass;
  if (name == "techno") return Preset::Techno;
  if (name == "metal") return Preset::Metal;
  if (name == "trap") return Preset::Trap;
  if (name == "rnb") return Preset::RnB;
  if (name == "jazz") return Preset::Jazz;
  if (name == "kpop") return Preset::KPop;
  if (name == "trance") return Preset::Trance;
  if (name == "gameOst") return Preset::GameOst;
  throw SonareException(ErrorCode::InvalidParameter, "unknown mastering preset: " + name);
}

const char* preset_to_string(Preset preset) noexcept {
  switch (preset) {
    case Preset::Pop:
      return "pop";
    case Preset::EDM:
      return "edm";
    case Preset::Acoustic:
      return "acoustic";
    case Preset::HipHop:
      return "hipHop";
    case Preset::AIMusic:
      return "aiMusic";
    case Preset::Speech:
      return "speech";
    case Preset::Streaming:
      return "streaming";
    case Preset::YouTube:
      return "youtube";
    case Preset::Broadcast:
      return "broadcast";
    case Preset::Podcast:
      return "podcast";
    case Preset::Audiobook:
      return "audiobook";
    case Preset::Cinema:
      return "cinema";
    case Preset::JPop:
      return "jpop";
    case Preset::Ambient:
      return "ambient";
    case Preset::Lofi:
      return "lofi";
    case Preset::Classical:
      return "classical";
    case Preset::DrumAndBass:
      return "drumAndBass";
    case Preset::Techno:
      return "techno";
    case Preset::Metal:
      return "metal";
    case Preset::Trap:
      return "trap";
    case Preset::RnB:
      return "rnb";
    case Preset::Jazz:
      return "jazz";
    case Preset::KPop:
      return "kpop";
    case Preset::Trance:
      return "trance";
    case Preset::GameOst:
      return "gameOst";
  }
  return "unknown";
}

MasteringChainConfig preset_config(Preset preset) {
  switch (preset) {
    case Preset::Pop:
      return make_pop();
    case Preset::EDM:
      return make_edm();
    case Preset::Acoustic:
      return make_acoustic();
    case Preset::HipHop:
      return make_hiphop();
    case Preset::AIMusic:
      return make_ai_music();
    case Preset::Speech:
      return make_speech();
    case Preset::Streaming:
      return make_streaming();
    case Preset::YouTube:
      return make_youtube();
    case Preset::Broadcast:
      return make_broadcast();
    case Preset::Podcast:
      return make_podcast();
    case Preset::Audiobook:
      return make_audiobook();
    case Preset::Cinema:
      return make_cinema();
    case Preset::JPop:
      return make_jpop();
    case Preset::Ambient:
      return make_ambient();
    case Preset::Lofi:
      return make_lofi();
    case Preset::Classical:
      return make_classical();
    case Preset::DrumAndBass:
      return make_drum_and_bass();
    case Preset::Techno:
      return make_techno();
    case Preset::Metal:
      return make_metal();
    case Preset::Trap:
      return make_trap();
    case Preset::RnB:
      return make_rnb();
    case Preset::Jazz:
      return make_jazz();
    case Preset::KPop:
      return make_kpop();
    case Preset::Trance:
      return make_trance();
    case Preset::GameOst:
      return make_game_ost();
  }
  // Unreachable for well-formed Preset values; defensive default.
  return MasteringChainConfig{};
}

MonoChainResult master_audio_mono(Preset preset, const float* samples, std::size_t length,
                                  int sample_rate, const Param* overrides,
                                  std::size_t override_count) {
  MasteringChainConfig config = preset_config(preset);
  if (overrides != nullptr && override_count > 0) {
    apply_chain_config_overrides(config, overrides, override_count);
  }
  MasteringChain chain(std::move(config));
  return chain.process_mono(samples, length, sample_rate);
}

StereoChainResult master_audio_stereo(Preset preset, const float* left, const float* right,
                                      std::size_t length, int sample_rate, const Param* overrides,
                                      std::size_t override_count) {
  MasteringChainConfig config = preset_config(preset);
  if (overrides != nullptr && override_count > 0) {
    apply_chain_config_overrides(config, overrides, override_count);
  }
  MasteringChain chain(std::move(config));
  return chain.process_stereo(left, right, length, sample_rate);
}

}  // namespace sonare::mastering::api
