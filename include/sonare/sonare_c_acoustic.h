#pragma once

/// @file sonare_c_acoustic.h
/// @brief C ABI for the geometric room-acoustics module: RIR synthesis from
///        room geometry, blind equivalent-room estimation, and the offline
///        room-character morph. The symbols are always exported by the shared
///        C ABI; feature-off builds return SONARE_ERROR_NOT_SUPPORTED.
///
/// The streaming engines (RoomReverb, RoomMorphProcessor) are reachable through
/// the generic insert API by name ("effects.reverb.room",
/// "effects.acoustic.roomMorph"); only the offline entry points live here.

#include <stddef.h>

#include "sonare_c_types.h"

/// ABI version of the flat acoustic POD structs below. Bump on any layout change.
#define SONARE_ACOUSTIC_ABI_VERSION 3u

/// Statistical late-reverberation RT60 model for RIR synthesis / room morph
/// (SonareRirSynthConfig::late_model, SonareRoomMorphConfig::late_model).
/// SONARE_REVERB_MODEL_DEFAULT is 0 so a zero-initialized config selects the
/// library default (Eyring), matching the C++ struct defaults
/// (RirSynthConfig::late_model / RoomMorphConfig::late_model) and every
/// high-level binding's preferEyring=true default. SABINE/EYRING select a model
/// explicitly. Mirrors sonare::acoustic::ReverbModel for the explicit values.
#define SONARE_REVERB_MODEL_DEFAULT 0
#define SONARE_REVERB_MODEL_SABINE 1
#define SONARE_REVERB_MODEL_EYRING 2

/// Building-material preset selector for the uniform-room wall material
/// (SonareRirSynthConfig::material_preset / SonareRoomMorphConfig::material_preset).
/// Mirrors sonare::acoustic::MaterialPreset. SONARE_MATERIAL_PRESET_NONE is 0 so a
/// zero-initialized config leaves the preset unused (back-compat): the per-band
/// array or scalar absorption then applies.
#define SONARE_MATERIAL_PRESET_NONE 0
#define SONARE_MATERIAL_PRESET_CONCRETE 1
#define SONARE_MATERIAL_PRESET_WOOD 2
#define SONARE_MATERIAL_PRESET_CURTAIN 3
#define SONARE_MATERIAL_PRESET_CARPET 4
#define SONARE_MATERIAL_PRESET_GLASS 5

/// Blind/IR acoustic-analyzer routing mode for room estimation
/// (SonareRoomEstimateConfig::mode). Mirrors AcousticConfig::Mode.
#define SONARE_ACOUSTIC_MODE_AUTO 0
#define SONARE_ACOUSTIC_MODE_BLIND 1
#define SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE 2

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Shoebox geometry + placement + synthesis controls for RIR synthesis.
///
/// The room is an axis-aligned box [0,length] x [0,width] x [0,height] with a
/// uniform wall material applied to all six walls. The wall material is chosen by
/// this precedence: a named `material_preset` (non-zero) wins; otherwise a
/// non-NULL `absorption_bands` per-octave-band array; otherwise the scalar
/// `absorption`.
///
/// NOTE: this C ABI exposes only uniform (single-material) shoebox rooms — one
/// material on every wall, selected as uniform scalar, per-band, or preset
/// absorption. Per-wall mesh materials (a different material per surface) are not
/// reachable through this ABI yet.
/// TODO(acoustic-c-abi): expose per-wall / polyhedral-mesh materials.
typedef struct {
  float length_m;
  float width_m;
  float height_m;
  float source_x;
  float source_y;
  float source_z;
  float listener_x;
  float listener_y;
  float listener_z;
  float absorption;     /* uniform wall absorption, clamped to [0, 0.999] */
  float max_seconds;    /* hard RIR length cap; 0 = natural length */
  float mixing_time_ms; /* early/late crossover (ms); 0 = auto (~sqrt(V) ms) */
  float crossfade_ms;   /* equal-power crossfade width around the mixing time (ms) */
  int ism_order;        /* image-source reflection order (>= 0) */
  int late_model;       /* SONARE_REVERB_MODEL_* ; DEFAULT (0) = Eyring */
  unsigned int seed;    /* deterministic late-tail seed */
  /* Optional per-octave-band wall absorption/scattering (125/250/500/1k/2k/4k..
   * Hz). When absorption_bands != NULL and absorption_band_count > 0 it
   * overrides the scalar `absorption` (unless material_preset selects a preset).
   * scattering_bands is optional and applied band-wise when present; missing
   * scattering bands default to 0. */
  const float* absorption_bands;
  size_t absorption_band_count;
  const float* scattering_bands;
  size_t scattering_band_count;
  int material_preset; /* SONARE_MATERIAL_PRESET_* ; NONE (0) = use bands/scalar */
} SonareRirSynthConfig;

/// @brief Synthesized room impulse response (mono). Free with
///        sonare_free_rir_synth_result.
typedef struct {
  float* rir; /* length samples; NULL on error */
  size_t length;
  int sample_rate;
  int has_error; /* 1 when geometry validation failed (rir is empty) */
} SonareRirSynthResult;

/// @brief Priors + analysis settings for blind room estimation.
typedef struct {
  float aspect_hint_lw;        /* length/width shape prior */
  float aspect_hint_lh;        /* length/height shape prior */
  float reference_absorption;  /* absorption prior anchoring the volume scale */
  float min_decay_db;          /* analyzer decay-fit span (dB); 0 = library default */
  float noise_floor_margin_db; /* analyzer noise-floor margin (dB); 0 = library default */
  int prefer_eyring;           /* 1 = Eyring model, 0 = Sabine */
  int n_octave_bands;          /* analyzer band count (0 = library default) */
  int mode;                    /* SONARE_ACOUSTIC_MODE_* analyzer routing */
} SonareRoomEstimateConfig;

