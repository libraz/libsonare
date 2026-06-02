#include "sonare_c_acoustic.h"

#if defined(SONARE_WITH_ACOUSTIC_SIM)
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
sonare::acoustic::ShoeboxRoom make_room(float length, float width, float height, float absorption) {
  return sonare::acoustic::uniform_shoebox({length, width, height}, absorption);
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
      make_room(config->length_m, config->width_m, config->height_m, config->absorption);
  const SourceListener placement{{config->source_x, config->source_y, config->source_z},
                                 {config->listener_x, config->listener_y, config->listener_z}};
  RirSynthConfig rc;
  rc.ism_order = config->ism_order < 0 ? 0 : config->ism_order;
  rc.seed = config->seed;
  rc.max_seconds = config->max_seconds;

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
        cfg.target =
            make_room(config->length_m, config->width_m, config->height_m, config->absorption);
        cfg.placement = {{config->source_x, config->source_y, config->source_z},
                         {config->listener_x, config->listener_y, config->listener_z}};
        cfg.source_tail_suppression = config->source_tail_suppression;
        cfg.wet = config->wet;
        cfg.ism_order = config->ism_order < 0 ? 0 : config->ism_order;
        cfg.seed = config->seed;
        cfg.max_seconds = config->max_seconds;
        return sonare::effects::acoustic::room_morph(audio, cfg);
      });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out, out_length);
#endif
}
