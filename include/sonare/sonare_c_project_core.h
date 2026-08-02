#pragma once

/// @file sonare_c_project_core.h
/// @brief Project lifecycle, IO, render, and shared flat-POD descriptors for the
///        headless arrangement C ABI. Included via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque headless-project handle. Wraps an arrangement EditHistory
///        (owning the Project + MIDI content store) plus an audio content store.
typedef struct SonareProject SonareProject;

/// Track kind ordinals; mirror sonare::arrangement::Track::Kind. Pinned by a
/// static_assert in the .cpp so reordering the C++ enum is caught.
typedef enum {
  SONARE_TRACK_AUDIO = 0,
  SONARE_TRACK_MIDI = 1,
  SONARE_TRACK_AUX = 2
} SonareProjectTrackKind;

/// Project clip-overlap policy ordinals.
typedef enum {
  SONARE_PROJECT_OVERLAP_DISALLOW = 0,
  SONARE_PROJECT_OVERLAP_ALLOW = 1
} SonareProjectOverlapPolicy;

/// @brief Project tempo segment descriptor for @ref sonare_project_set_tempo_segments.
///
/// @p start_sample is an output/derived timeline value kept in this POD for ABI
/// compatibility with compile/import surfaces. The setter ignores it and
/// derives sample positions from @p start_ppq / @p bpm during tempo-map
/// normalization.
typedef struct {
  double start_ppq;
  double bpm;
  double start_sample;
  double end_bpm; /* 0 = constant tempo */
} SonareProjectTempoSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTempoSegment, start_ppq) == 0, "TempoSegment.start_ppq offset");
static_assert(offsetof(SonareProjectTempoSegment, bpm) == 8, "TempoSegment.bpm offset");
static_assert(offsetof(SonareProjectTempoSegment, start_sample) == 16,
              "TempoSegment.start_sample offset");
static_assert(offsetof(SonareProjectTempoSegment, end_bpm) == 24, "TempoSegment.end_bpm offset");
static_assert(sizeof(SonareProjectTempoSegment) == 4u * sizeof(double),
              "SonareProjectTempoSegment layout drift");
#endif

/// @brief Project time-signature segment descriptor.
typedef struct {
  double start_ppq;
  int numerator;
  int denominator;
} SonareProjectTimeSignatureSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTimeSignatureSegment, start_ppq) == 0,
              "TimeSignatureSegment.start_ppq offset");
static_assert(offsetof(SonareProjectTimeSignatureSegment, numerator) == sizeof(double),
              "TimeSignatureSegment.numerator offset");
static_assert(offsetof(SonareProjectTimeSignatureSegment, denominator) ==
                  sizeof(double) + sizeof(int),
              "TimeSignatureSegment.denominator offset");
static_assert(sizeof(SonareProjectTimeSignatureSegment) == 2u * sizeof(double),
              "SonareProjectTimeSignatureSegment layout drift");
#endif

/// @brief One compile diagnostic surfaced by @ref sonare_project_compile.
///        Mirrors sonare::arrangement::Diagnostic (code / severity / target_id).
///        Human-readable messages are stored in the same order in
///        SonareProjectCompileResult::messages, one line per diagnostic, with
///        embedded CR/LF normalized to spaces to preserve this frozen POD layout
///        across the C ABI boundary.
typedef struct {
  uint32_t code;      /* sonare::arrangement::Diagnostic::Code ordinal */
  uint32_t severity;  /* 0 = error, 1 = warning */
  uint32_t target_id; /* affected clip / track / source id (0 = n/a) */
} SonareProjectDiagnostic;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectDiagnostic) == 3u * sizeof(uint32_t),
              "SonareProjectDiagnostic layout drift");
static_assert(offsetof(SonareProjectDiagnostic, code) == 0, "Diagnostic.code offset");
static_assert(offsetof(SonareProjectDiagnostic, severity) == 4, "Diagnostic.severity offset");
static_assert(offsetof(SonareProjectDiagnostic, target_id) == 8, "Diagnostic.target_id offset");
#endif

