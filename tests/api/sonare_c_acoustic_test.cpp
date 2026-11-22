#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "acoustic/rir_synthesizer.h"
#include "support/c_diagnostics.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_ACOUSTIC_SIM)
#include "support/alloc_guard.h"
#endif

namespace {

using sonare_test::has_diagnostic_code;
using sonare_test::last_diagnostics;

SonareRirSynthConfig valid_rir_config() {
  SonareRirSynthConfig cfg;
  sonare_rir_synth_config_init(&cfg);
  cfg.length_m = 7.0f;
  cfg.width_m = 5.0f;
  cfg.height_m = 3.0f;
  cfg.source.x = 1.5f;
  cfg.source.y = 1.0f;
  cfg.source.z = 1.2f;
  cfg.listener.x = 5.0f;
  cfg.listener.y = 4.0f;
  cfg.listener.z = 1.7f;
  cfg.absorption = 0.15f;
  cfg.max_sec = 0.1f;
  return cfg;
}

SonareRoomEstimateConfig valid_estimate_config() {
  SonareRoomEstimateConfig cfg;
  sonare_room_estimate_config_init(&cfg);
  cfg.aspect_hint_lw = 7.0f / 5.0f;
  cfg.aspect_hint_lh = 7.0f / 3.0f;
  return cfg;
}

}  // namespace

TEST_CASE("sonare acoustic C API synthesizes and frees RIRs", "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  SonareRirSynthResult result{};

  const SonareError err = sonare_synthesize_rir(&cfg, 48000, &result);

  REQUIRE(err == SONARE_OK);
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

TEST_CASE("sonare acoustic C API rejects non-finite and out-of-range RIR inputs",
          "[c_api][acoustic][numeric]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  for (float invalid : {nan, inf, -inf}) {
    SonareRirSynthConfig cfg = valid_rir_config();
    cfg.max_sec = invalid;
    SonareRirSynthResult result{};
    REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.rir == nullptr);
  }

  float absorption[] = {0.2f, nan};
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.band_absorption = absorption;
  cfg.band_absorption_count = 2;
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);

  cfg = valid_rir_config();
  cfg.mixing_time_ms = 10001.0f;
  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare acoustic C API refuses invalid RIR geometry with its diagnostic",
          "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.length_m = -1.0f;  // Finite ABI input; the acoustic validator diagnoses it.
  SonareRirSynthResult result{};

  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.rir == nullptr);
  REQUIRE(result.length == 0);
  const std::string detail = sonare_last_error_message();
  REQUIRE(detail.find("acoustic.invalid_dimensions") != std::string::npos);
  REQUIRE(detail.find("length_m must be finite, positive and within bounds") != std::string::npos);

  sonare_free_rir_synth_result(&result);
}

TEST_CASE("sonare acoustic C API publishes recoverable RIR diagnostics", "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.max_sec = 0.001f;
  SonareRirSynthResult result{};

  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_OK);
  REQUIRE(result.rir != nullptr);
  REQUIRE(std::string(sonare_last_error_message()).empty());
  REQUIRE(has_diagnostic_code("acoustic.rir_length_clamped"));

  sonare_free_rir_synth_result(&result);
}

TEST_CASE("sonare acoustic C API publishes every RIR diagnostic", "[c_api][acoustic]") {
  // A request that trips several at once.
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.max_sec = 0.005f;
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&cfg, 22050, &result) == SONARE_OK);

  const std::vector<SonareDiagnostic> diagnostics = last_diagnostics();
  REQUIRE(diagnostics.size() >= 2);
  for (const SonareDiagnostic& diagnostic : diagnostics) {
    CHECK(diagnostic.severity == SONARE_DIAGNOSTIC_WARNING);
    CHECK(!std::string(diagnostic.message).empty());
  }
  CHECK(has_diagnostic_code("acoustic.rir_length_clamped"));

  // Past the end is refused and the caller's struct is left as it was.
  SonareDiagnostic past;
  sonare_diagnostic_init(&past);
  CHECK(sonare_last_diagnostic(diagnostics.size(), &past) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(past.code).empty());

  // A later clean call clears the list, so a count read after it is that call's
  // rather than a slot nothing resets.
  SonareRirSynthConfig clean = valid_rir_config();
  clean.max_sec = 3.0f;
  SonareRirSynthResult clean_result{};
  REQUIRE(sonare_synthesize_rir(&clean, 22050, &clean_result) == SONARE_OK);
  CHECK(sonare_last_diagnostic_count() == 0);

  sonare_free_rir_synth_result(&result);
  sonare_free_rir_synth_result(&clean_result);
}

TEST_CASE("sonare acoustic C API honors the late-tail model selector", "[c_api][acoustic]") {
  // A more absorptive room makes Sabine and Eyring diverge (Eyring is preferred
  // for alpha-bar above ~0.2); the two models must produce different tails.
  SonareRirSynthConfig sabine = valid_rir_config();
  sabine.absorption = 0.4f;
  sabine.max_sec = 0.3f;
  sabine.prefer_eyring = 0;
  SonareRirSynthConfig eyring = sabine;
  eyring.prefer_eyring = 1;

  SonareRirSynthResult sabine_rir{};
  SonareRirSynthResult eyring_rir{};
  REQUIRE(sonare_synthesize_rir(&sabine, 48000, &sabine_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&eyring, 48000, &eyring_rir) == SONARE_OK);
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

TEST_CASE("sonare acoustic C API config init sets struct_size and the library defaults",
          "[c_api][acoustic]") {
  SonareRirSynthConfig rir;
  sonare_rir_synth_config_init(&rir);
  CHECK(rir.struct_size == sizeof(SonareRirSynthConfig));
  CHECK(rir.length_m == 7.0f);
  CHECK(rir.width_m == 5.0f);
  CHECK(rir.height_m == 3.0f);
  for (const SonareRoomPlacement& p : {rir.source, rir.listener}) {
    CHECK(std::isnan(p.x));
    CHECK(std::isnan(p.y));
    CHECK(std::isnan(p.z));
  }
  CHECK(rir.absorption == 0.2f);
  CHECK(rir.max_sec == 0.0f);
  CHECK(rir.mixing_time_ms == 0.0f);
  CHECK(rir.crossfade_ms == 5.0f);
  CHECK(rir.ism_order == 3);
  CHECK(rir.prefer_eyring == 1);
  CHECK(rir.seed == 1u);
  CHECK(rir.band_absorption == nullptr);
  CHECK(rir.band_absorption_count == 0);
  CHECK(rir.band_scattering == nullptr);
  CHECK(rir.band_scattering_count == 0);
  CHECK(rir.material_preset == SONARE_MATERIAL_PRESET_NONE);
  CHECK(rir.air_absorption_enabled == 1);
  CHECK(rir.air_temperature_c == 20.0f);
  CHECK(rir.air_humidity_percent == 50.0f);

  SonareRoomMorphConfig morph;
  sonare_room_morph_config_init(&morph);
  CHECK(morph.struct_size == sizeof(SonareRoomMorphConfig));
  CHECK(morph.length_m == 7.0f);
  for (const SonareRoomPlacement& p : {morph.source, morph.listener}) {
    CHECK(std::isnan(p.x));
    CHECK(std::isnan(p.y));
    CHECK(std::isnan(p.z));
  }
  CHECK(morph.absorption == 0.2f);
  CHECK(morph.source_tail_suppression == 0.5f);
  CHECK(morph.wet == 0.5f);
  CHECK(morph.max_sec == 0.0f);
  CHECK(morph.mixing_time_ms == 0.0f);
  CHECK(morph.crossfade_ms == 5.0f);
  CHECK(morph.ism_order == 3);
  CHECK(morph.prefer_eyring == 1);
  CHECK(morph.seed == 1u);
  CHECK(morph.material_preset == SONARE_MATERIAL_PRESET_NONE);
  CHECK(morph.air_absorption_enabled == 1);
  CHECK(morph.air_temperature_c == 20.0f);
  CHECK(morph.air_humidity_percent == 50.0f);
  // Not given: fitted to the room and placement at use.
  CHECK(std::isnan(morph.receiver_spacing_m));

  // The initialized default room synthesizes as it stands.
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&rir, 48000, &result) == SONARE_OK);
  CHECK(result.length > 0);
  sonare_free_rir_synth_result(&result);
}

