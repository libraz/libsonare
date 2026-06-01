/// @file presets.cpp
/// @brief Implementation of built-in mastering chain presets.
/// @details Each factory below configures only modules that the current
/// MasteringChain class supports (repair.declick, repair.dereverb,
/// repair.denoise, eq.tilt, dynamics.deesser, dynamics.transientShaper,
/// dynamics.compressor, dynamics.multibandComp, saturation.tape,
/// saturation.exciter, spectral.airBand, stereo.imager, stereo.monoMaker,
/// maximizer.truePeakLimiter, loudness). Numbers are an initial pass derived
/// from backup/mastering-demo-plan.md §6.1-6.6; they will be tuned in a
/// follow-up phase.

#include "mastering/api/presets.h"

#include <string>
#include <utility>

#include "mastering/api/chain.h"
#include "util/exception.h"

namespace sonare::mastering::api {
namespace {

// ---------------------------------------------------------------------------
// Per-preset factories. Each starts from a default MasteringChainConfig and
// flips on / configures the stages relevant to the preset.
// ---------------------------------------------------------------------------

void enable_loudness(MasteringChainConfig& cfg, float target_lufs, float ceiling_db) {
  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = ceiling_db;
  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = target_lufs;
  cfg.loudness.ceiling_db = ceiling_db;
  cfg.loudness.true_peak_oversample = 4;
}

MasteringChainConfig make_pop() {
  MasteringChainConfig cfg;

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

  enable_loudness(cfg, -14.0f, -1.0f);

  return cfg;
}

MasteringChainConfig make_edm() {
  MasteringChainConfig cfg;

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

  enable_loudness(cfg, -12.0f, -0.3f);

  return cfg;
}

MasteringChainConfig make_acoustic() {
  MasteringChainConfig cfg;

  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.0f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -22.0f;
  cfg.dynamics.compressor.config.ratio = 1.5f;
  cfg.dynamics.compressor.config.attack_ms = 20.0f;
  cfg.dynamics.compressor.config.release_ms = 200.0f;

  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 0.95f;

  enable_loudness(cfg, -16.0f, -1.5f);

  return cfg;
}

MasteringChainConfig make_hiphop() {
  MasteringChainConfig cfg;

  // Slight low-shelf-ish lean via negative tilt (low boost when pivot is mid).
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

  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.6f;
  cfg.spectral.air_band.config.shelf_frequency_hz = 14000.0f;

  cfg.stereo.mono_maker.enabled = true;
  cfg.stereo.mono_maker.config.amount = 0.3f;

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
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.8f;
  cfg.saturation.exciter.config.amount = 0.2f;
  cfg.stereo.imager.config.width = 1.15f;
  enable_loudness(cfg, -14.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_broadcast() {
  MasteringChainConfig cfg;
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.2f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.4f;
  cfg.dynamics.compressor.config.attack_ms = 25.0f;
  cfg.dynamics.compressor.config.release_ms = 250.0f;
  enable_loudness(cfg, -23.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_podcast() {
  auto cfg = make_speech();
  cfg.dynamics.compressor.config.threshold_db = -22.0f;
  cfg.dynamics.compressor.config.ratio = 3.5f;
  cfg.dynamics.deesser.enabled = true;
  enable_loudness(cfg, -16.0f, -1.5f);
  return cfg;
}

MasteringChainConfig make_audiobook() {
  auto cfg = make_speech();
  cfg.repair.declick.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 2.2f;
  cfg.eq.tilt.tilt_db = 0.5f;
  enable_loudness(cfg, -18.0f, -3.0f);
  return cfg;
}

MasteringChainConfig make_cinema() {
  MasteringChainConfig cfg;
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = -0.2f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -28.0f;
  cfg.dynamics.compressor.config.ratio = 1.25f;
  cfg.dynamics.compressor.config.attack_ms = 30.0f;
  cfg.dynamics.compressor.config.release_ms = 300.0f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.05f;
  enable_loudness(cfg, -27.0f, -2.0f);
  return cfg;
}

MasteringChainConfig make_jpop() {
  auto cfg = make_pop();
  cfg.eq.tilt.tilt_db = 0.8f;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.4f;
  cfg.saturation.exciter.config.amount = 0.22f;
  cfg.stereo.imager.config.width = 1.2f;
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_ambient() {
  MasteringChainConfig cfg;
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
  enable_loudness(cfg, -18.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_lofi() {
  MasteringChainConfig cfg;
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
  enable_loudness(cfg, -11.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_classical() {
  MasteringChainConfig cfg;
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.0f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -30.0f;
  cfg.dynamics.compressor.config.ratio = 1.15f;
  cfg.dynamics.compressor.config.attack_ms = 50.0f;
  cfg.dynamics.compressor.config.release_ms = 500.0f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.0f;
  enable_loudness(cfg, -23.0f, -2.0f);

  return cfg;
}

MasteringChainConfig make_drum_and_bass() {
  auto cfg = make_edm();
  cfg.eq.tilt.tilt_db = 0.7f;
  cfg.dynamics.compressor.config.threshold_db = -15.0f;
  cfg.dynamics.compressor.config.ratio = 3.5f;
  cfg.dynamics.transient_shaper.enabled = true;
  cfg.dynamics.transient_shaper.config.attack_gain_db = 2.8f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 1.5f;
  cfg.stereo.imager.config.width = 1.25f;
  enable_loudness(cfg, -8.0f, -0.3f);
  return cfg;
}

MasteringChainConfig make_techno() {
  auto cfg = make_edm();
  cfg.eq.tilt.tilt_db = 0.4f;
  cfg.dynamics.compressor.config.threshold_db = -17.0f;
  cfg.dynamics.compressor.config.ratio = 3.2f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 2.5f;
  cfg.saturation.exciter.config.amount = 0.18f;
  cfg.stereo.imager.config.width = 1.15f;
  enable_loudness(cfg, -9.0f, -0.4f);
  return cfg;
}

MasteringChainConfig make_metal() {
  MasteringChainConfig cfg;
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
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_trap() {
  auto cfg = make_hiphop();
  cfg.eq.tilt.tilt_db = -0.8f;
  cfg.dynamics.compressor.config.threshold_db = -17.0f;
  cfg.dynamics.compressor.config.ratio = 2.4f;
  cfg.saturation.tape.config.drive_db = 2.8f;
  cfg.saturation.exciter.config.amount = 0.12f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.08f;
  enable_loudness(cfg, -9.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_rnb() {
  MasteringChainConfig cfg;
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
  enable_loudness(cfg, -12.0f, -1.0f);
  return cfg;
}

MasteringChainConfig make_jazz() {
  auto cfg = make_acoustic();
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.35f;
  cfg.dynamics.compressor.config.attack_ms = 30.0f;
  cfg.dynamics.compressor.config.release_ms = 250.0f;
  cfg.saturation.tape.enabled = true;
  cfg.saturation.tape.config.drive_db = 0.8f;
  cfg.stereo.imager.config.width = 1.0f;
  enable_loudness(cfg, -18.0f, -1.5f);
  return cfg;
}

MasteringChainConfig make_kpop() {
  auto cfg = make_jpop();
  cfg.eq.tilt.tilt_db = 1.0f;
  cfg.dynamics.compressor.config.threshold_db = -15.0f;
  cfg.dynamics.compressor.config.ratio = 3.2f;
  cfg.saturation.exciter.config.amount = 0.28f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.35f;
  cfg.stereo.imager.config.width = 1.25f;
  enable_loudness(cfg, -8.0f, -0.5f);
  return cfg;
}

MasteringChainConfig make_trance() {
  auto cfg = make_edm();
  cfg.eq.tilt.tilt_db = 0.9f;
  cfg.dynamics.compressor.config.threshold_db = -16.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.saturation.exciter.config.amount = 0.30f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.30f;
  cfg.stereo.imager.config.width = 1.35f;
  enable_loudness(cfg, -8.5f, -0.4f);
  return cfg;
}

MasteringChainConfig make_game_ost() {
  MasteringChainConfig cfg;
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.1f;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -24.0f;
  cfg.dynamics.compressor.config.ratio = 1.5f;
  cfg.spectral.air_band.enabled = true;
  cfg.spectral.air_band.config.amount = 0.25f;
  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.2f;
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