/// @brief Result of @ref sonare_project_compile. @p diagnostics is a
///        heap-allocated array of @p diagnostic_count entries (free with
///        @ref sonare_project_free_compile_result). @p messages is the
///        newline-joined human-readable detail of every diagnostic (heap C
///        string, same free function), with line @c i corresponding to
///        @c diagnostics[i]. @p has_timeline is non-zero when compilation
///        produced a renderable timeline (no error diagnostics).
typedef struct {
  SonareProjectDiagnostic* diagnostics;
  size_t diagnostic_count;
  char* messages;
  int has_timeline;
} SonareProjectCompileResult;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectCompileResult, diagnostics) == 0,
              "CompileResult.diagnostics offset");
static_assert(offsetof(SonareProjectCompileResult, diagnostic_count) == sizeof(void*),
              "CompileResult.diagnostic_count offset");
static_assert(offsetof(SonareProjectCompileResult, messages) == sizeof(void*) + sizeof(size_t),
              "CompileResult.messages offset");
// Trailing `int has_timeline` + tail padding rounds the struct up to pointer
// alignment, so the int contributes exactly sizeof(void*) on both 64-bit
// (int 4 + pad 4 = 8) and wasm32 (int 4 = 4). Avoids a hardcoded 64-bit size.
static_assert(sizeof(SonareProjectCompileResult) == 3u * sizeof(void*) + sizeof(size_t),
              "SonareProjectCompileResult layout drift");
#endif

/// @brief Options for @ref sonare_project_bounce. Zero-initialize then override.
typedef struct {
  int64_t total_frames;           /* render length in frames at @p sample_rate */
  int block_size;                 /* render block size; <= 0 => 128 */
  int num_channels;               /* output channel count; <= 0 => 2 */
  int sample_rate;                /* must match the project sample rate; <= 0 => the project's */
  int instrument_latency_samples; /* host-instrument PDC fed to the compiler */
} SonareProjectBounceOptions;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectBounceOptions, total_frames) == 0,
              "BounceOptions.total_frames offset");
static_assert(sizeof(SonareProjectBounceOptions) == sizeof(int64_t) + 4 * sizeof(int),
              "SonareProjectBounceOptions layout drift");
#endif

// ============================================================================
// ABI version
// ============================================================================

/// @brief Runtime ABI version of the flat project POD layout. Equals
///        @ref SONARE_PROJECT_ABI_VERSION when the arrangement subsystem is
///        compiled in, 0 otherwise (so bindings can detect the absence).
uint32_t sonare_project_abi_version(void);

// ============================================================================
// Lifecycle / IO / render
// ============================================================================

/// @brief Creates an empty headless project handle.
SonareError sonare_project_create(SonareProject** out);
/// @brief Destroys a project handle. NULL is a safe no-op.
void sonare_project_destroy(SonareProject* project);

/// @brief Serializes the project (+ MIDI content) to deterministic JSON.
/// @param out_json Receives a heap C string (free with @ref sonare_free_string).
/// @param out_len  Receives the JSON byte length (excluding the NUL), or NULL.
SonareError sonare_project_serialize(const SonareProject* project, char** out_json,
                                     size_t* out_len);

/// @brief Deserializes project JSON into a NEW project handle.
/// @param json Project JSON (need not be NUL-terminated; @p len bytes are read).
/// @param len  Byte length of @p json.
/// @param out  Receives the new handle on success.
/// @param out_diag Optional; on failure receives a heap C string with the joined
///        diagnostic messages (free with @ref sonare_free_string). On success it
///        is set to NULL. Pass NULL to ignore. Malformed input returns an error
///        WITHOUT crashing.
SonareError sonare_project_deserialize(const char* json, size_t len, SonareProject** out,
                                       char** out_diag);

/// @brief Returns how many audio sources have no decoded PCM registered.
///
/// After deserializing project JSON, URI-backed audio sources are intentionally
/// unresolved until their host supplies PCM through @ref sonare_project_set_source_audio.
SonareError sonare_project_unresolved_audio_source_count(const SonareProject* project,
                                                         size_t* out_count);

/// @brief Returns an unresolved audio source id by zero-based index.
SonareError sonare_project_unresolved_audio_source_id_by_index(const SonareProject* project,
                                                               size_t index,
                                                               uint32_t* out_source_id);

