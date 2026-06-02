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
#define SONARE_ACOUSTIC_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Shoebox geometry + placement + synthesis controls for RIR synthesis.
///
/// The room is an axis-aligned box [0,length] x [0,width] x [0,height] with a
/// single uniform wall absorption. Source/listener are points inside it.
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
  float absorption;  /* uniform wall absorption, clamped to [0, 0.999] */
  float max_seconds; /* hard RIR length cap; 0 = natural length */
  int ism_order;     /* image-source reflection order (>= 0) */
  unsigned int seed; /* deterministic late-tail seed */
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
  float aspect_hint_lw;       /* length/width shape prior */
  float aspect_hint_lh;       /* length/height shape prior */
  float reference_absorption; /* absorption prior anchoring the volume scale */
  int prefer_eyring;          /* 1 = Eyring model, 0 = Sabine */
  int n_octave_bands;         /* analyzer band count (0 = library default) */
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
  int ism_order;
  unsigned int seed;
} SonareRoomMorphConfig;

#ifdef __cplusplus
static_assert(sizeof(SonareRirSynthConfig) == 13u * sizeof(float),
              "SonareRirSynthConfig unexpected size");
static_assert(sizeof(SonareRoomEstimateConfig) == 5u * sizeof(float),
              "SonareRoomEstimateConfig unexpected size");
static_assert(sizeof(SonareRoomMorphConfig) == 15u * sizeof(float),
              "SonareRoomMorphConfig unexpected size");
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