namespace {

std::vector<float> synthesize_rir_samples(const SonareRirSynthConfig& cfg) {
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &result) == SONARE_OK);
  std::vector<float> rir(result.rir, result.rir + result.length);
  sonare_free_rir_synth_result(&result);
  return rir;
}

std::vector<float> room_morph_samples(const SonareRoomMorphConfig& cfg) {
  std::vector<float> input(2000, 0.0f);
  input[0] = 1.0f;
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length) ==
          SONARE_OK);
  std::vector<float> samples(out, out + out_length);
  sonare_free_floats(out);
  return samples;
}

// The default placement scaled per axis from the default 7 x 5 x 3 m room, as the library does.
std::pair<SonareRoomPlacement, SonareRoomPlacement> scaled_default(float l, float w, float h) {
  const float sx = l / 7.0f;
  const float sy = w / 5.0f;
  const float sz = h / 3.0f;
  return {{1.0f * sx, 1.0f * sy, 1.2f * sz}, {5.0f * sx, 4.0f * sy, 1.7f * sz}};
}

}  // namespace

TEST_CASE("sonare acoustic C API resolves an unset placement to the default scaled to the room",
          "[c_api][acoustic]") {
  SonareRirSynthConfig unset;
  sonare_rir_synth_config_init(&unset);
  unset.max_sec = 0.1f;

  SECTION("the default room keeps the default placement bit for bit") {
    SonareRirSynthConfig given = unset;
    given.source = {1.0f, 1.0f, 1.2f};
    given.listener = {5.0f, 4.0f, 1.7f};
    const std::vector<float> resolved = synthesize_rir_samples(unset);
    REQUIRE(!resolved.empty());
    CHECK(resolved == synthesize_rir_samples(given));
  }

  SECTION("a room smaller than the default placement is accepted with the placement inside") {
    unset.length_m = 4.0f;
    unset.width_m = 3.0f;
    unset.height_m = 2.6f;
    const auto [source, listener] = scaled_default(4.0f, 3.0f, 2.6f);
    for (const SonareRoomPlacement& p : {source, listener}) {
      CHECK(p.x > 0.0f);
      CHECK(p.x < 4.0f);
      CHECK(p.y > 0.0f);
      CHECK(p.y < 3.0f);
      CHECK(p.z > 0.0f);
      CHECK(p.z < 2.6f);
    }
    SonareRirSynthConfig given = unset;
    given.source = source;
    given.listener = listener;
    const std::vector<float> resolved = synthesize_rir_samples(unset);
    REQUIRE(!resolved.empty());
    CHECK(resolved == synthesize_rir_samples(given));

    SonareRoomMorphConfig morph;
    sonare_room_morph_config_init(&morph);
    morph.length_m = 4.0f;
    morph.width_m = 3.0f;
    morph.height_m = 2.6f;
    morph.max_sec = 0.1f;
    SonareRoomMorphConfig morph_given = morph;
    morph_given.source = source;
    morph_given.listener = listener;
    const std::vector<float> morphed = room_morph_samples(morph);
    REQUIRE(!morphed.empty());
    CHECK(morphed == room_morph_samples(morph_given));
  }

  SECTION("an explicit position is read as given") {
    SonareRirSynthConfig given = unset;
    given.source = {2.0f, 2.0f, 1.5f};
    CHECK(synthesize_rir_samples(given) != synthesize_rir_samples(unset));
    SonareRirSynthConfig listener_only = unset;
    listener_only.listener = {5.0f, 4.0f, 1.7f};
    CHECK(synthesize_rir_samples(listener_only) == synthesize_rir_samples(unset));
  }
}

TEST_CASE("sonare acoustic C API refuses a partly NaN placement by component",
          "[c_api][acoustic]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::string predicate = " must be finite and in [-3.4028235e38, 3.4028235e38]";

  SonareRirSynthConfig rir;
  sonare_rir_synth_config_init(&rir);
  rir.source = {2.0f, nan, 1.0f};
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&rir, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "config.source.y" + predicate);
  CHECK(result.rir == nullptr);

  SonareRoomMorphConfig morph;
  sonare_room_morph_config_init(&morph);
  morph.listener = {nan, nan, 1.0f};
  std::vector<float> input(64, 0.0f);
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &morph, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "config.listener.x" + predicate);
  CHECK(out == nullptr);
}

TEST_CASE("sonare acoustic C API refuses a room config below its struct_size",
          "[c_api][acoustic]") {
  SonareRirSynthConfig rir = valid_rir_config();
  rir.struct_size = 0;
  SonareRirSynthResult result{};
  REQUIRE(sonare_synthesize_rir(&rir, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result.rir == nullptr);
  CHECK(std::string(sonare_last_error_message())
            .find("config.struct_size must be set by sonare_rir_synth_config_init") !=
        std::string::npos);
  rir.struct_size = sizeof(SonareRirSynthConfig) - 1;
  REQUIRE(sonare_synthesize_rir(&rir, 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);

  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;
  SonareRoomMorphConfig morph;
  sonare_room_morph_config_init(&morph);
  morph.max_sec = 0.1f;
  morph.struct_size = sizeof(SonareRoomMorphConfig) - 1;
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &morph, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message())
            .find("config.struct_size must be set by sonare_room_morph_config_init") !=
        std::string::npos);
  CHECK(out == nullptr);
  float* stereo_left = nullptr;
  float* stereo_right = nullptr;
  size_t stereo_length = 0;
  REQUIRE(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &morph,
                                   &stereo_left, &stereo_right,
                                   &stereo_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(stereo_left == nullptr);
}

TEST_CASE("sonare acoustic C API reads seed 0 as a seed", "[c_api][acoustic]") {
  // Seed 0 is an ordinary SplitMix64 seed: it renders its own deterministic tail, distinct
  // from the default seed 1.
  SonareRirSynthConfig base = valid_rir_config();
  base.absorption = 0.3f;
  base.max_sec = 0.4f;  // long enough that the seeded late tail contributes

  SonareRirSynthConfig zero_seed = base;
  zero_seed.seed = 0u;
  SonareRirSynthConfig one_seed = base;
  one_seed.seed = 1u;

  SonareRirSynthResult zero_rir{};
  SonareRirSynthResult zero_again{};
  SonareRirSynthResult one_rir{};
  REQUIRE(sonare_synthesize_rir(&zero_seed, 48000, &zero_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&zero_seed, 48000, &zero_again) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&one_seed, 48000, &one_rir) == SONARE_OK);
  REQUIRE(zero_rir.length > 0);
  REQUIRE(zero_rir.length == zero_again.length);
  for (size_t i = 0; i < zero_rir.length; ++i) REQUIRE(zero_rir.rir[i] == zero_again.rir[i]);

  bool differs_from_one = zero_rir.length != one_rir.length;
  const size_t common = std::min(zero_rir.length, one_rir.length);
  for (size_t i = 0; i < common && !differs_from_one; ++i) {
    differs_from_one = std::abs(zero_rir.rir[i] - one_rir.rir[i]) > 1e-6f;
  }
  REQUIRE(differs_from_one);

  sonare_free_rir_synth_result(&zero_rir);
  sonare_free_rir_synth_result(&zero_again);
  sonare_free_rir_synth_result(&one_rir);
}

