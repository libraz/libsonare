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
#define SONARE_ACOUSTIC_ABI_VERSION 6u

/// Building-material preset selector for the uniform-room wall material
/// (SonareRirSynthConfig::material_preset / SonareRoomMorphConfig::material_preset).
/// Mirrors sonare::acoustic::MaterialPreset. SONARE_MATERIAL_PRESET_NONE leaves the
/// preset unused, so the per-band array or scalar absorption applies; the `_init`
/// functions set it. Any other value is refused with SONARE_ERROR_INVALID_PARAMETER.
/// No preset; the per-band array or scalar absorption applies.
#define SONARE_MATERIAL_PRESET_NONE 0
/// Concrete walls.
#define SONARE_MATERIAL_PRESET_CONCRETE 1
/// Wooden walls.
#define SONARE_MATERIAL_PRESET_WOOD 2
/// Curtain-covered walls.
#define SONARE_MATERIAL_PRESET_CURTAIN 3
/// Carpeted walls.
#define SONARE_MATERIAL_PRESET_CARPET 4
/// Glass walls.
#define SONARE_MATERIAL_PRESET_GLASS 5

/// Blind/IR acoustic-analyzer routing mode for room estimation
/// (SonareRoomEstimateConfig::mode). Mirrors AcousticConfig::Mode. Any other
/// value is refused with SONARE_ERROR_INVALID_PARAMETER.
/// Choose blind or impulse-response analysis from the input.
#define SONARE_ACOUSTIC_MODE_AUTO 0
/// Treat the input as a reverberant recording and estimate blindly.
#define SONARE_ACOUSTIC_MODE_BLIND 1
/// Treat the input as a measured impulse response.
#define SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE 2

