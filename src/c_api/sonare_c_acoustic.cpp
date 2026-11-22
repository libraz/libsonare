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
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "acoustic/material.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/common/room_field_checks.h"
#include "util/parameter_checks.h"
#endif

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
using sonare::Audio;
using sonare_c_detail::CName;
using sonare_c_detail::require_struct_size;
using sonare_c_detail::run_mono_offline;
using sonare_c_detail::run_offline;
using sonare_c_detail::with_c_names;
#endif

namespace {

#if defined(SONARE_WITH_ACOUSTIC_SIM)
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

// The wall-material fields both room configs carry, refused by member name.
template <typename Config>
void check_wall_material(const Config& config) {
  namespace check = sonare::util::check;
  const auto bands = [](const float* values, size_t count, const char* name,
                        const std::string& count_name) {
    check::at_most(static_cast<double>(count),
                   static_cast<double>(sonare::acoustic::kMaxMaterialBands), count_name.c_str());
    check::non_null_when_count(values, count, name, count_name.c_str());
    for (size_t i = 0; i < count; ++i) {
      const std::string element = std::string(name) + "[" + std::to_string(i) + "]";
      check::in_range(values[i], 0.0, 1.0, element.c_str());
    }
  };
  check::in_range(config.absorption, 0.0, 1.0, "config.absorption");
  bands(config.band_absorption, config.band_absorption_count, "config.band_absorption",
        "config.band_absorption_count");
  bands(config.band_scattering, config.band_scattering_count, "config.band_scattering",
        "config.band_scattering_count");
  check::integer_in_range(config.material_preset, SONARE_MATERIAL_PRESET_NONE,
                          SONARE_MATERIAL_PRESET_GLASS, "config.material_preset");
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
  // Not given: resolved at use to the default placement scaled to the room.
  constexpr float kNotGiven = std::numeric_limits<float>::quiet_NaN();
  config->source = {kNotGiven, kNotGiven, kNotGiven};
  config->listener = {kNotGiven, kNotGiven, kNotGiven};
  config->absorption = kDefaultWallAbsorption;
  config->material_preset = SONARE_MATERIAL_PRESET_NONE;
  const AirAbsorption air{};
  config->air_temperature_c = air.temperature_c;
  config->air_humidity_percent = air.humidity_percent;
}

// A position left all-NaN is not given; any other non-finite component is refused by name.
std::array<std::optional<float>, 3> requested_position(const SonareRoomPlacement& p,
                                                       const std::string& subject) {
  if (std::isnan(p.x) && std::isnan(p.y) && std::isnan(p.z)) return {};
  namespace check = sonare::util::check;
  check::finite_float(p.x, (subject + ".x").c_str());
  check::finite_float(p.y, (subject + ".y").c_str());
  check::finite_float(p.z, (subject + ".z").c_str());
  return {p.x, p.y, p.z};
}

// The source, listener and (for a morph config) receiver spacing a room config asks for,
// resolved against its room; NaN is the C spelling of "not given".
template <typename Config>
sonare::acoustic::ResolvedRoomPlacement resolve_placement(const Config& config) {
  sonare::acoustic::RoomPlacementRequest request;
  request.source = requested_position(config.source, "config.source");
  request.listener = requested_position(config.listener, "config.listener");
  if constexpr (std::is_same_v<Config, SonareRoomMorphConfig>) {
    if (!std::isnan(config.receiver_spacing_m)) {
      request.receiver_spacing_m = config.receiver_spacing_m;
    }
  }
  return sonare::acoustic::resolve_room_placement(
      sonare::RoomDimensions{config.length_m, config.width_m, config.height_m}, request);
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
  // Not given: fitted to the room and placement at use.
  config->receiver_spacing_m = std::numeric_limits<float>::quiet_NaN();
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
  const sonare::acoustic::SourceListener placement =
      sonare::acoustic::resolve_room_placement(
          sonare::RoomDimensions{estimate->length_m, estimate->width_m, estimate->height_m}, {})
          .placement;
  out->source = {placement.source.x, placement.source.y, placement.source.z};
  out->listener = {placement.listener.x, placement.listener.y, placement.listener.z};
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_room_estimate_config_init(SonareRoomEstimateConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  const sonare::RoomEstimateConfig defaults;
  config->aspect_hint_lw = defaults.aspect_hint_lw;
  config->aspect_hint_lh = defaults.aspect_hint_lh;
  config->reference_absorption = defaults.reference_absorption;
  config->min_decay_db = defaults.acoustic.min_decay_db;
  config->noise_floor_margin_db = defaults.acoustic.noise_floor_margin_db;
  config->prefer_eyring = defaults.prefer_eyring ? 1 : 0;
  config->n_octave_bands = defaults.acoustic.n_octave_bands;
  config->mode = static_cast<int>(defaults.acoustic.mode);
#endif
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
  SONARE_C_TRY
  require_struct_size(config->struct_size, sizeof(SonareRirSynthConfig), "config",
                      "sonare_rir_synth_config_init");
  namespace check = sonare::util::check;
  check::integer_within(sample_rate, sonare::kAudioSampleRateDomain, "sample_rate");
  check::finite_float(config->length_m, "config.length_m");
  check::finite_float(config->width_m, "config.width_m");
  check::finite_float(config->height_m, "config.height_m");
  const sonare::acoustic::SourceListener placement = resolve_placement(*config).placement;
  check::integer_within(config->ism_order, sonare::effects::common::kRirIsmOrderDomain,
                        "config.ism_order");
  check::in_range(config->max_sec, 0.0, sonare::acoustic::kMaxRirSeconds, "config.max_sec");
  check::in_range(config->mixing_time_ms, 0.0, sonare::acoustic::kMaxRirMixingTimeMs,
                  "config.mixing_time_ms");
  check::in_range(config->crossfade_ms, 0.0, sonare::acoustic::kMaxRirCrossfadeMs,
                  "config.crossfade_ms");
  check_wall_material(*config);
  sonare::acoustic::RirSynthConfig rc;
  rc.late_model = config->prefer_eyring != 0 ? sonare::acoustic::ReverbModel::Eyring
                                             : sonare::acoustic::ReverbModel::Sabine;
  rc.air_absorption_enabled = config->air_absorption_enabled != 0;
  SONARE_C_REQUIRE_NONNULL(out);
  using namespace sonare::acoustic;
  const ShoeboxRoom room =
      make_room(config->length_m, config->width_m, config->height_m, config->absorption,
                config->band_absorption, config->band_absorption_count, config->band_scattering,
                config->band_scattering_count, config->material_preset);
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kAcousticSim, config,
                                                 sample_rate, out);
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
  SONARE_C_TRY
  require_struct_size(config->struct_size, sizeof(SonareRoomEstimateConfig), "config",
                      "sonare_room_estimate_config_init");
  namespace check = sonare::util::check;
  sonare::AcousticConfig::Mode mode = sonare::AcousticConfig::Mode::Auto;
  if (!sonare_c_detail::ordinal_to_enum(config->mode, SONARE_ACOUSTIC_MODE_AUTO,
                                        SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE, &mode)) {
    check::integer_in_range(config->mode, SONARE_ACOUSTIC_MODE_AUTO,
                            SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE, "config.mode");
  }
  check::greater_than(config->aspect_hint_lw, 0.0, "config.aspect_hint_lw");
  check::greater_than(config->aspect_hint_lh, 0.0, "config.aspect_hint_lh");
  check::in_range(config->reference_absorption, sonare::kRoomReferenceAbsorptionMin,
                  sonare::kRoomReferenceAbsorptionMax, "config.reference_absorption");
  check::greater_than(config->min_decay_db, 0.0, "config.min_decay_db");
  check::at_least(config->noise_floor_margin_db, 0.0, "config.noise_floor_margin_db");
  check::integer_within(config->n_octave_bands, sonare_c_detail::kRoomEstimateOctaveBandsDomain,
                        "config.n_octave_bands");
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        sonare::RoomEstimateConfig cfg;
        cfg.aspect_hint_lw = config->aspect_hint_lw;
        cfg.aspect_hint_lh = config->aspect_hint_lh;
        cfg.reference_absorption = config->reference_absorption;
        cfg.prefer_eyring = config->prefer_eyring != 0;
        cfg.acoustic.n_octave_bands = config->n_octave_bands;
        cfg.acoustic.min_decay_db = config->min_decay_db;
        cfg.acoustic.noise_floor_margin_db = config->noise_floor_margin_db;
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
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kAcousticSim, samples,
                                                 length, sample_rate, config, out);
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
void check_room_morph_config(const SonareRoomMorphConfig& config) {
  namespace check = sonare::util::check;
  check_wall_material(config);
  check::in_range(config.source_tail_suppression, 0.0, 1.0, "config.source_tail_suppression");
  check::in_range(config.wet, 0.0, 1.0, "config.wet");
}

sonare::effects::acoustic::RoomMorphConfig room_morph_config_from(
    const SonareRoomMorphConfig& config) {
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target =
      make_room(config.length_m, config.width_m, config.height_m, config.absorption,
                config.band_absorption, config.band_absorption_count, config.band_scattering,
                config.band_scattering_count, config.material_preset);
  const sonare::acoustic::ResolvedRoomPlacement resolved = resolve_placement(config);
  cfg.placement = resolved.placement;
  cfg.supplied = resolved.supplied;
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
  cfg.receiver_spacing_m = resolved.receiver_spacing_m;
  return cfg;
}

// The core morph config's member paths, as SonareRoomMorphConfig spells them.
const std::array<CName, 15> kRoomMorphNames{
    {{"target.dims.length", "config.length_m"},
     {"target.dims.width", "config.width_m"},
     {"target.dims.height", "config.height_m"},
     {"placement.source", "config.source"},
     {"placement.listener", "config.listener"},
     {"source_tail_suppression", "config.source_tail_suppression"},
     {"wet", "config.wet"},
     {"ism_order", "config.ism_order"},
     {"max_seconds", "config.max_sec"},
     {"mixing_time_ms", "config.mixing_time_ms"},
     {"crossfade_ms", "config.crossfade_ms"},
     {"air.temperature_c", "config.air_temperature_c"},
     {"air.humidity_percent", "config.air_humidity_percent"},
     {"receiver_spacing_m", "config.receiver_spacing_m"},
     {"seed", "config.seed"}}};

}  // namespace
#endif