TEST_CASE("sonare acoustic C API publishes the morph's own RIR diagnostics", "[c_api][acoustic]") {
  // The morph synthesizes its target RIR with the same code synthesize_rir uses,
  // so the same clamp fires -- and it used to be dropped, leaving a morph through
  // a room the caller did not ask for indistinguishable from one through the room
  // they did.
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig cfg;
  sonare_room_morph_config_init(&cfg);
  cfg.length_m = 12.0f;
  cfg.width_m = 9.0f;
  cfg.height_m = 5.0f;
  cfg.source.x = 2.0f;
  cfg.source.y = 2.0f;
  cfg.source.z = 1.5f;
  cfg.listener.x = 8.0f;
  cfg.listener.y = 6.0f;
  cfg.listener.z = 1.7f;
  cfg.absorption = 0.4f;
  cfg.wet = 1.0f;
  cfg.max_sec = 0.3f;
  cfg.seed = 1u;
  // Well above the synthesizer's safe maximum rather than one past it: this case
  // is about the channel the warning travels on, not about where the clamp sits.
  cfg.ism_order = 99;

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length) ==
          SONARE_OK);
  REQUIRE(out_length > 0);
  REQUIRE(has_diagnostic_code("acoustic.ism_order_clamped"));
  sonare_free_floats(out);

  // An order the synthesizer does not clamp leaves the channel clear, so the
  // warning above is the clamp's rather than a slot nothing ever resets.
  cfg.ism_order = 2;
  out = nullptr;
  out_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length) ==
          SONARE_OK);
  REQUIRE_FALSE(has_diagnostic_code("acoustic.ism_order_clamped"));
  sonare_free_floats(out);
}

TEST_CASE("sonare acoustic C API morph honours the mixing-time / crossfade controls",
          "[c_api][acoustic]") {
  // A different crossfade width must change the rendered tail, proving the morph
  // now plumbs mixing_time_ms / crossfade_ms. Use a fixed Eyring model
  // so only the crossfade differs.
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig base;
  sonare_room_morph_config_init(&base);
  base.length_m = 12.0f;
  base.width_m = 9.0f;
  base.height_m = 5.0f;
  base.source.x = 2.0f;
  base.source.y = 2.0f;
  base.source.z = 1.5f;
  base.listener.x = 8.0f;
  base.listener.y = 6.0f;
  base.listener.z = 1.7f;
  base.absorption = 0.2f;
  base.source_tail_suppression = 0.0f;
  base.wet = 1.0f;
  base.max_sec = 0.3f;
  base.ism_order = 3;
  base.seed = 1u;
  base.prefer_eyring = 1;
  base.mixing_time_ms = 20.0f;
  base.crossfade_ms = 2.0f;

  SonareRoomMorphConfig wide = base;
  wide.crossfade_ms = 30.0f;

  float* narrow_out = nullptr;
  float* wide_out = nullptr;
  size_t narrow_len = 0;
  size_t wide_len = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &base, &narrow_out, &narrow_len) ==
          SONARE_OK);
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &wide, &wide_out, &wide_len) ==
          SONARE_OK);
  REQUIRE(narrow_out != nullptr);
  REQUIRE(wide_out != nullptr);

  bool differs = narrow_len != wide_len;
  const size_t common = narrow_len < wide_len ? narrow_len : wide_len;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(narrow_out[i] - wide_out[i]) > 1e-6f;
  }
  REQUIRE(differs);

  sonare_free_floats(narrow_out);
  sonare_free_floats(wide_out);
}

TEST_CASE("sonare acoustic C API refuses a source outside the room", "[c_api][acoustic]") {
  SonareRirSynthConfig cfg = valid_rir_config();
  cfg.source.x = 99.0f;
  SonareRirSynthResult result{};

  const SonareError err = sonare_synthesize_rir(&cfg, 48000, &result);

  REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("acoustic.source_outside_room") !=
          std::string::npos);
  REQUIRE(result.length == 0);
  REQUIRE(result.rir == nullptr);
  sonare_free_rir_synth_result(&result);
}

TEST_CASE("sonare acoustic C API estimates a room from a synthesized RIR", "[c_api][acoustic]") {
  SonareRirSynthConfig synth_cfg = valid_rir_config();
  synth_cfg.max_sec = 0.0f;
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
  REQUIRE(estimate.band_absorption != nullptr);
  REQUIRE(estimate.rt60_bands != nullptr);

  sonare_free_room_estimate(&estimate);
  REQUIRE(estimate.band_absorption == nullptr);
  REQUIRE(estimate.rt60_bands == nullptr);
  REQUIRE(estimate.band_count == 0);
  sonare_free_rir_synth_result(&rir);
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_ACOUSTIC_SIM)
TEST_CASE("sonare acoustic room estimates publish atomically on allocation failure",
          "[c_api][acoustic][allocation]") {
  SonareRirSynthConfig synth_cfg = valid_rir_config();
  synth_cfg.max_sec = 0.1f;
  SonareRirSynthResult rir{};
  REQUIRE(sonare_synthesize_rir(&synth_cfg, 48000, &rir) == SONARE_OK);
  REQUIRE(rir.rir != nullptr);
  REQUIRE(rir.length > 0);

  const SonareRoomEstimateConfig estimate_cfg = valid_estimate_config();
  SonareRoomEstimate warm{};
  REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &warm) == SONARE_OK);
  REQUIRE(warm.band_count > 0);
  REQUIRE(warm.band_absorption != nullptr);
  REQUIRE(warm.rt60_bands != nullptr);
  sonare_free_room_estimate(&warm);

  SonareRoomEstimate first{};
  SonareError first_error = SONARE_ERROR_UNKNOWN;
  std::size_t first_allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    first_error = sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &first);
    first_allocations = guard.count();
  }
  REQUIRE(first_error == SONARE_OK);
  REQUIRE(first_allocations > 0);
  REQUIRE(first.band_count > 0);
  REQUIRE(first.band_absorption != nullptr);
  REQUIRE(first.rt60_bands != nullptr);
  sonare_free_room_estimate(&first);

  SonareRoomEstimate second{};
  SonareError second_error = SONARE_ERROR_UNKNOWN;
  std::size_t second_allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    second_error = sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &second);
    second_allocations = guard.count();
  }
  REQUIRE(second_error == SONARE_OK);
  REQUIRE(second_allocations == first_allocations);
  REQUIRE(second_allocations > 0);
  REQUIRE(second.band_count > 0);
  REQUIRE(second.band_absorption != nullptr);
  REQUIRE(second.rt60_bands != nullptr);
  sonare_free_room_estimate(&second);

  SonareRoomEstimate failed{};
  failed.volume = 1.0f;
  failed.length_m = 2.0f;
  failed.width_m = 3.0f;
  failed.height_m = 4.0f;
  failed.drr_db = 5.0f;
  failed.confidence = 6.0f;
  failed.band_absorption = reinterpret_cast<float*>(static_cast<std::uintptr_t>(1));
  failed.rt60_bands = reinterpret_cast<float*>(static_cast<std::uintptr_t>(2));
  failed.band_count = 7;
  SonareError failed_error = SONARE_OK;
  {
    // The final allocation is the RT60 array. The guard is scoped to the C
    // call, so Catch2's assertions cannot consume the injected failure.
    sonare::test::AllocationFailureAtGuard guard(second_allocations);
    failed_error = sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &failed);
  }
  REQUIRE(failed_error == SONARE_ERROR_OUT_OF_MEMORY);
  // REQUIRE aborts before the free below if a future implementation leaves a
  // sentinel or partially published allocation behind; neither is safe to
  // pass to the C free routine.
  REQUIRE(failed.volume == 0.0f);
  REQUIRE(failed.length_m == 0.0f);
  REQUIRE(failed.width_m == 0.0f);
  REQUIRE(failed.height_m == 0.0f);
  REQUIRE(failed.drr_db == 0.0f);
  REQUIRE(failed.confidence == 0.0f);
  REQUIRE(failed.band_absorption == nullptr);
  REQUIRE(failed.rt60_bands == nullptr);
  REQUIRE(failed.band_count == 0);
  sonare_free_room_estimate(&failed);
  CHECK(failed.band_absorption == nullptr);
  CHECK(failed.rt60_bands == nullptr);
  CHECK(failed.band_count == 0);

  sonare_free_rir_synth_result(&rir);
}
#endif