#ifdef __cplusplus
extern "C" {
#endif

/// @brief A point in the room, in metres along the room's x, y and z axes.
typedef struct {
  float x;  ///< Position along the room length, in metres.
  float y;  ///< Position along the room width, in metres.
  float z;  ///< Position along the room height, in metres.
} SonareRoomPlacement;

/// @brief Shoebox geometry + placement + synthesis controls for RIR synthesis.
///
/// Prepare one with sonare_rir_synth_config_init, which sets struct_size and every
/// field's library default; every field is then read literally (a 0 is a 0).
///
/// The room is an axis-aligned box [0,length] x [0,width] x [0,height] with a
/// uniform wall material applied to all six walls. The wall material is chosen by
/// this precedence: a named `material_preset` (non-zero) wins; otherwise a
/// non-NULL `band_absorption` per-octave-band array; otherwise the scalar
/// `absorption`. The band arrays stay the caller's and are read during the call
/// only; the library never frees them.
///
/// NOTE: this C ABI exposes only uniform (single-material) shoebox rooms — one
/// material on every wall, selected as uniform scalar, per-band, or preset
/// absorption. The core supports arbitrary polyhedral meshes with a distinct
/// material per surface (see acoustic/image_source.h `polyhedral_image_sources`),
/// but that is core-internal by design and intentionally not exposed on any
/// public surface (C ABI / Node / Python / WASM): the shoebox model covers the
/// supported room-acoustics use cases and keeps the ABI mesh-free.
typedef struct {
  /// sizeof(SonareRirSynthConfig), set by sonare_rir_synth_config_init. A smaller value is
  /// refused with SONARE_ERROR_INVALID_PARAMETER.
  uint32_t struct_size;
  float length_m;  ///< Room extent along x in metres.
  float width_m;   ///< Room extent along y in metres.
  float height_m;  ///< Room extent along z in metres.
  /// Source position; must lie inside the room. All three components NaN means not given: the
  /// default placement scaled to the room. A position only partly NaN is refused.
  SonareRoomPlacement source;
  /// Listener position, with the same rule as @c source.
  SonareRoomPlacement listener;
  /// Uniform wall absorption coefficient, clamped to [0, 0.999].
  float absorption;
  /// Hard RIR length cap in seconds; 0 = natural length.
  float max_sec;
  /// Early/late crossover in milliseconds; 0 = automatic (about sqrt(V) ms).
  float mixing_time_ms;
  /// Equal-power crossfade width around the mixing time, in milliseconds.
  float crossfade_ms;
  /// Image-source reflection order (>= 0).
  int ism_order;
  /// Non-zero selects the Eyring late-tail RT60 model, 0 the Sabine one; _init sets 1.
  int prefer_eyring;
  /// Seed of the deterministic late tail.
  unsigned int seed;
  /// Optional per-octave-band wall absorption (125, 250, 500, 1k, 2k, 4k.. Hz). When non-NULL
  /// with band_absorption_count > 0 it overrides the scalar absorption unless material_preset
  /// selects a preset. The late tail's decay time runs continuously between octave centres,
  /// so where absorption changes steeply between octaves the octave-band RT60 measured back
  /// leans toward the slower neighbour, as in a real room; the design value holds at the
  /// octave centre.
  const float* band_absorption;
  /// Number of entries in band_absorption.
  size_t band_absorption_count;
  /// Optional per-octave-band wall scattering, applied band-wise; missing bands default to 0.
  /// Independent of band_absorption and material_preset: it is applied to whichever material
  /// the absorption precedence selected, so a preset or scalar-absorption room can still be
  /// given rough walls.
  const float* band_scattering;
  /// Number of entries in band_scattering.
  size_t band_scattering_count;
  /// A @ref SONARE_MATERIAL_PRESET_NONE or other SONARE_MATERIAL_PRESET_* value; non-zero wins
  /// over the band arrays and the scalar absorption.
  int material_preset;
  /// Non-zero applies the ISO 9613-1 atmospheric absorption to the late tail; _init sets 1.
  /// While on, the per-band late RT60 gains the 4 m V air term, which mainly shortens the
  /// high bands of a large room. Out-of-range climate values are refused with
  /// SONARE_ERROR_INVALID_PARAMETER, coded acoustic.invalid_air_absorption in
  /// sonare_last_diagnostic.
  int air_absorption_enabled;
  /// Air temperature in degrees Celsius for the air absorption (default 20, the ISO reference).
  float air_temperature_c;
  /// Relative humidity in percent for the air absorption (default 50, the ISO reference).
  float air_humidity_percent;
} SonareRirSynthConfig;

/// @brief Synthesized room impulse response (mono). Free with
///        sonare_free_rir_synth_result.
typedef struct {
  float* rir;     ///< Heap-owned mono samples; NULL on error. Freed by the result's free function.
  size_t length;  ///< Number of samples in rir.
  int sample_rate;  ///< Sample rate of rir in Hz, as passed to sonare_synthesize_rir.
} SonareRirSynthResult;

/// @brief Priors + analysis settings for blind room estimation.
///
/// Prepare one with sonare_room_estimate_config_init, which sets struct_size and every
/// field's library default; every field is then read literally and checked against its
/// domain, so a 0 in a field whose domain excludes it is refused by name.
typedef struct {
  /// sizeof(SonareRoomEstimateConfig), set by sonare_room_estimate_config_init. A smaller value
  /// is refused with SONARE_ERROR_INVALID_PARAMETER.
  uint32_t struct_size;
  float aspect_hint_lw;  ///< Length/width shape prior, finite and > 0; default 1.
  float aspect_hint_lh;  ///< Length/height shape prior, finite and > 0; default 1.
  /// Absorption prior anchoring the volume scale, in [0.01, 0.99]; default 0.15. A value
  /// outside that range is refused. The reported volume scales with the cube of the prior.
  float reference_absorption;
  float min_decay_db;           ///< Analyzer decay-fit span in dB, finite and > 0; default 30.
  float noise_floor_margin_db;  ///< Analyzer noise-floor margin in dB, finite and >= 0; default 10.
  int prefer_eyring;            ///< 1 = Eyring model, 0 = Sabine; default 1.
  int n_octave_bands;           ///< Analyzer octave-band count, >= 1; default 6.
  /// A @ref SONARE_ACOUSTIC_MODE_AUTO, @ref SONARE_ACOUSTIC_MODE_BLIND or
  /// @ref SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE value routing the analyzer; default AUTO.
  int mode;
} SonareRoomEstimateConfig;

/// @brief Equivalent-room estimate. Free with sonare_free_room_estimate. NaN is the C-level
///        "not measured"; every binding reads an unmeasured scalar as absent.
typedef struct {
  float volume;    ///< Equivalent interior volume in m^3; NaN when no decay is measurable.
  float length_m;  ///< Representative room length in metres; NaN likewise.
  float width_m;   ///< Representative room width in metres; NaN likewise.
  float height_m;  ///< Representative room height in metres; NaN likewise.
  /// Direct-to-reverberant ratio in dB; NaN when the input is silent or empty, +inf when
  /// the direct sound leaves no energy outside its window.
  float drr_db;
  float confidence;  ///< Support for the estimate, in [0, 1].
  /// Per-octave-band mean absorption; heap-owned. Absorption and RT60 are independent
  /// estimates and either can fail to converge without the other: both arrays always have
  /// band_count entries, and a failed side is NaN-filled rather than truncated. NULL only
  /// when neither estimate produced any bands.
  float* band_absorption;
  /// Per-octave-band RT60 in seconds; heap-owned, same length and NaN rule as
  /// band_absorption.
  float* rt60_bands;
  size_t band_count;  ///< Length of both band arrays.
} SonareRoomEstimate;

/// @brief Target room + placement + morph controls for the offline morph.
///
/// The target wall material follows the same precedence as SonareRirSynthConfig
/// (material_preset > band_absorption > scalar absorption); only uniform
/// (single-material) target rooms are reachable through this ABI. Prepare one with
/// sonare_room_morph_config_init; every field is then read literally. The band
/// arrays stay the caller's and are read during the call only; the library
/// never frees them.
typedef struct {
  /// sizeof(SonareRoomMorphConfig), set by sonare_room_morph_config_init. A smaller value is
  /// refused with SONARE_ERROR_INVALID_PARAMETER.
  uint32_t struct_size;
  float length_m;  ///< Target room extent along x in metres.
  float width_m;   ///< Target room extent along y in metres.
  float height_m;  ///< Target room extent along z in metres.
  /// Source position; must lie inside the target room. All three components NaN means not
  /// given: the default placement scaled to the target room. A position only partly NaN is refused.
  SonareRoomPlacement source;
  /// Listener position, with the same rule as @c source.
  SonareRoomPlacement listener;
  float absorption;  ///< Target uniform wall absorption coefficient.
  /// Gentle reduction of the source's own reverb, in [0, 1].
  float source_tail_suppression;
  float wet;  ///< Target-room mix, in [0, 1].
  /// Target RIR length cap in seconds; 0 = natural length.
  float max_sec;
  /// Early/late crossover in milliseconds; 0 = automatic (about sqrt(V) ms).
  float mixing_time_ms;
  /// Equal-power crossfade width around the mixing time, in milliseconds.
  float crossfade_ms;
  int ism_order;  ///< Image-source reflection order of the target room (>= 0).
  /// Non-zero selects the Eyring target late-tail RT60 model, 0 the Sabine one; _init sets 1.
  int prefer_eyring;
  unsigned int seed;  ///< Seed of the deterministic late tail.
  /// Optional per-octave-band target-wall absorption; see SonareRirSynthConfig.
  const float* band_absorption;
  size_t band_absorption_count;  ///< Number of entries in band_absorption.
  /// Optional per-octave-band target-wall scattering; see SonareRirSynthConfig.
  const float* band_scattering;
  size_t band_scattering_count;  ///< Number of entries in band_scattering.
  /// A SONARE_MATERIAL_PRESET_* value; non-zero wins over the band arrays and the scalar
  /// absorption.
  int material_preset;
  /// Non-zero applies atmospheric absorption to the target room's late tail; see
  /// SonareRirSynthConfig.
  int air_absorption_enabled;
  float air_temperature_c;     ///< Air temperature in degrees Celsius.
  float air_humidity_percent;  ///< Relative humidity in percent.
  /// Receiver spacing in metres for sonare_room_morph_stereo, in (0, 4]. NaN means not given:
  /// 0.5 where that pair fits the room around the listener, else the largest spacing that
  /// fits. sonare_room_morph refuses a supplied spacing because the mono request has no pair.
  float receiver_spacing_m;
} SonareRoomMorphConfig;

#ifdef __cplusplus
// Each config's scalar prefix is a packed run of 4-byte members (float/int/
// unsigned), followed by the optional per-band-array + preset tail. The prefix
// length is asserted by the offset of the first trailing member, and the tail
// layout by member-relative offsets, so any padding/alignment/reorder change is
// caught. SonareRoomEstimateConfig stays a pure-float-sized POD.
//
// SonareRirSynthConfig prefix: struct_size + 13 floats + 3 ints/unsigned = 17 four-byte
// members, padded to the pointer's alignment.
static_assert(offsetof(SonareRirSynthConfig, band_absorption) ==
                  (17u * sizeof(float) + alignof(const float*) - 1) / alignof(const float*) *
                      alignof(const float*),
              "SonareRirSynthConfig scalar prefix layout changed");
static_assert(offsetof(SonareRirSynthConfig, band_scattering) ==
                  offsetof(SonareRirSynthConfig, band_absorption_count) + sizeof(size_t),
              "SonareRirSynthConfig scattering tail layout changed");
static_assert(offsetof(SonareRirSynthConfig, material_preset) ==
                  offsetof(SonareRirSynthConfig, band_scattering_count) + sizeof(size_t),
              "SonareRirSynthConfig optional tail layout changed");
// The air-absorption group follows material_preset as a packed 4-byte run, on
// every target: pinning it member-relative keeps the check pointer-width
// agnostic (a size_t/pointer is 8 bytes natively and 4 on wasm32).
static_assert(offsetof(SonareRirSynthConfig, air_absorption_enabled) ==
                  offsetof(SonareRirSynthConfig, material_preset) + sizeof(int),
              "SonareRirSynthConfig air-absorption layout changed");
static_assert(offsetof(SonareRirSynthConfig, air_humidity_percent) ==
                  offsetof(SonareRirSynthConfig, air_temperature_c) + sizeof(float),
              "SonareRirSynthConfig air climate layout changed");
static_assert(sizeof(SonareRoomEstimateConfig) == 9u * sizeof(float),
              "SonareRoomEstimateConfig unexpected size");
// SonareRoomMorphConfig prefix: struct_size + 15 floats + 3 ints/unsigned = 19 four-byte
// members, padded to the pointer's alignment.
static_assert(offsetof(SonareRoomMorphConfig, band_absorption) ==
                  (19u * sizeof(float) + alignof(const float*) - 1) / alignof(const float*) *
                      alignof(const float*),
              "SonareRoomMorphConfig scalar prefix layout changed");
static_assert(offsetof(SonareRoomMorphConfig, band_scattering) ==
                  offsetof(SonareRoomMorphConfig, band_absorption_count) + sizeof(size_t),
              "SonareRoomMorphConfig scattering tail layout changed");
static_assert(offsetof(SonareRoomMorphConfig, material_preset) ==
                  offsetof(SonareRoomMorphConfig, band_scattering_count) + sizeof(size_t),
              "SonareRoomMorphConfig optional tail layout changed");
static_assert(offsetof(SonareRoomMorphConfig, air_absorption_enabled) ==
                  offsetof(SonareRoomMorphConfig, material_preset) + sizeof(int),
              "SonareRoomMorphConfig air-absorption layout changed");
static_assert(offsetof(SonareRoomMorphConfig, air_humidity_percent) ==
                  offsetof(SonareRoomMorphConfig, air_temperature_c) + sizeof(float),
              "SonareRoomMorphConfig air climate layout changed");
static_assert(offsetof(SonareRoomMorphConfig, receiver_spacing_m) ==
                  offsetof(SonareRoomMorphConfig, air_humidity_percent) + sizeof(float),
              "SonareRoomMorphConfig receiver spacing layout changed");
#endif

/// @brief Fill @p config with struct_size and the library default of every field: a
///   7 x 5 x 3 m room, source and listener all-NaN (the default placement scaled to the
///   room, resolved at use), absorption 0.2, no length cap (max_sec 0), automatic mixing time
///   (mixing_time_ms 0), crossfade 5 ms, ism_order 3, EYRING, seed 1, no band arrays,
///   MATERIAL_PRESET_NONE, air absorption ON at 20 degC / 50 %. No-op on NULL.
void sonare_rir_synth_config_init(SonareRirSynthConfig* config);

/// @brief Fill @p config with struct_size and the library default of every field: the
///   sonare_rir_synth_config_init room and placement, source_tail_suppression 0.5, wet
///   0.5, receiver_spacing_m NaN (fitted to the room), and the same synthesis defaults. No-op
///   on NULL.
void sonare_room_morph_config_init(SonareRoomMorphConfig* config);

/// @brief Synthesize a room impulse response from shoebox geometry.
/// @param config      Geometry + placement + synthesis controls.
/// @param sample_rate Output sample rate (Hz).
/// @param out         Receives the synthesized RIR (caller frees).
/// @return SONARE_ERROR_INVALID_PARAMETER for an invalid room or placement, with the
///   first Error diagnostic's message in sonare_last_error_message() and its code (for
///   example acoustic.source_outside_room) in sonare_last_diagnostic; @p out is left empty.
///   Recoverable problems (a clamped tail) are Warning entries of sonare_last_diagnostic.
SonareError sonare_synthesize_rir(const SonareRirSynthConfig* config, int sample_rate,
                                  SonareRirSynthResult* out);
/// @brief Release the buffer of a @ref SonareRirSynthResult; NULL-safe.
void sonare_free_rir_synth_result(SonareRirSynthResult* result);

/// @brief Fill @p config with struct_size and the library default of every field: aspect
///   priors 1 and 1, reference_absorption 0.15, min_decay_db 30, noise_floor_margin_db 10,
///   EYRING, 6 octave bands, ACOUSTIC_MODE_AUTO. No-op on NULL.
void sonare_room_estimate_config_init(SonareRoomEstimateConfig* config);

/// @brief Estimate an equivalent room from a recording (or impulse response).
SonareError sonare_estimate_room(const float* samples, size_t length, int sample_rate,
                                 const SonareRoomEstimateConfig* config, SonareRoomEstimate* out);
/// @brief Release the band arrays of a @ref SonareRoomEstimate; NULL-safe.
void sonare_free_room_estimate(SonareRoomEstimate* result);

/// @brief Offline room-character morph toward a target room.
/// @param out        Receives the morphed mono buffer (caller frees via
///                   sonare_free_floats). Length is the input length plus the
///                   target room's reverb tail.
SonareError sonare_room_morph(const float* samples, size_t length, int sample_rate,
                              const SonareRoomMorphConfig* config, float** out, size_t* out_length);

/// @brief Offline room-character morph of a stereo pair toward a target room heard at two
///        receivers spaced config->receiver_spacing_m apart across the listener.
/// @param out_left   Receives the morphed left channel (caller frees via sonare_free_floats).
/// @param out_right  Receives the morphed right channel (caller frees via sonare_free_floats).
/// @param out_length Receives the sample count of each channel: the input length plus the
///                   target room's reverb tail.
SonareError sonare_room_morph_stereo(const float* left, const float* right, size_t length,
                                     int sample_rate, const SonareRoomMorphConfig* config,
                                     float** out_left, float** out_right, size_t* out_length);

/// @brief Name of a SONARE_MATERIAL_PRESET_* value ("none", "concrete", "wood", "curtain",
///        "carpet", "glass"), or NULL when @p preset is not one. The values are contiguous from
///        0, so a caller lists the valid names by counting up until NULL and resolves a name to
///        its value by position. Static storage.
const char* sonare_material_preset_name(int preset);

/// @brief Name of a SONARE_ACOUSTIC_MODE_* value ("auto", "blind", "impulseResponse"), or NULL
///        when @p mode is not one. Contiguous from 0, enumerated as sonare_material_preset_name.
///        Static storage, never invalidated; do not free.
const char* sonare_acoustic_mode_name(int mode);

/// @brief Room geometry for sonare_synthesize_rir taken from a room estimate.
/// @details Initializes @p out with sonare_rir_synth_config_init, then sets the three dimensions
/// and borrows the estimate's per-band
///   absorption as @c band_absorption (so @p estimate must outlive any use of @p out).
///   Source and listener are written explicitly as the default placement scaled to the
///   estimated room, the same positions an all-NaN placement resolves to at use, so they lie
///   inside any estimated room. The scalar @c absorption is the mean of the finite bands;
///   with no finite band it stays at the default. Absorption bands that are missing or
///   contain a non-finite value (the fit did not converge) are not borrowed, so the scalar
///   applies. A caller may overwrite any field before synthesizing. Refuses
///   (SONARE_ERROR_INVALID_PARAMETER, with a message) an estimate whose dimensions are not
///   finite and positive, i.e. no decay was measurable.
/// @param estimate Result of sonare_estimate_room.
/// @param out      Receives the geometry. Its band pointer is borrowed from @p estimate and
///                 released with it by sonare_free_room_estimate; do not free it.
SonareError sonare_room_geometry_from_estimate(const SonareRoomEstimate* estimate,
                                               SonareRirSynthConfig* out);

#ifdef __cplusplus
}
#endif