/// @brief Registers decoded interleaved PCM for an existing audio source.
///
/// The edit is undoable. @p frames must be positive, @p channels positive, and
/// @p sample_rate within the supported project audio range.
SonareError sonare_project_set_source_audio(SonareProject* project, uint32_t source_id,
                                            const float* interleaved, int64_t frames, int channels,
                                            int sample_rate);

/// @brief Sets the project sample rate (Hz). Must be > 0.
SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate);

/// @brief Sets the project's clip-overlap policy.
SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy);

/// @brief Replaces the project tempo segment list.
SonareError sonare_project_set_tempo_segments(SonareProject* project,
                                              const SonareProjectTempoSegment* segments,
                                              size_t segment_count);

/// @brief Replaces the project time-signature segment list.
SonareError sonare_project_set_time_signatures(SonareProject* project,
                                               const SonareProjectTimeSignatureSegment* segments,
                                               size_t segment_count);

/// @brief Project timeline marker with its kind and (for key signatures) the
///        structured key. @c kind is a SonareMarkerKind ordinal; the key fields
///        apply only to the key-signature kind. The layout mirrors
///        SonareEngineMarker so one binding shape serves both.
typedef struct {
  uint32_t id;
  uint8_t kind;      /* SonareMarkerKind */
  int8_t key_fifths; /* key signature only: -7..7 (sharps positive) */
  uint8_t key_minor; /* key signature only: 0 major / 1 minor */
  double ppq;
  char name[64];
} SonareProjectMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectMarker) == 80u, "SonareProjectMarker layout drift");
static_assert(offsetof(SonareProjectMarker, ppq) == 8u, "SonareProjectMarker ppq offset");
static_assert(offsetof(SonareProjectMarker, name) == 16u, "SonareProjectMarker name offset");
#endif

/// @brief Adds or replaces a marker. @p marker_id 0 allocates a new id. The
///        marker is created with the default (Marker) kind; use
///        sonare_project_set_marker_ex to set a text / lyric / cue / key
///        signature kind.
SonareError sonare_project_set_marker(SonareProject* project, uint32_t marker_id, double ppq,
                                      const char* name, uint32_t* out_marker_id);

/// @brief Adds or replaces a marker from a full SonareProjectMarker, including
///        its kind and key signature. @p marker->id 0 allocates a new id; the
///        allocated / affected id is returned via @p out_marker_id. For the
///        key-signature kind @p marker->key_fifths must be in -7..7 (the SMF
///        `sf` range); an out-of-range value yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_set_marker_ex(SonareProject* project, const SonareProjectMarker* marker,
                                         uint32_t* out_marker_id);

/// @brief Adds or replaces a full marker while accepting an unbounded UTF-8 name.
/// @details @p marker supplies the non-name fields. @p name is copied verbatim;
///          use this API when the fixed 64-byte compatibility field is too small.
SonareError sonare_project_set_marker_ex_name(SonareProject* project,
                                              const SonareProjectMarker* marker, const char* name,
                                              uint32_t* out_marker_id);

/// @brief Reads a project marker by index (0-based, in stored order). An index
///        out of range yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_marker_by_index(const SonareProject* project, size_t index,
                                           SonareProjectMarker* out);

/// @brief Returns the complete UTF-8 marker name by index.
/// @details Free @p out_name with sonare_free_string.
SonareError sonare_project_marker_name_by_index(const SonareProject* project, size_t index,
                                                char** out_name);

/// @brief Flat, read-only track descriptor returned by @ref sonare_project_track_by_index.
typedef struct {
  uint32_t id;
  uint32_t kind; /* SonareProjectTrackKind */
  uint32_t midi_destination_id;
  float gain;
  float pan;
  uint8_t mute;
  uint8_t solo;
  uint8_t reserved[2];
  char name[64];
} SonareProjectTrack;

/// @brief Flat, read-only clip descriptor returned by @ref sonare_project_clip_by_index.
typedef struct {
  uint32_t id;
  uint32_t track_id;
  uint32_t source_id;
  uint32_t source_kind; /* 0 audio, 1 MIDI */
  double start_ppq;
  double length_ppq;
  double source_offset_ppq;
  float gain;
  uint32_t loop_mode;
  double loop_length_ppq;
} SonareProjectClip;