TEST_CASE("sonare acoustic C API rejects invalid estimate priors and morph geometry",
          "[c_api][acoustic][numeric]") {
  std::vector<float> input(2000, 0.0f);
  input[0] = 1.0f;

  SonareRoomEstimateConfig estimate_cfg = valid_estimate_config();
  estimate_cfg.reference_absorption = std::numeric_limits<float>::quiet_NaN();
  SonareRoomEstimate estimate{};
  REQUIRE(sonare_estimate_room(input.data(), input.size(), 48000, &estimate_cfg, &estimate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(estimate.band_absorption == nullptr);
  REQUIRE(estimate.rt60_bands == nullptr);

  SonareRoomMorphConfig morph;
  sonare_room_morph_config_init(&morph);
  morph.length_m = 5.0f;
  morph.width_m = 4.0f;
  morph.height_m = 3.0f;
  morph.source.x = 99.0f;
  morph.source.y = 1.0f;
  morph.source.z = 1.0f;
  morph.listener.x = 3.0f;
  morph.listener.y = 2.0f;
  morph.listener.z = 1.0f;
  morph.absorption = 0.2f;
  morph.source_tail_suppression = 0.5f;
  morph.wet = 0.5f;
  morph.ism_order = 3;
  float* output = nullptr;
  size_t output_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &morph, &output, &output_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(output == nullptr);
  REQUIRE(output_length == 0);

  morph.source.x = 1.0f;
  morph.wet = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &morph, &output, &output_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(output == nullptr);
}

TEST_CASE("sonare acoustic C API room morph appends a target tail", "[c_api][acoustic]") {
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig cfg;
  sonare_room_morph_config_init(&cfg);
  cfg.length_m = 12.0f;
  cfg.width_m = 9.0f;
  cfg.height_m = 5.0f;
  cfg.source.x = 2.0f;
  cfg.source.y = 2.0f;
  cfg.source.z = 1.5f;
  cfg.listener.x = 8.0f;
  cfg.listener.y = 6.0f;
  cfg.listener.z = 1.7f;
  cfg.absorption = 0.08f;
  cfg.source_tail_suppression = 0.5f;
  cfg.wet = 0.7f;
  cfg.max_sec = 0.1f;
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

TEST_CASE("sonare acoustic C API uses per-band wall absorption", "[c_api][acoustic]") {
  // A per-octave-band absorption array (high bands far more absorptive) must
  // override the scalar `absorption` and yield a different RIR than the scalar
  // path, while a zeroed/NULL bands array preserves the scalar back-compat path.
  const float bands[6] = {0.05f, 0.05f, 0.10f, 0.30f, 0.55f, 0.70f};

  SonareRirSynthConfig banded = valid_rir_config();
  banded.absorption = 0.05f;  // scalar that would be used if bands were ignored
  banded.band_absorption = bands;
  banded.band_absorption_count = 6;
  banded.material_preset = SONARE_MATERIAL_PRESET_NONE;  // bands apply
  banded.max_sec = 0.3f;

  SonareRirSynthConfig scalar = banded;
  scalar.band_absorption = nullptr;  // fall back to the scalar 0.05 absorption
  scalar.band_absorption_count = 0;

  SonareRirSynthResult banded_rir{};
  SonareRirSynthResult scalar_rir{};
  REQUIRE(sonare_synthesize_rir(&banded, 48000, &banded_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&scalar, 48000, &scalar_rir) == SONARE_OK);
  REQUIRE(banded_rir.length > 0);
  REQUIRE(scalar_rir.length > 0);

  bool differs = banded_rir.length != scalar_rir.length;
  const size_t common =
      banded_rir.length < scalar_rir.length ? banded_rir.length : scalar_rir.length;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(banded_rir.rir[i] - scalar_rir.rir[i]) > 1e-6f;
  }
  REQUIRE(differs);  // the per-band array genuinely changed the synthesis

  sonare_free_rir_synth_result(&banded_rir);
  sonare_free_rir_synth_result(&scalar_rir);
}

TEST_CASE("sonare acoustic C API uses per-band wall scattering", "[c_api][acoustic]") {
  const float absorption[6] = {0.12f, 0.12f, 0.12f, 0.12f, 0.12f, 0.12f};
  const float smooth_scattering[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  const float rough_scattering[6] = {0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f};

  SonareRirSynthConfig smooth = valid_rir_config();
  smooth.band_absorption = absorption;
  smooth.band_absorption_count = 6;
  smooth.band_scattering = smooth_scattering;
  smooth.band_scattering_count = 6;
  smooth.material_preset = SONARE_MATERIAL_PRESET_NONE;
  smooth.max_sec = 0.3f;
  smooth.mixing_time_ms = 0.0f;

  SonareRirSynthConfig rough = smooth;
  rough.band_scattering = rough_scattering;

  SonareRirSynthResult smooth_rir{};
  SonareRirSynthResult rough_rir{};
  REQUIRE(sonare_synthesize_rir(&smooth, 48000, &smooth_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&rough, 48000, &rough_rir) == SONARE_OK);
  REQUIRE(smooth_rir.length > 0);
  REQUIRE(rough_rir.length > 0);

  bool differs = smooth_rir.length != rough_rir.length;
  const size_t common = smooth_rir.length < rough_rir.length ? smooth_rir.length : rough_rir.length;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(smooth_rir.rir[i] - rough_rir.rir[i]) > 1e-6f;
  }
  REQUIRE(differs);

  sonare_free_rir_synth_result(&smooth_rir);
  sonare_free_rir_synth_result(&rough_rir);
}

TEST_CASE("sonare acoustic C API selects a material preset", "[c_api][acoustic]") {
  // A preset (non-zero) wins over both the per-band array and the scalar, so two
  // distinct presets must yield distinct RIRs from the same geometry.
  SonareRirSynthConfig curtain = valid_rir_config();
  curtain.material_preset = SONARE_MATERIAL_PRESET_CURTAIN;  // heavy HF absorption
  curtain.max_sec = 0.3f;
  SonareRirSynthConfig concrete = curtain;
  concrete.material_preset = SONARE_MATERIAL_PRESET_CONCRETE;  // near-rigid

  SonareRirSynthResult curtain_rir{};
  SonareRirSynthResult concrete_rir{};
  REQUIRE(sonare_synthesize_rir(&curtain, 48000, &curtain_rir) == SONARE_OK);
  REQUIRE(sonare_synthesize_rir(&concrete, 48000, &concrete_rir) == SONARE_OK);
  REQUIRE(curtain_rir.length > 0);
  REQUIRE(concrete_rir.length > 0);

  // Concrete (near-rigid) and curtain (heavy HF absorption) imprint very
  // different reflection coefficients and decay, so the synthesized RIRs differ
  // in CONTENT. Compare sample-by-sample (max_sec caps both to the same
  // length, so a length comparison alone would be vacuous).
  bool differs = concrete_rir.length != curtain_rir.length;
  const size_t common =
      concrete_rir.length < curtain_rir.length ? concrete_rir.length : curtain_rir.length;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(concrete_rir.rir[i] - curtain_rir.rir[i]) > 1e-6f;
  }
  REQUIRE(differs);

  sonare_free_rir_synth_result(&curtain_rir);
  sonare_free_rir_synth_result(&concrete_rir);
}

TEST_CASE("sonare acoustic C API morph honours the new tail controls", "[c_api][acoustic]") {
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig base;
  sonare_room_morph_config_init(&base);
  base.length_m = 12.0f;
  base.width_m = 9.0f;
  base.height_m = 5.0f;
  base.source.x = 2.0f;
  base.source.y = 2.0f;
  base.source.z = 1.5f;
  base.listener.x = 8.0f;
  base.listener.y = 6.0f;
  base.listener.z = 1.7f;
  base.absorption = 0.4f;  // absorptive enough that Sabine/Eyring diverge
  base.source_tail_suppression = 0.0f;
  base.wet = 1.0f;
  base.max_sec = 0.3f;
  base.ism_order = 3;
  base.seed = 1u;
  base.prefer_eyring = 0;
  base.mixing_time_ms = 0.0f;  // auto

  SonareRoomMorphConfig eyring = base;
  eyring.prefer_eyring = 1;

  float* sab_out = nullptr;
  float* eyr_out = nullptr;
  size_t sab_len = 0;
  size_t eyr_len = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &base, &sab_out, &sab_len) ==
          SONARE_OK);
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &eyring, &eyr_out, &eyr_len) ==
          SONARE_OK);
  REQUIRE(sab_out != nullptr);
  REQUIRE(eyr_out != nullptr);

  // The late-tail model now propagates through the morph, so the two renders
  // differ (length and/or samples).
  bool differs = sab_len != eyr_len;
  const size_t common = sab_len < eyr_len ? sab_len : eyr_len;
  for (size_t i = 0; i < common && !differs; ++i) {
    differs = std::abs(sab_out[i] - eyr_out[i]) > 1e-6f;
  }
  REQUIRE(differs);

  sonare_free_floats(sab_out);
  sonare_free_floats(eyr_out);
}

