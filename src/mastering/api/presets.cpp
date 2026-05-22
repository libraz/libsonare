/// @file presets.cpp
/// @brief Implementation of built-in mastering chain presets.
/// @details Each factory below configures only modules that the current
/// MasteringChain class supports (repair.denoise, eq.tilt, dynamics.compressor,
/// saturation.tape, saturation.exciter, spectral.airBand, stereo.imager,
/// stereo.monoMaker, maximizer.truePeakLimiter, loudness). Numbers are an
/// initial pass derived from backup/mastering-demo-plan.md §6.1-6.6; they will
/// be tuned in a follow-up phase.

#include "mastering/api/presets.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "mastering/api/chain.h"

namespace sonare::mastering::api {
namespace {

// ---------------------------------------------------------------------------
// Per-preset factories. Each starts from a default MasteringChainConfig and
// flips on / configures the stages relevant to the preset.
// ---------------------------------------------------------------------------

MasteringChainConfig make_pop() {
  MasteringChainConfig cfg;

  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 0.5f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -18.0f;
  cfg.dynamics.compressor.config.ratio = 2.5f;
  cfg.dynamics.compressor.config.attack_ms = 5.0f;
  cfg.dynamics.compressor.config.release_ms = 50.0f;

  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.amount = 0.15f;

  cfg.stereo.imager.enabled = true;
  cfg.stereo.imager.config.width = 1.1f;

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -14.0f;
  cfg.loudness.ceiling_db = -1.0f;
  cfg.loudness.true_peak_oversample = 4;

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

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -0.3f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -12.0f;
  cfg.loudness.ceiling_db = -0.3f;
  cfg.loudness.true_peak_oversample = 4;

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

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -1.5f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -16.0f;
  cfg.loudness.ceiling_db = -1.5f;
  cfg.loudness.true_peak_oversample = 4;

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

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -0.5f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -13.0f;
  cfg.loudness.ceiling_db = -0.5f;
  cfg.loudness.true_peak_oversample = 4;

  return cfg;
}

MasteringChainConfig make_ai_music() {
  MasteringChainConfig cfg;

  // Light spectral repair for AI-generated artifacts.
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

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -14.0f;
  cfg.loudness.ceiling_db = -1.0f;
  cfg.loudness.true_peak_oversample = 4;

  return cfg;
}

MasteringChainConfig make_speech() {
  MasteringChainConfig cfg;

  cfg.repair.denoise.enabled = true;

  // Positive tilt -> high-frequency emphasis for intelligibility.
  cfg.eq.tilt.enabled = true;
  cfg.eq.tilt.tilt_db = 1.0f;

  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.threshold_db = -20.0f;
  cfg.dynamics.compressor.config.ratio = 3.0f;
  cfg.dynamics.compressor.config.attack_ms = 5.0f;
  cfg.dynamics.compressor.config.release_ms = 100.0f;

  cfg.maximizer.true_peak_limiter.enabled = true;
  cfg.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;

  cfg.loudness.enabled = true;
  cfg.loudness.target_lufs = -16.0f;
  cfg.loudness.ceiling_db = -1.0f;
  cfg.loudness.true_peak_oversample = 4;

  return cfg;
}

}  // namespace

std::vector<std::string> preset_names() {
  return {"pop", "edm", "acoustic", "hipHop", "aiMusic", "speech"};
}

Preset preset_from_string(const std::string& name) {
  if (name == "pop") return Preset::Pop;
  if (name == "edm") return Preset::EDM;
  if (name == "acoustic") return Preset::Acoustic;
  if (name == "hipHop") return Preset::HipHop;
  if (name == "aiMusic") return Preset::AIMusic;
  if (name == "speech") return Preset::Speech;
  throw std::invalid_argument("unknown mastering preset: " + name);
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
