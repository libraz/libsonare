#include <sonare/sonare_c_acoustic.h>

#if defined(SONARE_WITH_ACOUSTIC_SIM)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#endif

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
using sonare::Audio;
using sonare_c_detail::run_mono_offline;
using sonare_c_detail::run_offline;
#endif

namespace {

#if defined(SONARE_WITH_ACOUSTIC_SIM)
// Map a SONARE_MATERIAL_PRESET_* selector onto sonare::acoustic::MaterialPreset.
// Returns false for SONARE_MATERIAL_PRESET_NONE or any out-of-range value.
bool preset_from_int(int selector, sonare::acoustic::MaterialPreset* out) {
  using sonare::acoustic::MaterialPreset;
  switch (selector) {
    case SONARE_MATERIAL_PRESET_CONCRETE:
      *out = MaterialPreset::Concrete;
      return true;
    case SONARE_MATERIAL_PRESET_WOOD:
      *out = MaterialPreset::Wood;
      return true;
    case SONARE_MATERIAL_PRESET_CURTAIN:
      *out = MaterialPreset::Curtain;
      return true;
    case SONARE_MATERIAL_PRESET_CARPET:
      *out = MaterialPreset::Carpet;
      return true;
    case SONARE_MATERIAL_PRESET_GLASS:
      *out = MaterialPreset::Glass;
      return true;
    default:
      return false;  // NONE / unknown
  }
}

// Map a SONARE_REVERB_MODEL_* selector onto sonare::acoustic::ReverbModel.
// SONARE_REVERB_MODEL_DEFAULT (0, the zero-initialized value) and any unknown
// selector resolve to the C++ library default (Eyring), so a {}-zeroed config
// matches the C++ struct defaults and the high-level bindings' preferEyring
// default; only an explicit SABINE selects Sabine.
sonare::acoustic::ReverbModel reverb_model_from_int(int selector) {
  using sonare::acoustic::ReverbModel;
  return selector == SONARE_REVERB_MODEL_SABINE ? ReverbModel::Sabine : ReverbModel::Eyring;
}

// Build a uniform shoebox whose single wall material is chosen by precedence:
//   material_preset (non-zero) > per-band absorption array > scalar absorption.
// All six walls share the resulting material (this ABI exposes only uniform
// rooms; per-wall mesh materials are not reachable here yet).
sonare::acoustic::ShoeboxRoom make_room(float length, float width, float height, float absorption,
                                        const float* absorption_bands, size_t absorption_band_count,
                                        const float* scattering_bands, size_t scattering_band_count,
                                        int material_preset) {
  using namespace sonare::acoustic;
  const sonare::RoomDimensions dims{length, width, height};

  MaterialPreset preset{};
  if (preset_from_int(material_preset, &preset)) {
    ShoeboxRoom room;
    room.dims = dims;
    const Material wall = make_material(preset);
    for (Material& w : room.walls) w = wall;
    return room;
  }

  if (absorption_bands != nullptr && absorption_band_count > 0) {
    ShoeboxRoom room;
    room.dims = dims;
    Material wall;
    wall.absorption.reserve(absorption_band_count);
    for (size_t i = 0; i < absorption_band_count; ++i) {
      wall.absorption.push_back(std::clamp(absorption_bands[i], 0.0f, 0.999f));
    }
    wall.scattering.reserve(absorption_band_count);
    for (size_t i = 0; i < absorption_band_count; ++i) {
      const float scattering =
          scattering_bands != nullptr && i < scattering_band_count ? scattering_bands[i] : 0.0f;
      wall.scattering.push_back(std::clamp(scattering, 0.0f, 1.0f));
    }
    for (Material& w : room.walls) w = wall;
    return room;
  }

  // Back-compat scalar path (unchanged behaviour for zeroed optional fields).
  return uniform_shoebox(dims, absorption);
}

// Heap-copies a float vector into a caller-owned array (NULL when empty).
float* copy_bands(const std::vector<float>& values, size_t* count) {
  *count = values.size();
  return sonare_c_detail::copy_vector(values);
}

// RIR synthesis reports geometry and synthesis limits as non-fatal core
// diagnostics so a caller can inspect the result. Preserve their first Error
// and Warning in the matching C-ABI channels as well: a C caller (and the
// high-level CLIs) otherwise only sees `has_error` and has no truthful
// explanation of a failed or clamped request.
void publish_rir_diagnostics(const std::vector<sonare::Diagnostic>& diagnostics) {
  for (const sonare::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity != sonare::Diagnostic::Severity::Error) continue;
    const std::string detail = diagnostic.code + ": " + diagnostic.message;
    sonare_c_detail::set_last_error(detail.c_str());
    break;
  }
  for (const sonare::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity != sonare::Diagnostic::Severity::Warning) continue;
    const std::string detail = diagnostic.code + ": " + diagnostic.message;
    sonare_c_detail::set_last_warning(detail.c_str());
    break;
  }
}
#endif

}  // namespace