namespace {

// A hall big enough for the atmospheric term to bias the late tail clearly,
// capped short so the case stays well under the slow-test threshold. The mixing
// time is auto (~sqrt(V) ms, about 100 ms here), so 0.5 s leaves a substantial
// late tail for the air term to act on.
SonareRirSynthConfig air_hall_config() {
  SonareRirSynthConfig cfg;
  sonare_rir_synth_config_init(&cfg);
  cfg.length_m = 30.0f;
  cfg.width_m = 24.0f;
  cfg.height_m = 15.0f;
  cfg.source.x = 3.0f;
  cfg.source.y = 3.0f;
  cfg.source.z = 1.5f;
  cfg.listener.x = 10.0f;
  cfg.listener.y = 8.0f;
  cfg.listener.z = 1.7f;
  cfg.absorption = 0.2f;
  cfg.max_sec = 0.5f;
  cfg.ism_order = 2;
  cfg.seed = 3u;
  return cfg;
}

SonareRirSynthResult synthesized(const SonareRirSynthConfig& cfg) {
  SonareRirSynthResult out{};
  REQUIRE(sonare_synthesize_rir(&cfg, 48000, &out) == SONARE_OK);
  REQUIRE(out.length > 0);
  return out;
}

bool same_rir(const SonareRirSynthResult& a, const SonareRirSynthResult& b) {
  if (a.length != b.length) return false;
  for (size_t i = 0; i < a.length; ++i) {
    if (std::abs(a.rir[i] - b.rir[i]) > 1e-6f) return false;
  }
  return true;
}

// Energy from `first` onward, i.e. past the early reflections, where the
// statistical tail (the only part the air term reshapes) lives.
double tail_energy(const SonareRirSynthResult& r, size_t first) {
  double sum = 0.0;
  for (size_t i = first; i < r.length; ++i) sum += double(r.rir[i]) * double(r.rir[i]);
  return sum;
}

}  // namespace

TEST_CASE("sonare acoustic C API routes air absorption into the synthesized tail",
          "[c_api][acoustic]") {
  // Before these fields existed, the ISO 9613-1 atmospheric term was reachable
  // only through the streaming insert ("effects.reverb.room"); the offline
  // entry point could not request it at all. Each section below pins one of the
  // three fields individually, so dropping any single one on the way to the
  // core fails here rather than hiding behind the other two.
  const SonareRirSynthConfig base = air_hall_config();

  SonareRirSynthConfig geometry_only = base;
  geometry_only.air_absorption_enabled = 0;
  SonareRirSynthConfig iso = base;
  iso.air_absorption_enabled = 1;
  iso.air_temperature_c = 20.0f;
  iso.air_humidity_percent = 50.0f;

  SonareRirSynthResult off = synthesized(geometry_only);
  SonareRirSynthResult on = synthesized(iso);

  SECTION("enabling it changes the tail and removes energy from it") {
    REQUIRE_FALSE(same_rir(off, on));
    // Directional check: air absorption can only take energy out of the tail.
    // The crossover sits near 100 ms; sample 9600 (200 ms) is comfortably past
    // it, so this compares statistical tail against statistical tail.
    const double off_energy = tail_energy(off, 9600);
    const double on_energy = tail_energy(on, 9600);
    CAPTURE(off_energy, on_energy);
    REQUIRE(on_energy < off_energy);
  }

  SECTION("the initialized climate is the ISO reference") {
    SonareRirSynthConfig initialized = base;
    initialized.air_absorption_enabled = 1;
    SonareRirSynthResult implicit_iso = synthesized(initialized);
    REQUIRE(same_rir(implicit_iso, on));
    sonare_free_rir_synth_result(&implicit_iso);
  }

  SECTION("a literal 0 degC / 0 % is a climate of its own") {
    SonareRirSynthConfig freezing = iso;
    freezing.air_temperature_c = 0.0f;
    freezing.air_humidity_percent = 0.0f;
    SonareRirSynthResult freezing_rir = synthesized(freezing);
    REQUIRE_FALSE(same_rir(freezing_rir, on));
    REQUIRE_FALSE(same_rir(freezing_rir, off));
    sonare_free_rir_synth_result(&freezing_rir);
  }

  SECTION("temperature and humidity are each read on their own") {
    SonareRirSynthConfig warm = iso;
    warm.air_temperature_c = 35.0f;
    SonareRirSynthConfig humid = iso;
    humid.air_humidity_percent = 90.0f;
    SonareRirSynthResult warm_rir = synthesized(warm);
    SonareRirSynthResult humid_rir = synthesized(humid);
    REQUIRE_FALSE(same_rir(warm_rir, on));
    REQUIRE_FALSE(same_rir(humid_rir, on));
    REQUIRE_FALSE(same_rir(warm_rir, humid_rir));
    sonare_free_rir_synth_result(&warm_rir);
    sonare_free_rir_synth_result(&humid_rir);
  }

  SECTION("the climate is ignored while the flag is off") {
    SonareRirSynthConfig off_but_set = base;
    off_but_set.air_absorption_enabled = 0;
    off_but_set.air_temperature_c = 35.0f;
    off_but_set.air_humidity_percent = 90.0f;
    SonareRirSynthResult ignored = synthesized(off_but_set);
    REQUIRE(same_rir(ignored, off));
    sonare_free_rir_synth_result(&ignored);
  }

  sonare_free_rir_synth_result(&off);
  sonare_free_rir_synth_result(&on);
}