/// @brief Equivalent-room estimate. Free with sonare_free_room_estimate.
typedef struct {
  float volume;   /* equivalent interior volume (m^3) */
  float length_m; /* representative dimensions (m) */
  float width_m;
  float height_m;
  float drr_db;            /* direct-to-reverberant ratio (dB) */
  float confidence;        /* honest [0,1] support for the estimate */
  float* absorption_bands; /* per-octave-band mean absorption; NULL if none */
  float* rt60_bands;       /* per-octave-band RT60 (s); NULL if none */
  size_t band_count;       /* length of both band arrays */
} SonareRoomEstimate;

/// @brief Target room + placement + morph controls for the offline morph.
///
/// The target wall material follows the same precedence as SonareRirSynthConfig
/// (material_preset > absorption_bands > scalar absorption); only uniform
/// (single-material) target rooms are reachable through this ABI.
typedef struct {
  float length_m;
  float width_m;
  float height_m;
  float source_x;
  float source_y;
  float source_z;
  float listener_x;
  float listener_y;
  float listener_z;
  float absorption;              /* target uniform wall absorption */
  float source_tail_suppression; /* [0,1] gentle source-reverb reduction */
  float wet;                     /* [0,1] target-room mix */
  float max_seconds;             /* target RIR length cap; 0 = natural */
  float mixing_time_ms;          /* early/late crossover (ms); 0 = auto (~sqrt(V) ms) */
  float crossfade_ms;            /* equal-power crossfade width around mixing time (ms) */
  int ism_order;
  int late_model; /* SONARE_REVERB_MODEL_* ; DEFAULT (0) = Eyring */
  unsigned int seed;
  /* Optional per-octave-band target-wall absorption/scattering; see
   * SonareRirSynthConfig. */
  const float* absorption_bands;
  size_t absorption_band_count;
  const float* scattering_bands;
  size_t scattering_band_count;
  int material_preset; /* SONARE_MATERIAL_PRESET_* ; NONE (0) = use bands/scalar */
} SonareRoomMorphConfig;

#ifdef __cplusplus
// Each config's scalar prefix is a packed run of 4-byte members (float/int/
// unsigned), followed by the optional per-band-array + preset tail. The prefix
// length is asserted by the offset of the first trailing member, and the tail
// layout by member-relative offsets, so any padding/alignment/reorder change is
// caught. SonareRoomEstimateConfig stays a pure-float-sized POD.
//
// SonareRirSynthConfig prefix: 13 floats + 3 ints/unsigned = 16 four-byte members.
static_assert(offsetof(SonareRirSynthConfig, absorption_bands) == 16u * sizeof(float),
              "SonareRirSynthConfig scalar prefix layout changed");
static_assert(offsetof(SonareRirSynthConfig, scattering_bands) ==
                  offsetof(SonareRirSynthConfig, absorption_band_count) + sizeof(size_t),
              "SonareRirSynthConfig scattering tail layout changed");
static_assert(offsetof(SonareRirSynthConfig, material_preset) ==
                  offsetof(SonareRirSynthConfig, scattering_band_count) + sizeof(size_t),
              "SonareRirSynthConfig optional tail layout changed");
static_assert(sizeof(SonareRoomEstimateConfig) == 8u * sizeof(float),
              "SonareRoomEstimateConfig unexpected size");
// SonareRoomMorphConfig prefix: 15 floats + 3 ints/unsigned = 18 four-byte members
// (gained late_model/mixing_time_ms/crossfade_ms over the v1 layout).
static_assert(offsetof(SonareRoomMorphConfig, absorption_bands) == 18u * sizeof(float),
              "SonareRoomMorphConfig scalar prefix layout changed");
static_assert(offsetof(SonareRoomMorphConfig, scattering_bands) ==
                  offsetof(SonareRoomMorphConfig, absorption_band_count) + sizeof(size_t),
              "SonareRoomMorphConfig scattering tail layout changed");
static_assert(offsetof(SonareRoomMorphConfig, material_preset) ==
                  offsetof(SonareRoomMorphConfig, scattering_band_count) + sizeof(size_t),
              "SonareRoomMorphConfig optional tail layout changed");
#endif

/// @brief Synthesize a room impulse response from shoebox geometry.
/// @param config      Geometry + placement + synthesis controls.
/// @param sample_rate Output sample rate (Hz).
/// @param out         Receives the synthesized RIR (caller frees).
/// Invalid geometry is not an error: @c out->has_error is set to 1 and the RIR
/// is left empty (length 0), mirroring the C++ diagnostics contract.
SonareError sonare_synthesize_rir(const SonareRirSynthConfig* config, int sample_rate,
                                  SonareRirSynthResult* out);
void sonare_free_rir_synth_result(SonareRirSynthResult* result);

/// @brief Estimate an equivalent room from a recording (or impulse response).
SonareError sonare_estimate_room(const float* samples, size_t length, int sample_rate,
                                 const SonareRoomEstimateConfig* config, SonareRoomEstimate* out);
void sonare_free_room_estimate(SonareRoomEstimate* result);

/// @brief Offline room-character morph toward a target room.
/// @param out        Receives the morphed mono buffer (caller frees via
///                   sonare_free_floats). Length is the input length plus the
///                   target room's reverb tail.
SonareError sonare_room_morph(const float* samples, size_t length, int sample_rate,
                              const SonareRoomMorphConfig* config, float** out, size_t* out_length);

#ifdef __cplusplus
}
#endif