SonareError sonare_room_morph(const float* samples, size_t length, int sample_rate,
                              const SonareRoomMorphConfig* config, float** out,
                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_diagnostics();
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(config);
  require_struct_size(config->struct_size, sizeof(SonareRoomMorphConfig), "config",
                      "sonare_room_morph_config_init");
  check_room_morph_config(*config);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_offline(
      "samples", samples, length, sample_rate, out, out_length, [&](const Audio& audio) -> Audio {
        sonare::effects::acoustic::RoomMorphResult result = with_c_names(kRoomMorphNames, [&] {
          return sonare::effects::acoustic::room_morph(audio, room_morph_config_from(*config));
        });
        // Published the way synthesize_rir publishes its own: the target RIR is
        // synthesized by the same code, and a clamp there means this morph used
        // a room the caller did not request.
        publish_rir_diagnostics(result.diagnostics);
        return std::move(result.audio);
      });
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kAcousticSim, samples,
                                                 length, sample_rate, config, out, out_length);
#endif
}

SonareError sonare_room_morph_stereo(const float* left, const float* right, size_t length,
                                     int sample_rate, const SonareRoomMorphConfig* config,
                                     float** out_left, float** out_right, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_left, out_right, out_length);
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_diagnostics();
  SonareError err = sonare_c_detail::validate_audio_params("left", left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = sonare_c_detail::validate_audio_params("right", right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(config);
  require_struct_size(config->struct_size, sizeof(SonareRoomMorphConfig), "config",
                      "sonare_room_morph_config_init");
  check_room_morph_config(*config);
  SONARE_C_REQUIRE_NONNULL(out_left);
  SONARE_C_REQUIRE_NONNULL(out_right);
  SONARE_C_REQUIRE_NONNULL(out_length);
  const sonare::effects::acoustic::RoomMorphStereoResult result =
      with_c_names(kRoomMorphNames, [&] {
        return sonare::effects::acoustic::room_morph_stereo(
            Audio::from_buffer(left, length, sample_rate),
            Audio::from_buffer(right, length, sample_rate), room_morph_config_from(*config));
      });
  publish_rir_diagnostics(result.diagnostics);
  const size_t n = result.left.size();
  std::unique_ptr<float[]> left_out(new float[n]);
  std::unique_ptr<float[]> right_out(new float[n]);
  std::memcpy(left_out.get(), result.left.data(), n * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), n * sizeof(float));
  *out_left = sonare_c_detail::release_array(left_out);
  *out_right = sonare_c_detail::release_array(right_out);
  *out_length = n;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out_left);
  SONARE_C_REQUIRE_NONNULL(out_right);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kAcousticSim, left,
                                                 right, length, sample_rate, config);
#endif
}
