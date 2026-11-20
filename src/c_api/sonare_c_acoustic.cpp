#include <sonare/sonare_c_acoustic.h>

#include <array>

#include "acoustic/estimate_geometry.h"
#include "acoustic/late_reverb.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "acoustic/selector_names.h"
#include "effects/acoustic/room_morph.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "acoustic/material.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "util/zero_is_default.h"
#endif

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
using sonare::Audio;
using sonare_c_detail::run_mono_offline;
using sonare_c_detail::run_offline;
using sonare_c_detail::struct_size_accepted;
#endif

namespace {

#if defined(SONARE_WITH_ACOUSTIC_SIM)
// The material selector set make_room maps; anything else is refused.
bool valid_material_preset(int selector) {
  return sonare_c_detail::valid_c_enum(selector, SONARE_MATERIAL_PRESET_NONE,
                                       SONARE_MATERIAL_PRESET_GLASS);
}

// Build a uniform shoebox whose single wall material is chosen by precedence:
//   material_preset (non-zero) > per-band absorption array > scalar absorption.
// All six walls share the resulting material (this ABI exposes only uniform
// rooms; per-wall mesh materials are not reachable here yet). The precedence,
// the coefficient validation and the scattering-band application all live in
// the core builder, which every other surface calls as well.
sonare::acoustic::ShoeboxRoom make_room(float length, float width, float height, float absorption,
                                        const float* absorption_bands, size_t absorption_band_count,
                                        const float* scattering_bands, size_t scattering_band_count,
                                        int material_preset) {
  using namespace sonare::acoustic;

  WallMaterialRequest request;
  request.has_preset = material_preset_from_selector(material_preset, &request.preset);
  if (absorption_bands != nullptr && absorption_band_count > 0) {
    request.absorption_bands.assign(absorption_bands, absorption_bands + absorption_band_count);
  }
  if (scattering_bands != nullptr && scattering_band_count > 0) {
    request.scattering_bands.assign(scattering_bands, scattering_bands + scattering_band_count);
  }
  request.absorption = absorption;
  return make_uniform_room(sonare::RoomDimensions{length, width, height}, request);
}

// Publishes the synthesizer's whole diagnostic list, in its own order, so a C
// caller reads the same entries Node and WASM return as an array.
void publish_rir_diagnostics(const std::vector<sonare::Diagnostic>& diagnostics) {
  sonare_c_detail::set_last_diagnostics(diagnostics);
}

static_assert(static_cast<int>(sonare::AcousticConfig::Mode::Auto) == SONARE_ACOUSTIC_MODE_AUTO &&
                  static_cast<int>(sonare::AcousticConfig::Mode::Blind) ==
                      SONARE_ACOUSTIC_MODE_BLIND &&
                  static_cast<int>(sonare::AcousticConfig::Mode::ImpulseResponse) ==
                      SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE,
              "SONARE_ACOUSTIC_MODE_* must mirror AcousticConfig::Mode");
#endif

static_assert(SONARE_MATERIAL_PRESET_GLASS + 1 ==
                  sonare::acoustic::kMaterialPresetSelectorNames.size(),
              "material preset names must cover the SONARE_MATERIAL_PRESET_* values");
static_assert(SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE + 1 ==
                  sonare::acoustic::kAcousticModeSelectorNames.size(),
              "acoustic mode names must cover the SONARE_ACOUSTIC_MODE_* values");

// The geometry, placement and synthesis defaults the two room configs share.
template <typename Config>
void init_room_fields(Config* config) {
  using namespace sonare::acoustic;
  config->length_m = kDefaultRoomDims.length;
  config->width_m = kDefaultRoomDims.width;
  config->height_m = kDefaultRoomDims.height;
  config->source = {kDefaultPlacement.source.x, kDefaultPlacement.source.y,
                    kDefaultPlacement.source.z};
  config->listener = {kDefaultPlacement.listener.x, kDefaultPlacement.listener.y,
                      kDefaultPlacement.listener.z};
  config->absorption = kDefaultWallAbsorption;
  config->material_preset = SONARE_MATERIAL_PRESET_NONE;
  const AirAbsorption air{};
  config->air_temperature_c = air.temperature_c;
  config->air_humidity_percent = air.humidity_percent;
}

template <size_t N>
const char* enum_name(const std::array<const char*, N>& names, int value) {
  return value >= 0 && static_cast<size_t>(value) < N ? names[value] : nullptr;
}

}  // namespace