SonareError sonare_synthesize_rir(const SonareRirSynthConfig* config, int sample_rate,
                                  SonareRirSynthResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  sonare_c_detail::clear_last_warning();
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate < sonare_c_detail::kMinSampleRate ||
      sample_rate > sonare_c_detail::kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto finite = [](float value) { return std::isfinite(value); };
  const auto unit = [&](float value) { return finite(value) && value >= 0.0f && value <= 1.0f; };
  if (!finite(config->length_m) || !finite(config->width_m) || !finite(config->height_m) ||
      !finite(config->source_x) || !finite(config->source_y) || !finite(config->source_z) ||
      !finite(config->listener_x) || !finite(config->listener_y) || !finite(config->listener_z) ||
      !unit(config->absorption) || config->ism_order < 0 || !std::isfinite(config->max_seconds) ||
      config->max_seconds < 0.0f || config->max_seconds > sonare::acoustic::kMaxRirSeconds ||
      !std::isfinite(config->mixing_time_ms) || config->mixing_time_ms < 0.0f ||
      config->mixing_time_ms > sonare::acoustic::kMaxRirMixingTimeMs ||
      !std::isfinite(config->crossfade_ms) || config->crossfade_ms < 0.0f ||
      config->crossfade_ms > sonare::acoustic::kMaxRirCrossfadeMs ||
      config->absorption_band_count > sonare::acoustic::kMaxMaterialBands ||
      config->scattering_band_count > sonare::acoustic::kMaxMaterialBands ||
      (config->absorption_band_count > 0 && config->absorption_bands == nullptr) ||
      (config->scattering_band_count > 0 && config->scattering_bands == nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < config->absorption_band_count; ++i) {
    if (!unit(config->absorption_bands[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < config->scattering_band_count; ++i) {
    if (!unit(config->scattering_bands[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->rir = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->has_error = 0;

  SONARE_C_TRY
  using namespace sonare::acoustic;
  const ShoeboxRoom room =
      make_room(config->length_m, config->width_m, config->height_m, config->absorption,
                config->absorption_bands, config->absorption_band_count, config->scattering_bands,
                config->scattering_band_count, config->material_preset);
  const SourceListener placement{{config->source_x, config->source_y, config->source_z},
                                 {config->listener_x, config->listener_y, config->listener_z}};
  RirSynthConfig rc;
  rc.ism_order = config->ism_order < 0 ? 0 : config->ism_order;
  rc.late_model = reverb_model_from_int(config->late_model);
  // seed == 0 means "keep the library default" (1), so a zero-initialized POD
  // produces the same deterministic RIR as the C++/Node/Python/WASM defaults
  // instead of silently seeding with 0.
  if (config->seed != 0) rc.seed = config->seed;
  rc.max_seconds = config->max_seconds;
  rc.mixing_time_ms = config->mixing_time_ms;
  // crossfade_ms == 0 means "keep the library default"; a true zero crossfade is
  // not a useful synthesis setting, so a zeroed POD preserves the C++ default.
  if (config->crossfade_ms > 0.0f) rc.crossfade_ms = config->crossfade_ms;
  rc.air_absorption_enabled = config->air_absorption_enabled != 0;
  // Zero climate values keep the ISO reference (20 degC / 50 % RH) on the same
  // "0 means the library default" rule as seed and crossfade_ms, so enabling
  // air absorption on an otherwise zeroed POD gives the documented climate
  // rather than a silent 0 degC / 0 % one. An implausible value is left for the
  // core to diagnose (acoustic.invalid_air_absorption), which keeps the failure
  // in the same has_error channel as the geometry checks.
  if (config->air_temperature_c != 0.0f) rc.air.temperature_c = config->air_temperature_c;
  if (config->air_humidity_percent != 0.0f) {
    rc.air.humidity_percent = config->air_humidity_percent;
  }

  const RirSynthResult res = synthesize_rir(room, placement, sample_rate, rc);
  out->has_error = has_error(res.diagnostics) ? 1 : 0;
  publish_rir_diagnostics(res.diagnostics);
  out->length = res.rir.size();
  if (!res.rir.empty()) {
    out->rir = new float[res.rir.size()];
    std::memcpy(out->rir, res.rir.data(), res.rir.size() * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(config, sample_rate, out);
#endif
}

void sonare_free_rir_synth_result(SonareRirSynthResult* result) {
  if (!result) return;
  delete[] result->rir;
  result->rir = nullptr;
  result->length = 0;
}

SonareError sonare_estimate_room(const float* samples, size_t length, int sample_rate,
                                 const SonareRoomEstimateConfig* config, SonareRoomEstimate* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  out->absorption_bands = nullptr;
  out->rt60_bands = nullptr;
  out->band_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    sonare::RoomEstimateConfig cfg;
    cfg.aspect_hint_lw = config->aspect_hint_lw == 0.0f ? 1.0f : config->aspect_hint_lw;
    cfg.aspect_hint_lh = config->aspect_hint_lh == 0.0f ? 1.0f : config->aspect_hint_lh;
    cfg.reference_absorption = config->reference_absorption;
    cfg.prefer_eyring = config->prefer_eyring != 0;
    if (config->n_octave_bands != 0) cfg.acoustic.n_octave_bands = config->n_octave_bands;
    if (config->min_decay_db != 0.0f) cfg.acoustic.min_decay_db = config->min_decay_db;
    if (config->noise_floor_margin_db != 0.0f) {
      cfg.acoustic.noise_floor_margin_db = config->noise_floor_margin_db;
    }
    switch (config->mode) {
      case SONARE_ACOUSTIC_MODE_BLIND:
        cfg.acoustic.mode = sonare::AcousticConfig::Mode::Blind;
        break;
      case SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE:
        cfg.acoustic.mode = sonare::AcousticConfig::Mode::ImpulseResponse;
        break;
      default:
        cfg.acoustic.mode = sonare::AcousticConfig::Mode::Auto;
        break;
    }

    const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
    out->volume = est.volume;
    out->length_m = est.dims.length;
    out->width_m = est.dims.width;
    out->height_m = est.dims.height;
    out->drr_db = est.drr_db;
    out->confidence = est.confidence;
    // The two band vectors are independent estimates (absorption from the
    // inverse problem, RT60 from the decay fit) and either can legitimately
    // fail to converge on its own. band_count is documented as the length of
    // BOTH arrays, so truncating to the shorter one silently dropped a
    // fully-computed array whenever its sibling failed — exactly when a
    // caller most wants the surviving data. Pad the shorter side with NaN
    // instead so both arrays reach band_count and neither is ever discarded
    // because the other came back empty.
    const size_t max_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
    auto pad_with_nan = [max_count](std::vector<float> values) {
      values.resize(max_count, std::numeric_limits<float>::quiet_NaN());
      return values;
    };
    size_t a_count = 0;
    size_t r_count = 0;
    out->absorption_bands = copy_bands(pad_with_nan(est.absorption_bands), &a_count);
    out->rt60_bands = copy_bands(pad_with_nan(est.rt60_bands), &r_count);
    out->band_count = max_count;
    return SONARE_OK;
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out);
#endif
}

void sonare_free_room_estimate(SonareRoomEstimate* result) {
  if (!result) return;
  delete[] result->absorption_bands;
  delete[] result->rt60_bands;
  result->absorption_bands = nullptr;
  result->rt60_bands = nullptr;
  result->band_count = 0;
}

SonareError sonare_room_morph(const float* samples, size_t length, int sample_rate,
                              const SonareRoomMorphConfig* config, float** out,
                              size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  const auto unit = [](float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  };
  if (!unit(config->absorption) || !unit(config->source_tail_suppression) || !unit(config->wet) ||
      config->absorption_band_count > sonare::acoustic::kMaxMaterialBands ||
      config->scattering_band_count > sonare::acoustic::kMaxMaterialBands ||
      (config->absorption_band_count > 0 && config->absorption_bands == nullptr) ||
      (config->scattering_band_count > 0 && config->scattering_bands == nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < config->absorption_band_count; ++i) {
    if (!unit(config->absorption_bands[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < config->scattering_band_count; ++i) {
    if (!unit(config->scattering_bands[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  return run_mono_offline(
      samples, length, sample_rate, out, out_length, [&](const Audio& audio) -> Audio {
        sonare::effects::acoustic::RoomMorphConfig cfg;
        cfg.target = make_room(config->length_m, config->width_m, config->height_m,
                               config->absorption, config->absorption_bands,
                               config->absorption_band_count, config->scattering_bands,
                               config->scattering_band_count, config->material_preset);
        cfg.placement = {{config->source_x, config->source_y, config->source_z},
                         {config->listener_x, config->listener_y, config->listener_z}};
        cfg.source_tail_suppression = config->source_tail_suppression;
        cfg.wet = config->wet;
        cfg.ism_order = config->ism_order;
        // seed == 0 keeps the library default (1); see synthesize_rir above.
        if (config->seed != 0) cfg.seed = config->seed;
        cfg.max_seconds = config->max_seconds;
        cfg.late_model = reverb_model_from_int(config->late_model);
        cfg.mixing_time_ms = config->mixing_time_ms;  // 0 = auto (~sqrt(V) ms)
        // crossfade_ms == 0 preserves the C++ default (a true zero crossfade is
        // not a useful setting), matching the RIR-synth ABI convention.
        if (config->crossfade_ms != 0.0f) cfg.crossfade_ms = config->crossfade_ms;
        // Air absorption on the target room; zero climate values keep the ISO
        // reference, exactly as in synthesize_rir above. Here the core rejects an
        // implausible climate by throwing, which run_mono_offline maps to
        // SONARE_ERROR_INVALID_PARAMETER.
        cfg.air_absorption_enabled = config->air_absorption_enabled != 0;
        if (config->air_temperature_c != 0.0f) cfg.air.temperature_c = config->air_temperature_c;
        if (config->air_humidity_percent != 0.0f) {
          cfg.air.humidity_percent = config->air_humidity_percent;
        }
        return sonare::effects::acoustic::room_morph(audio, cfg);
      });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out, out_length);
#endif
}