TEST_CASE("sonare acoustic C API reports an implausible air climate", "[c_api][acoustic]") {
  SECTION("RIR synthesis refuses it with the diagnostic's text") {
    SonareRirSynthConfig cfg = air_hall_config();
    cfg.air_absorption_enabled = 1;
    cfg.air_temperature_c = -500.0f;  // below absolute zero
    SonareRirSynthResult out{};
    REQUIRE(sonare_synthesize_rir(&cfg, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.length == 0);
    const char* message = sonare_last_error_message();
    REQUIRE(message != nullptr);
    REQUIRE(std::string(message).find("acoustic.invalid_air_absorption") != std::string::npos);
    sonare_free_rir_synth_result(&out);
  }

  SECTION("the morph rejects it, because that path validates instead of diagnosing") {
    std::vector<float> input(4000, 0.0f);
    input[0] = 1.0f;
    SonareRoomMorphConfig cfg;
    sonare_room_morph_config_init(&cfg);
    cfg.length_m = 12.0f;
    cfg.width_m = 9.0f;
    cfg.height_m = 5.0f;
    cfg.source.x = 2.0f;
    cfg.source.y = 2.0f;
    cfg.source.z = 1.5f;
    cfg.listener.x = 8.0f;
    cfg.listener.y = 6.0f;
    cfg.listener.z = 1.7f;
    cfg.absorption = 0.08f;
    cfg.source_tail_suppression = 0.5f;
    cfg.wet = 0.7f;
    cfg.max_sec = 0.1f;
    cfg.ism_order = 2;
    cfg.air_absorption_enabled = 1;
    cfg.air_humidity_percent = 150.0f;  // outside [0, 100]
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
  }
}

TEST_CASE("sonare acoustic C API routes air absorption into the morph target room",
          "[c_api][acoustic]") {
  std::vector<float> input(4000, 0.0f);
  input[0] = 1.0f;

  SonareRoomMorphConfig base;
  sonare_room_morph_config_init(&base);
  base.length_m = 30.0f;
  base.width_m = 24.0f;
  base.height_m = 15.0f;
  base.source.x = 3.0f;
  base.source.y = 3.0f;
  base.source.z = 1.5f;
  base.listener.x = 10.0f;
  base.listener.y = 8.0f;
  base.listener.z = 1.7f;
  base.absorption = 0.2f;
  base.source_tail_suppression = 0.5f;
  base.wet = 1.0f;
  base.max_sec = 0.5f;
  base.ism_order = 2;
  base.seed = 3u;

  const auto morph = [&](const SonareRoomMorphConfig& cfg) {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(out != nullptr);
    std::vector<float> copy(out, out + out_length);
    sonare_free_floats(out);
    return copy;
  };

  SonareRoomMorphConfig geometry_only = base;
  geometry_only.air_absorption_enabled = 0;
  SonareRoomMorphConfig iso = base;
  iso.air_absorption_enabled = 1;
  iso.air_temperature_c = 20.0f;
  iso.air_humidity_percent = 50.0f;
  SonareRoomMorphConfig initialized = base;
  initialized.air_absorption_enabled = 1;
  SonareRoomMorphConfig freezing = iso;
  freezing.air_temperature_c = 0.0f;
  freezing.air_humidity_percent = 0.0f;

  const std::vector<float> off = morph(geometry_only);
  const std::vector<float> on = morph(iso);
  const std::vector<float> implicit_iso = morph(initialized);
  const std::vector<float> literal_zero = morph(freezing);

  REQUIRE(off.size() == on.size());
  bool differs = false;
  for (size_t i = 0; i < off.size() && !differs; ++i) {
    differs = std::abs(off[i] - on[i]) > 1e-6f;
  }
  REQUIRE(differs);
  REQUIRE(implicit_iso == on);
  REQUIRE(literal_zero != on);
}

TEST_CASE("sonare acoustic C API air absorption selector", "[c_api][acoustic]") {
  SECTION("the initialized selector equals an explicit ON") {
    SonareRirSynthConfig implicit_cfg = air_hall_config();
    SonareRirSynthConfig on_cfg = implicit_cfg;
    on_cfg.air_absorption_enabled = 1;
    SonareRirSynthResult implicit_rir = synthesized(implicit_cfg);
    SonareRirSynthResult on_rir = synthesized(on_cfg);
    REQUIRE(same_rir(implicit_rir, on_rir));
    sonare_free_rir_synth_result(&implicit_rir);
    sonare_free_rir_synth_result(&on_rir);
  }
}

TEST_CASE("sonare acoustic C API estimate config init carries the library defaults",
          "[c_api][acoustic]") {
  SonareRoomEstimateConfig cfg;
  sonare_room_estimate_config_init(&cfg);
  CHECK(cfg.struct_size == sizeof(SonareRoomEstimateConfig));
  CHECK(cfg.aspect_hint_lw == 1.0f);
  CHECK(cfg.aspect_hint_lh == 1.0f);
  CHECK(cfg.reference_absorption == 0.15f);
  CHECK(cfg.min_decay_db == 30.0f);
  CHECK(cfg.noise_floor_margin_db == 10.0f);
  CHECK(cfg.prefer_eyring == 1);
  CHECK(cfg.n_octave_bands == 6);
  CHECK(cfg.mode == SONARE_ACOUSTIC_MODE_AUTO);
  sonare_room_estimate_config_init(nullptr);
}

TEST_CASE(
    "sonare acoustic C API estimate config refuses a zero struct_size and every 0 outside "
    "its domain",
    "[c_api][acoustic]") {
  SonareRirSynthConfig synth_cfg = valid_rir_config();
  synth_cfg.max_sec = 0.0f;
  SonareRirSynthResult rir{};
  REQUIRE(sonare_synthesize_rir(&synth_cfg, 48000, &rir) == SONARE_OK);
  REQUIRE(rir.length > 0);

  const auto refused_by = [&](const SonareRoomEstimateConfig& cfg, const char* subject) {
    SonareRoomEstimate out{};
    REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &cfg, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find(subject) != std::string::npos);
  };

  SonareRoomEstimateConfig zeroed{};
  refused_by(zeroed, "config.struct_size must be set by sonare_room_estimate_config_init");

  SonareRoomEstimateConfig cfg = valid_estimate_config();
  cfg.aspect_hint_lw = 0.0f;
  refused_by(cfg, "config.aspect_hint_lw");
  cfg = valid_estimate_config();
  cfg.aspect_hint_lh = 0.0f;
  refused_by(cfg, "config.aspect_hint_lh");
  cfg = valid_estimate_config();
  cfg.reference_absorption = 0.0f;
  refused_by(cfg, "config.reference_absorption");
  cfg.reference_absorption = 1.0f;
  refused_by(cfg, "config.reference_absorption");
  cfg = valid_estimate_config();
  cfg.min_decay_db = 0.0f;
  refused_by(cfg, "config.min_decay_db");
  cfg = valid_estimate_config();
  cfg.n_octave_bands = 0;
  refused_by(cfg, "config.n_octave_bands");

  // 0 dB is a real noise-floor margin, so it is read literally.
  cfg = valid_estimate_config();
  cfg.noise_floor_margin_db = 0.0f;
  SonareRoomEstimate ok{};
  CHECK(sonare_estimate_room(rir.rir, rir.length, 48000, &cfg, &ok) == SONARE_OK);

  // The default prior and the bound it is clamped to by the core give rooms far apart.
  SonareRoomEstimateConfig def = valid_estimate_config();
  SonareRoomEstimate def_estimate{};
  REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &def, &def_estimate) == SONARE_OK);
  SonareRoomEstimateConfig floored = def;
  floored.reference_absorption = 0.01f;
  SonareRoomEstimate floored_estimate{};
  REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &floored, &floored_estimate) ==
          SONARE_OK);
  REQUIRE(def_estimate.volume > 0.0f);
  CHECK(floored_estimate.volume < def_estimate.volume / 1000.0f);

  sonare_free_room_estimate(&ok);
  sonare_free_room_estimate(&def_estimate);
  sonare_free_room_estimate(&floored_estimate);
  sonare_free_rir_synth_result(&rir);
}

TEST_CASE("sonare acoustic C API applies wall scattering without per-band absorption",
          "[c_api][acoustic]") {
  // `band_scattering` is documented as optional and applied band-wise when
  // present, with no dependency on `band_absorption`. It used to reach the
  // material only alongside a per-band absorption array, so a caller asking for
  // rough walls on a scalar-absorption or preset room got a perfectly specular
  // one with no error and no diagnostic.
  const float rough[6] = {0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f};

  SonareRirSynthConfig smooth = valid_rir_config();
  smooth.max_sec = 0.3f;
  smooth.mixing_time_ms = 0.0f;
  SonareRirSynthConfig scattered = smooth;
  scattered.band_scattering = rough;
  scattered.band_scattering_count = 6;

  auto synthesize = [](const SonareRirSynthConfig& cfg) {
    SonareRirSynthResult out{};
    REQUIRE(sonare_synthesize_rir(&cfg, 48000, &out) == SONARE_OK);
    REQUIRE(out.length > 0);
    return out;
  };
  auto differs = [](const SonareRirSynthResult& a, const SonareRirSynthResult& b) {
    if (a.length != b.length) return true;
    for (size_t i = 0; i < a.length; ++i) {
      if (std::abs(a.rir[i] - b.rir[i]) > 1e-6f) return true;
    }
    return false;
  };

  // Scalar absorption path.
  SonareRirSynthResult scalar_smooth = synthesize(smooth);
  SonareRirSynthResult scalar_rough = synthesize(scattered);
  CHECK(differs(scalar_smooth, scalar_rough));

  // Preset path: a preset carries its own material, and the caller's scattering
  // array still applies on top of it.
  SonareRirSynthConfig preset_smooth = smooth;
  preset_smooth.material_preset = SONARE_MATERIAL_PRESET_WOOD;
  SonareRirSynthConfig preset_rough = preset_smooth;
  preset_rough.band_scattering = rough;
  preset_rough.band_scattering_count = 6;
  SonareRirSynthResult preset_smooth_rir = synthesize(preset_smooth);
  SonareRirSynthResult preset_rough_rir = synthesize(preset_rough);
  CHECK(differs(preset_smooth_rir, preset_rough_rir));

  sonare_free_rir_synth_result(&scalar_smooth);
  sonare_free_rir_synth_result(&scalar_rough);
  sonare_free_rir_synth_result(&preset_smooth_rir);
  sonare_free_rir_synth_result(&preset_rough_rir);
}