void sonare_rir_synth_config_init(SonareRirSynthConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  init_room_fields(config);
  const sonare::acoustic::RirSynthConfig defaults;
  config->max_sec = defaults.max_seconds;
  config->mixing_time_ms = defaults.mixing_time_ms;
  config->crossfade_ms = defaults.crossfade_ms;
  config->ism_order = defaults.ism_order;
  config->seed = defaults.seed;
  config->prefer_eyring = defaults.late_model == sonare::acoustic::ReverbModel::Eyring ? 1 : 0;
  config->air_absorption_enabled = defaults.air_absorption_enabled ? 1 : 0;
}

void sonare_room_morph_config_init(SonareRoomMorphConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  init_room_fields(config);
  const sonare::effects::acoustic::RoomMorphConfig defaults;
  config->source_tail_suppression = defaults.source_tail_suppression;
  config->wet = defaults.wet;
  config->max_sec = defaults.max_seconds;
  config->mixing_time_ms = defaults.mixing_time_ms;
  config->crossfade_ms = defaults.crossfade_ms;
  config->ism_order = defaults.ism_order;
  config->seed = defaults.seed;
  config->receiver_spacing_m = defaults.receiver_spacing_m;
  config->prefer_eyring = defaults.late_model == sonare::acoustic::ReverbModel::Eyring ? 1 : 0;
  config->air_absorption_enabled = defaults.air_absorption_enabled ? 1 : 0;
}

const char* sonare_material_preset_name(int preset) {
  return enum_name(sonare::acoustic::kMaterialPresetSelectorNames, preset);
}

const char* sonare_acoustic_mode_name(int mode) {
  return enum_name(sonare::acoustic::kAcousticModeSelectorNames, mode);
}