/// @brief Flat, read-only source descriptor returned by @ref sonare_project_source_by_index.
typedef struct {
  uint32_t id;
  uint32_t kind; /* 0 audio, 1 MIDI */
  uint32_t channel_count;
  uint32_t storage_handle_id;
  double sample_rate_hint;
  char name_or_uri[128];
} SonareProjectSource;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectTrack) == 88u, "SonareProjectTrack layout drift");
static_assert(offsetof(SonareProjectTrack, name) == 24u, "SonareProjectTrack name offset");
static_assert(sizeof(SonareProjectClip) == 56u, "SonareProjectClip layout drift");
static_assert(offsetof(SonareProjectClip, start_ppq) == 16u, "SonareProjectClip start_ppq offset");
static_assert(offsetof(SonareProjectClip, loop_length_ppq) == 48u,
              "SonareProjectClip loop_length_ppq offset");
static_assert(sizeof(SonareProjectSource) == 152u, "SonareProjectSource layout drift");
static_assert(offsetof(SonareProjectSource, sample_rate_hint) == 16u,
              "SonareProjectSource sample_rate_hint offset");
static_assert(offsetof(SonareProjectSource, name_or_uri) == 24u,
              "SonareProjectSource name_or_uri offset");
#endif

/// @brief Reads a project track by 0-based stored index.
SonareError sonare_project_track_by_index(const SonareProject* project, size_t index,
                                          SonareProjectTrack* out);
/// @brief Reads a project clip by 0-based stored index.
SonareError sonare_project_clip_by_index(const SonareProject* project, size_t index,
                                         SonareProjectClip* out);
/// @brief Reads a project source by 0-based stored index.
SonareError sonare_project_source_by_index(const SonareProject* project, size_t index,
                                           SonareProjectSource* out);

/// @brief Replaces the project's mixer scene from scene JSON.
SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* scene_json);

/// @brief Reads the project sample rate (Hz).
SonareError sonare_project_get_sample_rate(const SonareProject* project, double* out_sample_rate);

/// @brief Reads the project overlap policy (SonareProjectOverlapPolicy ordinal).
SonareError sonare_project_get_overlap_policy(const SonareProject* project,
                                              uint32_t* out_overlap_policy);

/// @brief Counts value-model entities without requiring JSON serialization.
SonareError sonare_project_track_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_clip_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_source_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_marker_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_tempo_segment_count(const SonareProject* project, size_t* out_count);
SonareError sonare_project_time_signature_count(const SonareProject* project, size_t* out_count);

/// @brief Compiles the project into an RT-readable timeline, surfacing
///        diagnostics. Never throws; bad input yields error diagnostics.
/// @param out Zero-initialized result; free with
///        @ref sonare_project_free_compile_result.
SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out);

/// @brief Returns compile diagnostics produced by the most recent
///        @ref sonare_project_bounce / instrument bounce on this project.
/// @details This surfaces non-fatal warnings from the bounce's internal compile
///          step, such as MIDI clips rendering silently without a bound
///          instrument. If no bounce has run, the result is empty with
///          @p has_timeline set non-zero. Free with
///          @ref sonare_project_free_compile_result.
SonareError sonare_project_last_bounce_compile_result(const SonareProject* project,
                                                      SonareProjectCompileResult* out);

/// @brief Frees the heap buffers held by a compile result and zeroes it.
void sonare_project_free_compile_result(SonareProjectCompileResult* result);

/// @brief Compiles + renders the project offline to interleaved float audio.
/// @param options Render options (zero-init then override). May be NULL for
///        defaults (project sample rate, 2 channels, block 128).
/// @param out_interleaved Receives a heap float array (free with
///        @ref sonare_free_floats) of length @p out_len = total_frames * channels.
/// @param out_len Receives the interleaved sample count.
/// @details Deterministic: the same project + options yields bit-identical
///          output within one build. When options->total_frames is positive,
///          that explicit length is used as-is; mixer FX / instrument tails are
///          auto-extended only when total_frames <= 0.
SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len);

#ifdef __cplusplus
}
#endif