TEST_CASE("sonare acoustic C API names its material presets and modes", "[c_api][acoustic]") {
  const char* presets[] = {"none", "concrete", "wood", "curtain", "carpet", "glass"};
  for (int i = 0; i < 6; ++i) {
    REQUIRE(sonare_material_preset_name(i) != nullptr);
    CHECK(std::string(sonare_material_preset_name(i)) == presets[i]);
  }
  CHECK(sonare_material_preset_name(6) == nullptr);
  CHECK(sonare_material_preset_name(-1) == nullptr);

  const char* modes[] = {"auto", "blind", "impulseResponse"};
  for (int i = 0; i < 3; ++i) {
    REQUIRE(sonare_acoustic_mode_name(i) != nullptr);
    CHECK(std::string(sonare_acoustic_mode_name(i)) == modes[i]);
  }
  CHECK(sonare_acoustic_mode_name(3) == nullptr);
}

TEST_CASE("sonare_room_geometry_from_estimate maps an estimate onto the synthesizer config",
          "[c_api][acoustic]") {
  float bands[] = {0.1f, 0.12f, 0.15f, 0.2f, 0.25f, 0.3f};
  SonareRoomEstimate estimate{};
  estimate.volume = 100.0f;
  estimate.length_m = 6.0f;
  estimate.width_m = 4.5f;
  estimate.height_m = 3.0f;
  estimate.band_absorption = bands;
  estimate.band_count = 6;

  SonareRirSynthConfig geometry;
  sonare_rir_synth_config_init(&geometry);
  geometry.seed = 99u;
  REQUIRE(sonare_room_geometry_from_estimate(&estimate, &geometry) == SONARE_OK);
  CHECK(geometry.length_m == 6.0f);
  CHECK(geometry.width_m == 4.5f);
  CHECK(geometry.height_m == 3.0f);
  CHECK(geometry.band_absorption == bands);
  CHECK(geometry.band_absorption_count == 6);
  CHECK(geometry.seed == 1u);

  SECTION("feeds the synthesizer with the placement it returns") {
    geometry.ism_order = 2;
    geometry.max_sec = 0.1f;
    SonareRirSynthResult rir{};
    REQUIRE(sonare_synthesize_rir(&geometry, 22050, &rir) == SONARE_OK);
    CHECK(rir.length > 0);
    sonare_free_rir_synth_result(&rir);
  }

  SECTION("the scalar absorption is the mean of the finite bands") {
    CHECK(geometry.absorption ==
          Catch::Approx((0.1f + 0.12f + 0.15f + 0.2f + 0.25f + 0.3f) / 6.0f));
  }

  SECTION("the default dimensions map to the default placement exactly") {
    estimate.length_m = 7.0f;
    estimate.width_m = 5.0f;
    estimate.height_m = 3.0f;
    REQUIRE(sonare_room_geometry_from_estimate(&estimate, &geometry) == SONARE_OK);
    CHECK(geometry.source.x == 1.0f);
    CHECK(geometry.source.y == 1.0f);
    CHECK(geometry.source.z == 1.2f);
    CHECK(geometry.listener.x == 5.0f);
    CHECK(geometry.listener.y == 4.0f);
    CHECK(geometry.listener.z == 1.7f);
  }

  SECTION("a small room gets a placement inside it that synthesizes") {
    estimate.length_m = 1.33f;
    estimate.width_m = 1.33f;
    estimate.height_m = 1.33f;
    REQUIRE(sonare_room_geometry_from_estimate(&estimate, &geometry) == SONARE_OK);
    for (float v : {geometry.source.x, geometry.source.y, geometry.source.z, geometry.listener.x,
                    geometry.listener.y, geometry.listener.z}) {
      CHECK(v > 0.0f);
      CHECK(v < 1.33f);
    }
    geometry.ism_order = 2;
    geometry.max_sec = 0.1f;
    SonareRirSynthResult rir{};
    REQUIRE(sonare_synthesize_rir(&geometry, 22050, &rir) == SONARE_OK);
    CHECK(rir.length > 0);
    sonare_free_rir_synth_result(&rir);
  }

  SECTION("an estimate without bands keeps the default absorption") {
    estimate.band_absorption = nullptr;
    estimate.band_count = 0;
    REQUIRE(sonare_room_geometry_from_estimate(&estimate, &geometry) == SONARE_OK);
    CHECK(geometry.band_absorption == nullptr);
    CHECK(geometry.band_absorption_count == 0);
    CHECK(geometry.absorption == 0.2f);
  }

  SECTION("bands that did not converge are not borrowed and stay out of the mean") {
    bands[2] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_room_geometry_from_estimate(&estimate, &geometry) == SONARE_OK);
    CHECK(geometry.band_absorption == nullptr);
    CHECK(geometry.band_absorption_count == 0);
    CHECK(geometry.length_m == 6.0f);
    CHECK(geometry.absorption == Catch::Approx((0.1f + 0.12f + 0.2f + 0.25f + 0.3f) / 5.0f));
  }

  SECTION("refuses an unmeasured estimate") {
    SonareRoomEstimate unmeasured = estimate;
    unmeasured.length_m = std::numeric_limits<float>::quiet_NaN();
    CHECK(sonare_room_geometry_from_estimate(&unmeasured, &geometry) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_room_geometry_from_estimate(nullptr, &geometry) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_room_geometry_from_estimate(&estimate, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

namespace {

SonareRoomMorphConfig stereo_morph_config() {
  SonareRoomMorphConfig cfg;
  sonare_room_morph_config_init(&cfg);
  cfg.length_m = 8.0f;
  cfg.width_m = 6.0f;
  cfg.height_m = 3.5f;
  cfg.source.x = 2.0f;
  cfg.source.y = 3.0f;
  cfg.source.z = 1.2f;
  cfg.listener.x = 6.0f;
  cfg.listener.y = 3.0f;
  cfg.listener.z = 1.5f;
  cfg.absorption = 0.15f;
  cfg.wet = 1.0f;
  cfg.max_sec = 0.3f;
  return cfg;
}

}  // namespace

TEST_CASE("sonare acoustic C API stereo morph returns two different channels",
          "[c_api][acoustic]") {
  std::vector<float> input(4800, 0.0f);
  input[0] = 1.0f;
  const SonareRoomMorphConfig cfg = stereo_morph_config();

  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg, &out_left,
                                   &out_right, &out_length) == SONARE_OK);
  REQUIRE(out_length > input.size());
  REQUIRE(out_left != nullptr);
  REQUIRE(out_right != nullptr);
  // Identical inputs through two receivers: the added room differs between the channels.
  CHECK_FALSE(std::equal(out_left, out_left + out_length, out_right));

  // The mono morph of the same input keeps its single-listener output length.
  float* mono = nullptr;
  size_t mono_length = 0;
  REQUIRE(sonare_room_morph(input.data(), input.size(), 48000, &cfg, &mono, &mono_length) ==
          SONARE_OK);
  CHECK(mono_length > input.size());
  sonare_free_floats(mono);
  sonare_free_floats(out_left);
  sonare_free_floats(out_right);
}

TEST_CASE("sonare acoustic C API stereo morph refuses invalid arguments", "[c_api][acoustic]") {
  std::vector<float> input(4800, 0.0f);
  input[0] = 1.0f;
  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;

  SonareRoomMorphConfig too_wide = stereo_morph_config();
  too_wide.receiver_spacing_m = 4.5f;
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &too_wide,
                                 &out_left, &out_right,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_left == nullptr);
  CHECK(out_right == nullptr);
  CHECK(out_length == 0);

  SonareRoomMorphConfig negative = stereo_morph_config();
  negative.receiver_spacing_m = -0.5f;
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &negative,
                                 &out_left, &out_right,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // A spacing of 0 is read as 0, outside (0, 4].
  SonareRoomMorphConfig coincident = stereo_morph_config();
  coincident.receiver_spacing_m = 0.0f;
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &coincident,
                                 &out_left, &out_right,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // A listener 0.1 m from the side wall puts one receiver of a given 0.5 m pair outside.
  SonareRoomMorphConfig wall = stereo_morph_config();
  wall.listener.y = 0.1f;
  wall.source.y = 0.1f;
  wall.receiver_spacing_m = 0.5f;
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &wall, &out_left,
                                 &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  // The mono morph does not read the spacing, so the same placement still renders.
  float* mono = nullptr;
  size_t mono_length = 0;
  CHECK(sonare_room_morph(input.data(), input.size(), 48000, &wall, &mono, &mono_length) ==
        SONARE_OK);
  sonare_free_floats(mono);

  const SonareRoomMorphConfig cfg = stereo_morph_config();
  CHECK(sonare_room_morph_stereo(input.data(), nullptr, input.size(), 48000, &cfg, &out_left,
                                 &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, nullptr,
                                 &out_left, &out_right,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg, nullptr,
                                 &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg, &out_left,
                                 nullptr, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg, &out_left,
                                 &out_right, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare acoustic C API fits an omitted receiver spacing to the room",
          "[c_api][acoustic]") {
  std::vector<float> input(4800, 0.0f);
  input[0] = 1.0f;
  // The same wall-hugging placement: no 0.5 m pair fits, but a narrower one does.
  SonareRoomMorphConfig omitted = stereo_morph_config();
  omitted.listener.y = 0.1f;
  omitted.source.y = 0.1f;
  REQUIRE(std::isnan(omitted.receiver_spacing_m));
  const float spacing = sonare::acoustic::default_receiver_spacing(
      {omitted.length_m, omitted.width_m, omitted.height_m},
      {{omitted.source.x, omitted.source.y, omitted.source.z},
       {omitted.listener.x, omitted.listener.y, omitted.listener.z}});
  REQUIRE(spacing > 0.0f);
  REQUIRE(spacing < 0.5f);
  SonareRoomMorphConfig fitted = omitted;
  fitted.receiver_spacing_m = spacing;

  auto render = [&](const SonareRoomMorphConfig& cfg) {
    float* left = nullptr;
    float* right = nullptr;
    size_t length = 0;
    REQUIRE(sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg, &left,
                                     &right, &length) == SONARE_OK);
    std::vector<float> both(left, left + length);
    both.insert(both.end(), right, right + length);
    sonare_free_floats(left);
    sonare_free_floats(right);
    return both;
  };
  const std::vector<float> from_omitted = render(omitted);
  const std::vector<float> from_fitted = render(fitted);
  REQUIRE(from_omitted.size() == from_fitted.size());
  for (size_t i = 0; i < from_omitted.size(); ++i) {
    INFO(i);
    REQUIRE(from_omitted[i] == Catch::Approx(from_fitted[i]).margin(1e-6));
  }
}

TEST_CASE("sonare acoustic C API refuses a selector outside its documented set",
          "[c_api][acoustic]") {
  std::vector<float> input(2000, 0.0f);
  input[0] = 1.0f;
  SonareRoomMorphConfig morph;
  sonare_room_morph_config_init(&morph);
  morph.length_m = 7.0f;
  morph.width_m = 5.0f;
  morph.height_m = 3.0f;
  morph.source.x = 1.5f;
  morph.source.y = 1.0f;
  morph.source.z = 1.2f;
  morph.listener.x = 5.0f;
  morph.listener.y = 4.0f;
  morph.listener.z = 1.7f;
  morph.absorption = 0.3f;
  morph.wet = 1.0f;
  morph.max_sec = 0.1f;
  morph.ism_order = 2;

  const auto rir_result = [](const SonareRirSynthConfig& cfg) {
    SonareRirSynthResult result{};
    const SonareError err = sonare_synthesize_rir(&cfg, 48000, &result);
    const bool wrote = result.rir != nullptr;
    sonare_free_rir_synth_result(&result);
    return std::make_pair(err, wrote);
  };
  const auto morph_result = [&](const SonareRoomMorphConfig& cfg) {
    float* out = nullptr;
    size_t out_length = 0;
    const SonareError err =
        sonare_room_morph(input.data(), input.size(), 48000, &cfg, &out, &out_length);
    const bool wrote = out != nullptr;
    sonare_free_floats(out);
    float* stereo_left = nullptr;
    float* stereo_right = nullptr;
    size_t stereo_length = 0;
    const SonareError stereo_err =
        sonare_room_morph_stereo(input.data(), input.data(), input.size(), 48000, &cfg,
                                 &stereo_left, &stereo_right, &stereo_length);
    const bool stereo_wrote = stereo_left != nullptr;
    sonare_free_floats(stereo_left);
    sonare_free_floats(stereo_right);
    REQUIRE(stereo_err == err);
    return std::make_pair(err, wrote || stereo_wrote);
  };

  for (int prefer_eyring : {0, 1}) {
    CAPTURE(prefer_eyring);
    SonareRirSynthConfig cfg = valid_rir_config();
    cfg.prefer_eyring = prefer_eyring;
    REQUIRE(rir_result(cfg) == std::make_pair(SONARE_OK, true));
    SonareRoomMorphConfig m = morph;
    m.prefer_eyring = prefer_eyring;
    REQUIRE(morph_result(m) == std::make_pair(SONARE_OK, true));
  }

  for (int preset : {-1, SONARE_MATERIAL_PRESET_GLASS + 1, 99}) {
    CAPTURE(preset);
    SonareRirSynthConfig cfg = valid_rir_config();
    cfg.material_preset = preset;
    REQUIRE(rir_result(cfg) == std::make_pair(SONARE_ERROR_INVALID_PARAMETER, false));
    SonareRoomMorphConfig m = morph;
    m.material_preset = preset;
    REQUIRE(morph_result(m) == std::make_pair(SONARE_ERROR_INVALID_PARAMETER, false));
  }
  for (int preset = SONARE_MATERIAL_PRESET_NONE; preset <= SONARE_MATERIAL_PRESET_GLASS; ++preset) {
    CAPTURE(preset);
    SonareRirSynthConfig cfg = valid_rir_config();
    cfg.material_preset = preset;
    REQUIRE(rir_result(cfg) == std::make_pair(SONARE_OK, true));
  }

  // The estimator's routing mode is refused the same way, before any analysis.
  SonareRirSynthConfig source = valid_rir_config();
  source.max_sec = 0.5f;
  SonareRirSynthResult rir{};
  REQUIRE(sonare_synthesize_rir(&source, 48000, &rir) == SONARE_OK);
  for (int mode : {-1, SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE + 1}) {
    CAPTURE(mode);
    SonareRoomEstimateConfig estimate_cfg = valid_estimate_config();
    estimate_cfg.mode = mode;
    SonareRoomEstimate estimate{};
    REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &estimate) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(estimate.band_absorption == nullptr);
    REQUIRE(estimate.band_count == 0);
  }
  SonareRoomEstimateConfig estimate_cfg = valid_estimate_config();
  SonareRoomEstimate estimate{};
  REQUIRE(sonare_estimate_room(rir.rir, rir.length, 48000, &estimate_cfg, &estimate) == SONARE_OK);
  sonare_free_room_estimate(&estimate);
  sonare_free_rir_synth_result(&rir);
}
