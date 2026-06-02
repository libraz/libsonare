#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "sonare_c.h"

namespace {

SonareRirSynthConfig valid_rir_config() {
  SonareRirSynthConfig cfg{};
  cfg.length_m = 7.0f;
  cfg.width_m = 5.0f;
  cfg.height_m = 3.0f;
  cfg.source_x = 1.5f;
  cfg.source_y = 1.0f;
  cfg.source_z = 1.2f;
  cfg.listener_x = 5.0f;
  cfg.listener_y = 4.0f;
  cfg.listener_z = 1.7f;
  cfg.absorption = 0.15f;
  cfg.max_seconds = 0.1f;
  cfg.mixing_time_ms = 0.0f;
  cfg.crossfade_ms = 0.0f;
  cfg.ism_order = 3;
  cfg.late_model = SONARE_REVERB_MODEL_EYRING;
  cfg.seed = 1u;
  return cfg;
}

SonareRoomEstimateConfig valid_estimate_config() {
  SonareRoomEstimateConfig cfg{};
  cfg.aspect_hint_lw = 7.0f / 5.0f;
  cfg.aspect_hint_lh = 7.0f / 3.0f;
  cfg.reference_absorption = 0.15f;
  cfg.min_decay_db = 0.0f;
  cfg.noise_floor_margin_db = 0.0f;
  cfg.prefer_eyring = 1;
  cfg.n_octave_bands = 0;
  cfg.mode = SONARE_ACOUSTIC_MODE_AUTO;
  return cfg;
}

}  // namespace

TEST_CASE("sonare acoustic C API synthesizes and frees RIRs", "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  SonareRirSynthResult result{};

  const SonareError err = sonare_synthesize_rir(&cfg, 48000, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.has_error == 0);
  REQUIRE(result.sample_rate == 48000);
  REQUIRE(result.length > 0);
  REQUIRE(result.rir != nullptr);

  bool nonzero = false;
  for (size_t i = 0; i < result.length; ++i) {
    nonzero = nonzero || std::abs(result.rir[i]) > 0.0f;
  }
  REQUIRE(nonzero);

  sonare_free_rir_synth_result(&result);
  REQUIRE(result.rir == nullptr);
  REQUIRE(result.length == 0);
}

TEST_CASE("sonare acoustic C API honors the late-tail model selector", "[c_api][acoustic]") {
  // A more absorptive room makes Sabine and Eyring diverge (Eyring is preferred
  // for alpha-bar above ~0.2); the two models must produce different tails.
  SonareRirSynthConfig sabine = valid_rir_config();
  sabine.absorption = 0.4f;
  sabine.max_seconds = 0.3f;
  sabine.late_model = SONARE_REVERB_MODEL_SABINE;
  SonareRirSynthConfig eyring = sabine;
  eyring.late_model = SONARE_REVERB_MODEL_EYRING;

  SonareRirSynthResult sabine_rir{};
  SonareRirSynthResult eyring_rir{};
  REQUIRE(sonare_synthesize_rir(&sabine, 48000, &sabine_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&eyring, 48000, &eyring_rir) == SONARE_OK);
  REQUIRE(sabine_rir.has_error == 0);
  REQUIRE(eyring_rir.has_error == 0);
  REQUIRE(sabine_rir.length > 0);
  REQUIRE(eyring_rir.length > 0);

  bool differs = sabine_rir.length != eyring_rir.length;
  const size_t common =
      sabine_rir.length < eyring_rir.length ? sabine_rir.length : eyring_rir.length;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(sabine_rir.rir[i] - eyring_rir.rir[i]) > 1e-6f;
  }
  REQUIRE(differs);

  sonare_free_rir_synth_result(&sabine_rir);
  sonare_free_rir_synth_result(&eyring_rir);
}

TEST_CASE("sonare acoustic C API reports invalid geometry as empty RIR", "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.source_x = 99.0f;
  SonareRirSynthResult result{};

  const SonareError err = sonare_synthesize_rir(&cfg, 48000, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.has_error == 1);
  REQUIRE(result.length == 0);
  REQUIRE(result.rir == nullptr);
  sonare_free_rir_synth_result(&result);
}

TEST_CASE("sonare acoustic C API estimates a room from a synthesized RIR", "[c_api][acoustic]") {
  SonareRirSynthConfig synth_cfg = valid_rir_config();
  synth_cfg.max_seconds = 0.0f;
  SonareRirSynthResult rir{};
  REQUIRE(sonare_synthesize_rir(&synth_cfg, 48000, &rir) == SONARE_OK);
  REQUIRE(rir.rir != nullptr);
  REQUIRE(rir.length > 0);

  SonareRoomEstimateConfig estimate_cfg = valid_estimate_config();
  SonareRoomEstimate estimate{};
  const SonareError err =
      sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &estimate);

  REQUIRE(err == SONARE_OK);
  REQUIRE(estimate.volume > 0.0f);
  REQUIRE(estimate.length_m > 0.0f);
  REQUIRE(estimate.width_m > 0.0f);
  REQUIRE(estimate.height_m > 0.0f);
  REQUIRE(estimate.confidence > 0.0f);
  REQUIRE(estimate.band_count > 0);
  REQUIRE(estimate.absorption_bands != nullptr);
  REQUIRE(estimate.rt60_bands != nullptr);

  sonare_free_room_estimate(&estimate);
  REQUIRE(estimate.absorption_bands == nullptr);
  REQUIRE(estimate.rt60_bands == nullptr);
  REQUIRE(estimate.band_count == 0);
  sonare_free_rir_synth_result(&rir);
}

TEST_CASE("sonare acoustic C API room morph appends a target tail", "[c_api][acoustic]") {
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig cfg{};
  cfg.length_m = 12.0f;
  cfg.width_m = 9.0f;
  cfg.height_m = 5.0f;
  cfg.source_x = 2.0f;
  cfg.source_y = 2.0f;
  cfg.source_z = 1.5f;
  cfg.listener_x = 8.0f;
  cfg.listener_y = 6.0f;
  cfg.listener_z = 1.7f;
  cfg.absorption = 0.08f;
  cfg.source_tail_suppression = 0.5f;
  cfg.wet = 0.7f;
  cfg.max_seconds = 0.1f;
  cfg.ism_order = 3;
  cfg.seed = 1u;

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length);

  REQUIRE(err == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_length > input.size());
  REQUIRE(std::abs(out[0]) > 0.0f);
  sonare_free_floats(out);
}
