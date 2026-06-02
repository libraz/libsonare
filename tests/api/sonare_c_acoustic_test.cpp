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
  cfg.ism_order = 3;
  cfg.seed = 1u;
  return cfg;
}

SonareRoomEstimateConfig valid_estimate_config() {
  SonareRoomEstimateConfig cfg{};
  cfg.aspect_hint_lw = 7.0f / 5.0f;
  cfg.aspect_hint_lh = 7.0f / 3.0f;
  cfg.reference_absorption = 0.15f;
  cfg.prefer_eyring = 1;
  cfg.n_octave_bands = 0;
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