SonareError sonare_room_geometry_from_estimate(const SonareRoomEstimate* estimate,
                                               SonareRirSynthConfig* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(estimate);
  SONARE_C_REQUIRE_NONNULL(out);
  sonare_rir_synth_config_init(out);
  if (!sonare::acoustic::estimated_dimensions_measured(estimate->length_m, estimate->width_m,
                                                       estimate->height_m)) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                                    "room estimate has no measurable dimensions");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sonare::acoustic::estimated_absorption_measured(estimate->band_absorption,
                                                      estimate->band_count)) {
    out->band_absorption = estimate->band_absorption;
    out->band_absorption_count = estimate->band_count;
  }
  const float mean =
      sonare::acoustic::mean_finite_absorption(estimate->band_absorption, estimate->band_count);
  if (std::isfinite(mean)) out->absorption = mean;
  out->length_m = estimate->length_m;
  out->width_m = estimate->width_m;
  out->height_m = estimate->height_m;
  const sonare::acoustic::SourceListener placement = sonare::acoustic::scaled_default_placement(
      estimate->length_m, estimate->width_m, estimate->height_m);
  out->source = {placement.source.x, placement.source.y, placement.source.z};
  out->listener = {placement.listener.x, placement.listener.y, placement.listener.z};
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_synthesize_rir(const SonareRirSynthConfig* config, int sample_rate,
                                  SonareRirSynthResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_diagnostics();
  // The result records the requested rate even when the call is refused.
  if (out != nullptr) out->sample_rate = sample_rate;
  SONARE_C_REQUIRE_NONNULL(config);
  if (!struct_size_accepted(config->struct_size, sizeof(SonareRirSynthConfig),
                            "SonareRirSynthConfig") ||
      sample_rate < sonare_c_detail::kMinSampleRate ||
      sample_rate > sonare_c_detail::kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto finite = [](float value) { return std::isfinite(value); };
  const auto unit = [&](float value) { return finite(value) && value >= 0.0f && value <= 1.0f; };
  if (!finite(config->length_m) || !finite(config->width_m) || !finite(config->height_m) ||
      !finite(config->source.x) || !finite(config->source.y) || !finite(config->source.z) ||
      !finite(config->listener.x) || !finite(config->listener.y) || !finite(config->listener.z) ||
      !unit(config->absorption) || config->ism_order < 0 || !std::isfinite(config->max_sec) ||
      config->max_sec < 0.0f || config->max_sec > sonare::acoustic::kMaxRirSeconds ||
      !std::isfinite(config->mixing_time_ms) || config->mixing_time_ms < 0.0f ||
      config->mixing_time_ms > sonare::acoustic::kMaxRirMixingTimeMs ||
      !std::isfinite(config->crossfade_ms) || config->crossfade_ms < 0.0f ||
      config->crossfade_ms > sonare::acoustic::kMaxRirCrossfadeMs ||
      config->band_absorption_count > sonare::acoustic::kMaxMaterialBands ||
      config->band_scattering_count > sonare::acoustic::kMaxMaterialBands ||
      (config->band_absorption_count > 0 && config->band_absorption == nullptr) ||
      (config->band_scattering_count > 0 && config->band_scattering == nullptr) ||
      !valid_material_preset(config->material_preset)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::acoustic::RirSynthConfig rc;
  rc.late_model = config->prefer_eyring != 0 ? sonare::acoustic::ReverbModel::Eyring
                                             : sonare::acoustic::ReverbModel::Sabine;
  rc.air_absorption_enabled = config->air_absorption_enabled != 0;
  for (size_t i = 0; i < config->band_absorption_count; ++i) {
    if (!unit(config->band_absorption[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < config->band_scattering_count; ++i) {
    if (!unit(config->band_scattering[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  using namespace sonare::acoustic;
  const ShoeboxRoom room =
      make_room(config->length_m, config->width_m, config->height_m, config->absorption,
                config->band_absorption, config->band_absorption_count, config->band_scattering,
                config->band_scattering_count, config->material_preset);
  const SourceListener placement{{config->source.x, config->source.y, config->source.z},
                                 {config->listener.x, config->listener.y, config->listener.z}};
  rc.ism_order = config->ism_order;
  rc.seed = config->seed;
  rc.max_seconds = config->max_sec;
  rc.mixing_time_ms = config->mixing_time_ms;
  rc.crossfade_ms = config->crossfade_ms;
  // An implausible climate is left for the core to diagnose (acoustic.invalid_air_absorption).
  rc.air.temperature_c = config->air_temperature_c;
  rc.air.humidity_percent = config->air_humidity_percent;

  const RirSynthResult res = synthesize_rir(room, placement, sample_rate, rc);
  publish_rir_diagnostics(res.diagnostics);
  if (has_error(res.diagnostics)) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                                    first_error_text(res.diagnostics).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!res.rir.empty()) {
    std::unique_ptr<float[]> rir(new float[res.rir.size()]);
    std::memcpy(rir.get(), res.rir.data(), res.rir.size() * sizeof(float));
    out->rir = rir.release();
  }
  out->length = res.rir.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(config, sample_rate, out);
#endif
}

void sonare_free_rir_synth_result(SonareRirSynthResult* result) {
  if (!result) return;
  delete[] result->rir;
  *result = {};
}

SonareError sonare_estimate_room(const float* samples, size_t length, int sample_rate,
                                 const SonareRoomEstimateConfig* config, SonareRoomEstimate* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  SONARE_C_REQUIRE_NONNULL(config);
  sonare::AcousticConfig::Mode mode = sonare::AcousticConfig::Mode::Auto;
  if (!sonare_c_detail::ordinal_to_enum(config->mode, SONARE_ACOUSTIC_MODE_AUTO,
                                        SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE, &mode)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "estimate_room", "samples", samples, length, sample_rate,
      [&](const Audio& audio) -> SonareError {
        sonare::RoomEstimateConfig cfg;
        // Same "0 selects the library value" rule as the RIR-synth config above,
        // applied to EVERY float in the struct rather than field by field. An
        // exception here is not a small one: the Eyring volume scales with the cube
        // of the absorption prior, so a literal 0 clamped to the analyzer's 0.01
        // floor reports a 36.7 m^3 room as 0.0087 m^3.
        cfg.aspect_hint_lw =
            sonare::ZeroIsDefault(config->aspect_hint_lw).or_default(cfg.aspect_hint_lw);
        cfg.aspect_hint_lh =
            sonare::ZeroIsDefault(config->aspect_hint_lh).or_default(cfg.aspect_hint_lh);
        cfg.reference_absorption = sonare::ZeroIsDefault(config->reference_absorption)
                                       .or_default(cfg.reference_absorption);
        cfg.prefer_eyring = config->prefer_eyring != 0;
        if (config->n_octave_bands != 0) cfg.acoustic.n_octave_bands = config->n_octave_bands;
        cfg.acoustic.min_decay_db =
            sonare::ZeroIsDefault(config->min_decay_db).or_default(cfg.acoustic.min_decay_db);
        cfg.acoustic.noise_floor_margin_db = sonare::ZeroIsDefault(config->noise_floor_margin_db)
                                                 .or_default(cfg.acoustic.noise_floor_margin_db);
        cfg.acoustic.mode = mode;

        const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
        SonareRoomEstimate staged{};
        staged.volume = est.volume;
        staged.length_m = est.dims.length;
        staged.width_m = est.dims.width;
        staged.height_m = est.dims.height;
        staged.drr_db = est.drr_db;
        staged.confidence = est.confidence;
        // The two band vectors are independent estimates (absorption from the
        // inverse problem, RT60 from the decay fit) and either can legitimately
        // fail to converge on its own. band_count is documented as the length of
        // BOTH arrays, so truncating to the shorter one silently dropped a
        // fully-computed array whenever its sibling failed — exactly when a
        // caller most wants the surviving data. Pad the shorter side with NaN
        // instead so both arrays reach band_count and neither is ever discarded
        // because the other came back empty.
        const size_t max_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
        std::unique_ptr<float[]> staged_absorption(
            sonare_c_detail::copy_float_vector_padded(est.absorption_bands, max_count));
        std::unique_ptr<float[]> staged_rt60(
            sonare_c_detail::copy_float_vector_padded(est.rt60_bands, max_count));
        staged.band_absorption = staged_absorption.release();
        staged.rt60_bands = staged_rt60.release();
        staged.band_count = max_count;
        *out = staged;
        return SONARE_OK;
      });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out);
#endif
}

void sonare_free_room_estimate(SonareRoomEstimate* result) {
  if (!result) return;
  delete[] result->band_absorption;
  delete[] result->rt60_bands;
  *result = {};
}

#if defined(SONARE_WITH_ACOUSTIC_SIM)
namespace {

// The scalar and band checks the core does not repeat, shared by both morph entries.
bool room_morph_config_is_valid(const SonareRoomMorphConfig& config) {
  if (!struct_size_accepted(config.struct_size, sizeof(SonareRoomMorphConfig),
                            "SonareRoomMorphConfig")) {
    return false;
  }
  const auto unit = [](float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  };
  if (!unit(config.absorption) || !unit(config.source_tail_suppression) || !unit(config.wet) ||
      config.band_absorption_count > sonare::acoustic::kMaxMaterialBands ||
      config.band_scattering_count > sonare::acoustic::kMaxMaterialBands ||
      (config.band_absorption_count > 0 && config.band_absorption == nullptr) ||
      (config.band_scattering_count > 0 && config.band_scattering == nullptr) ||
      !valid_material_preset(config.material_preset)) {
    return false;
  }
  for (size_t i = 0; i < config.band_absorption_count; ++i) {
    if (!unit(config.band_absorption[i])) return false;
  }
  for (size_t i = 0; i < config.band_scattering_count; ++i) {
    if (!unit(config.band_scattering[i])) return false;
  }
  return true;
}

sonare::effects::acoustic::RoomMorphConfig room_morph_config_from(
    const SonareRoomMorphConfig& config) {
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target =
      make_room(config.length_m, config.width_m, config.height_m, config.absorption,
                config.band_absorption, config.band_absorption_count, config.band_scattering,
                config.band_scattering_count, config.material_preset);
  cfg.placement = {{config.source.x, config.source.y, config.source.z},
                   {config.listener.x, config.listener.y, config.listener.z}};
  cfg.source_tail_suppression = config.source_tail_suppression;
  cfg.wet = config.wet;
  cfg.ism_order = config.ism_order;
  cfg.seed = config.seed;
  cfg.max_seconds = config.max_sec;
  cfg.mixing_time_ms = config.mixing_time_ms;
  cfg.crossfade_ms = config.crossfade_ms;
  cfg.late_model = config.prefer_eyring != 0 ? sonare::acoustic::ReverbModel::Eyring
                                             : sonare::acoustic::ReverbModel::Sabine;
  cfg.air_absorption_enabled = config.air_absorption_enabled != 0;
  // The core rejects an implausible climate by throwing, mapped to INVALID_PARAMETER.
  cfg.air.temperature_c = config.air_temperature_c;
  cfg.air.humidity_percent = config.air_humidity_percent;
  // Read by the stereo entry only; the mono morph neither validates nor uses it.
  cfg.receiver_spacing_m = config.receiver_spacing_m;
  return cfg;
}

}  // namespace
#endif

SonareError sonare_room_morph(const float* samples, size_t length, int sample_rate,
                              const SonareRoomMorphConfig* config, float** out,
                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_diagnostics();
  if (!config || !room_morph_config_is_valid(*config)) return SONARE_ERROR_INVALID_PARAMETER;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  return run_mono_offline("room_morph", "samples", samples, length, sample_rate, out, out_length,
                          [&](const Audio& audio) -> Audio {
                            sonare::effects::acoustic::RoomMorphResult result =
                                sonare::effects::acoustic::room_morph(
                                    audio, room_morph_config_from(*config));
                            // Published the way synthesize_rir publishes its own: the target RIR is
                            // synthesized by the same code, and a clamp there means this morph used
                            // a room the caller did not request.
                            publish_rir_diagnostics(result.diagnostics);
                            return std::move(result.audio);
                          });
#else
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out, out_length);
#endif
}

SonareError sonare_room_morph_stereo(const float* left, const float* right, size_t length,
                                     int sample_rate, const SonareRoomMorphConfig* config,
                                     SonareRoomMorphStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_diagnostics();
  SonareError err = sonare_c_detail::validate_audio_params("room_morph_stereo", "left", left,
                                                           length, sample_rate);
  if (err != SONARE_OK) return err;
  err = sonare_c_detail::validate_audio_params("room_morph_stereo", "right", right, length,
                                               sample_rate);
  if (err != SONARE_OK) return err;
  if (!config || !room_morph_config_is_valid(*config)) return SONARE_ERROR_INVALID_PARAMETER;
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  const sonare::effects::acoustic::RoomMorphStereoResult result =
      sonare::effects::acoustic::room_morph_stereo(Audio::from_buffer(left, length, sample_rate),
                                                   Audio::from_buffer(right, length, sample_rate),
                                                   room_morph_config_from(*config));
  publish_rir_diagnostics(result.diagnostics);
  const size_t n = result.left.size();
  std::unique_ptr<float[]> left_out(new float[n]);
  std::unique_ptr<float[]> right_out(new float[n]);
  std::memcpy(left_out.get(), result.left.data(), n * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), n * sizeof(float));
  out->left = sonare_c_detail::release_array(left_out);
  out->right = sonare_c_detail::release_array(right_out);
  out->length = n;
  out->sample_rate = result.left.sample_rate();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_STUB_NOT_SUPPORTED(left, right, length, sample_rate, config);
#endif
}

void sonare_free_room_morph_stereo_result(SonareRoomMorphStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  *result = {};
}
