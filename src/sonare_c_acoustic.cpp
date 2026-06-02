#include "sonare_c_acoustic.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
#include <algorithm>
#include <cstring>
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

// Build a uniform shoebox whose single wall material is chosen by precedence:
//   material_preset (non-zero) > per-band absorption array > scalar absorption.
// All six walls share the resulting material (this ABI exposes only uniform
// rooms; per-wall mesh materials are not reachable here yet).
sonare::acoustic::ShoeboxRoom make_room(float length, float width, float height, float absorption,
                                        const float* absorption_bands, size_t absorption_band_count,
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
    // Keep the Material invariant absorption.size() == scattering.size(); the
    // synthesis paths read only absorption, scattering stays specular (0).
    wall.scattering.assign(absorption_band_count, 0.0f);
    for (Material& w : room.walls) w = wall;
    return room;
  }

  // Back-compat scalar path (unchanged behaviour for zeroed optional fields).
  return uniform_shoebox(dims, absorption);
}

// Heap-copies a float vector into a caller-owned array (NULL when empty).
float* copy_bands(const std::vector<float>& values, size_t* count) {
  *count = values.size();
  if (values.empty()) return nullptr;
  float* out = new float[values.size()];
  std::memcpy(out, values.data(), values.size() * sizeof(float));
  return out;
}
#endif

}  // namespace

SonareError sonare_synthesize_rir(const SonareRirSynthConfig* config, int sample_rate,
                                  SonareRirSynthResult* out) {
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate < sonare_c_detail::kMinSampleRate ||
      sample_rate > sonare_c_detail::kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->rir = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->has_error = 0;

  SONARE_C_TRY
  using namespace sonare::acoustic;
  const ShoeboxRoom room =
      make_room(config->length_m, config->width_m, config->height_m, config->absorption,
                config->absorption_bands, config->absorption_band_count, config->material_preset);
  const SourceListener placement{{config->source_x, config->source_y, config->source_z},
                                 {config->listener_x, config->listener_y, config->listener_z}};
  RirSynthConfig rc;
  rc.ism_order = config->ism_order < 0 ? 0 : config->ism_order;
  rc.late_model =
      config->late_model == SONARE_REVERB_MODEL_SABINE ? ReverbModel::Sabine : ReverbModel::Eyring;
  rc.seed = config->seed;
  rc.max_seconds = config->max_seconds;
  rc.mixing_time_ms = config->mixing_time_ms;
  // crossfade_ms == 0 means "keep the library default"; a true zero crossfade is
  // not a useful synthesis setting, so a zeroed POD preserves the C++ default.
  if (config->crossfade_ms > 0.0f) rc.crossfade_ms = config->crossfade_ms;

  const RirSynthResult res = synthesize_rir(room, placement, sample_rate, rc);
  out->has_error = has_error(res.diagnostics) ? 1 : 0;
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
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  if (!config || !out) return SONARE_ERROR_INVALID_PARAMETER;
  out->absorption_bands = nullptr;
  out->rt60_bands = nullptr;
  out->band_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    sonare::RoomEstimateConfig cfg;
    cfg.aspect_hint_lw = config->aspect_hint_lw > 0.0f ? config->aspect_hint_lw : 1.0f;
    cfg.aspect_hint_lh = config->aspect_hint_lh > 0.0f ? config->aspect_hint_lh : 1.0f;
    cfg.reference_absorption = config->reference_absorption;
    cfg.prefer_eyring = config->prefer_eyring != 0;
    if (config->n_octave_bands > 0) cfg.acoustic.n_octave_bands = config->n_octave_bands;
    if (config->min_decay_db > 0.0f) cfg.acoustic.min_decay_db = config->min_decay_db;
    if (config->noise_floor_margin_db > 0.0f) {
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
    size_t a_count = 0;
    size_t r_count = 0;
    out->absorption_bands = copy_bands(est.absorption_bands, &a_count);
    out->rt60_bands = copy_bands(est.rt60_bands, &r_count);
    // The two band vectors share a length; report the shorter so a consumer that
    // iterates to band_count can never read past the smaller array.
    out->band_count = a_count < r_count ? a_count : r_count;
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
#if defined(SONARE_WITH_ACOUSTIC_SIM)
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  return run_mono_offline(
      samples, length, sample_rate, out, out_length, [&](const Audio& audio) -> Audio {
        sonare::effects::acoustic::RoomMorphConfig cfg;
        cfg.target = make_room(config->length_m, config->width_m, config->height_m,
                               config->absorption, config->absorption_bands,
                               config->absorption_band_count, config->material_preset);
        cfg.placement = {{config->source_x, config->source_y, config->source_z},
                         {config->listener_x, config->listener_y, config->listener_z}};
        cfg.source_tail_suppression = config->source_tail_suppression;
        cfg.wet = config->wet;
        cfg.ism_order = config->ism_order < 0 ? 0 : config->ism_order;
        cfg.seed = config->seed;
        cfg.max_seconds = config->max_seconds;
        cfg.late_model = config->late_model == SONARE_REVERB_MODEL_SABINE
                             ? sonare::acoustic::ReverbModel::Sabine
                             : sonare::acoustic::ReverbModel::Eyring;
        cfg.mixing_time_ms = config->mixing_time_ms;  // 0 = auto (~sqrt(V) ms)
        // crossfade_ms == 0 preserves the C++ default (a true zero crossfade is
        // not a useful setting), matching the RIR-synth ABI convention.
        if (config->crossfade_ms > 0.0f) cfg.crossfade_ms = config->crossfade_ms;
        return sonare::effects::acoustic::room_morph(audio, cfg);
      });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out, out_length);
#endif
}
