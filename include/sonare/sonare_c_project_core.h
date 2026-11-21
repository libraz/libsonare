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

/// @brief Track kind ordinals; mirror sonare::arrangement::Track::Kind. Pinned by a
/// static_assert in the .cpp so reordering the C++ enum is caught.
typedef enum SONARE_ENUM_BASE {
  SONARE_TRACK_AUDIO = 0,  ///< Carries audio clips.
  SONARE_TRACK_MIDI = 1,   ///< Carries MIDI clips routed to an instrument destination.
  SONARE_TRACK_AUX = 2     ///< Auxiliary (bus) track.
} SonareProjectTrackKind;

/// @brief Project clip-overlap policy ordinals.
typedef enum SONARE_ENUM_BASE {
  SONARE_PROJECT_OVERLAP_DISALLOW = 0,  ///< Clips on one track may not overlap.
  SONARE_PROJECT_OVERLAP_ALLOW = 1      ///< Clips on one track may overlap.
} SonareProjectOverlapPolicy;

/// @brief Project tempo segment descriptor for @ref sonare_project_set_tempo_segments.
///
/// @p start_sample is an output/derived timeline value kept in this POD for ABI
/// compatibility with compile/import surfaces. The setter ignores it and
/// derives sample positions from @p start_ppq / @p bpm during tempo-map
/// normalization.
typedef struct {
  double start_ppq;     ///< Segment start in quarter notes, [0, 1e12].
  double bpm;           ///< Tempo in beats per minute, in (0, 100000].
  double start_sample;  ///< Derived timeline sample position; ignored by the setter, 0 when read.
  double end_bpm;       ///< Tempo reached at the segment end in BPM; 0 = constant tempo.
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
  double start_ppq;  ///< Segment start in quarter notes, [0, 1e12].
  int numerator;     ///< Beats per bar; positive.
  int denominator;   ///< Beat unit (4 = quarter note); positive.
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
  uint32_t code;       ///< sonare::arrangement::Diagnostic::Code ordinal.
  uint32_t severity;   ///< 0 = error, 1 = warning.
  uint32_t target_id;  ///< Affected clip / track / source id; 0 = not applicable.
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
///        @ref sonare_free_project_compile_result). @p messages is the
///        newline-joined human-readable detail of every diagnostic (heap C
///        string, same free function), with line @c i corresponding to
///        @c diagnostics[i]. @p has_timeline is non-zero when compilation
///        produced a renderable timeline (no error diagnostics).
typedef struct {
  SonareProjectDiagnostic*
      diagnostics;          ///< Heap array of @p diagnostic_count entries; may be NULL.
  size_t diagnostic_count;  ///< Number of entries in @p diagnostics.
  char* messages;           ///< Heap C string, one line per diagnostic; may be NULL.
  int has_timeline;         ///< Non-zero when a renderable timeline was produced.
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

/// The render length derived from the compiled arrangement (its musical end plus the longest
/// hosted-instrument release tail). Outside the length's real domain (a positive frame count), so
/// it never reads as a length.
#define SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT (-1)

/// The output width of the scene master's layout (2 with no master). Outside the channel count's
/// real domain, so it never reads as a width.
#define SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER (-1)

/// The project's own sample rate. Outside the rate's real domain, so it never reads as a rate.
#define SONARE_PROJECT_SAMPLE_RATE_PROJECT (-1)

/// @brief Options for @ref sonare_project_bounce. Start from
///        @ref sonare_project_bounce_options_init; every field is then read literally and
///        checked against the range given beside it.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareProjectBounceOptions); set by the init function.
  /// Render length in frames at @p sample_rate; positive. Default
  /// @ref SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT, which derives it.
  int64_t total_frames;
  /// Render block size in frames; positive. Default 128.
  int block_size;
  /// Output channel count: 1, 2, 6 or 8, at most the width of the scene master's layout
  /// (mono/stereo/no master allow 2). Default @ref SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER,
  /// the width of the scene master's layout (2 with no master). The master is mixed at this
  /// width; 1 folds a 2-channel master to 0.5(L+R).
  int num_channels;
  /// Sample rate in Hz; must equal the project sample rate. Default
  /// @ref SONARE_PROJECT_SAMPLE_RATE_PROJECT, the project's.
  int sample_rate;
  /// Host-instrument latency compensation in samples fed to the compiler; >= 0. Default 0.
  int instrument_latency_samples;
} SonareProjectBounceOptions;

/// @brief Writes every default into @p options and sets its @c struct_size; NULL is ignored.
void sonare_project_bounce_options_init(SonareProjectBounceOptions* options);

#ifdef __cplusplus
static_assert(offsetof(SonareProjectBounceOptions, struct_size) == 0,
              "BounceOptions.struct_size offset");
static_assert(offsetof(SonareProjectBounceOptions, total_frames) == sizeof(int64_t),
              "BounceOptions.total_frames offset");
static_assert(sizeof(SonareProjectBounceOptions) == 2 * sizeof(int64_t) + 4 * sizeof(int),
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
/// @note Release @p out with @ref sonare_project_destroy; it is a handle, not a
///       sonare_free_* buffer.
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
/// @param out_diag Optional; receives a heap C string with the joined diagnostic
///        messages, or NULL when the load produced none. A SUCCESSFUL load can
///        set it: a document with a dropped warp map, a duplicate automation
///        lane or an unparsable MIDI content key loads with warnings, and those
///        warnings are what this reports. So a non-NULL string does NOT mean
///        failure -- the return code alone does -- and the caller owns the string
///        on every return code and must release it with @ref sonare_free_string.
///        A key no record of the document reads (other than `$`- and `x-`-prefixed ones)
///        fails the load as `invalid_format`, naming its path: `unknown project key
///        'clips[3].bogus'`, or `scene: unknown scene key 'strips[2].faderDB'` inside the
///        embedded mixer scene. Insert `params` bags and the MIDI content map's keys are data
///        and are not checked.
///        Pass NULL to ignore the diagnostics entirely. Malformed input returns
///        an error WITHOUT crashing.
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
/// @p sample_rate within the supported project audio range. The source's
/// num_channels and sample_rate_hint are set from @p channels and
/// @p sample_rate in the same undo step.
SonareError sonare_project_set_source_audio(SonareProject* project, uint32_t source_id,
                                            const float* interleaved, int64_t frames, int channels,
                                            int sample_rate);

/// @brief Replaces an audio source's owning metadata strings as one undoable
///        edit.
///
/// Both strings must be non-NULL NUL-terminated strings. An empty string clears
/// the corresponding field. All other source state is preserved. The two
/// fields are committed and undone together through the existing source
/// replacement command.
SonareError sonare_project_set_audio_source_metadata(SonareProject* project, uint32_t source_id,
                                                     const char* content_hash,
                                                     const char* external_stem_role);

/// @brief Sets the project sample rate in Hz. Must be a whole number of hertz
///        within [8000, 384000]; anything else returns SONARE_ERROR_INVALID_PARAMETER.
/// @details Applied through the edit history like every other setter, so it is
///          undoable and takes its place in the undo stack.
SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate);

/// @brief Sets the project's clip-overlap policy.
SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy);

/// @brief Replaces the project tempo segment list.
/// @details Takes the same segments as its engine counterpart
///          (@ref sonare_engine_set_tempo_segments): @c start_ppq in
///          [0, 1e12] and strictly increasing across the list, @c bpm finite and
///          in (0, 100000], @c end_bpm either 0 (no ramp) or in the same tempo
///          range. Anything else returns SONARE_ERROR_INVALID_PARAMETER; the
///          segments a project stores are the segments its own MIR and loop
///          helpers build a tempo map from, so an unbounded one computes
///          meaningless sample positions rather than failing.
SonareError sonare_project_set_tempo_segments(SonareProject* project,
                                              const SonareProjectTempoSegment* segments,
                                              size_t segment_count);

/// @brief Replaces the project time-signature segment list.
/// @details Same shape rules as the tempo setter above: @c start_ppq in
///          [0, 1e12] and strictly increasing across the list, with a positive
///          numerator and denominator. Segments are stored verbatim and read
///          back in stored order, so an out-of-order list would survive a save
///          and reload with only the compiler's internal sort hiding it.
SonareError sonare_project_set_time_signatures(SonareProject* project,
                                               const SonareProjectTimeSignatureSegment* segments,
                                               size_t segment_count);

/// @brief Reads a project tempo segment by index (0-based, in stored order).
/// @details Pairs with @ref sonare_project_tempo_segment_count, which is the
///          only way to learn the valid index range; an index at or past that
///          count yields SONARE_ERROR_INVALID_PARAMETER.
///
///          @p start_sample comes back as the project stores it, which is 0 for
///          every segment: a project keeps musical positions only, and sample
///          positions are derived by tempo-map normalization when the project is
///          compiled. Read @p start_ppq and @p bpm; a caller that needs sample
///          positions has to run them through the tempo map itself.
SonareError sonare_project_tempo_segment_by_index(const SonareProject* project, size_t index,
                                                  SonareProjectTempoSegment* out);

/// @brief Reads a project time-signature segment by index (0-based, in stored order).
/// @details Pairs with @ref sonare_project_time_signature_count. An index out of
///          range yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_time_signature_by_index(const SonareProject* project, size_t index,
                                                   SonareProjectTimeSignatureSegment* out);

/// @brief Project timeline marker with its kind and (for key signatures) the
///        structured key. @c kind is a SonareMarkerKind ordinal; the key fields
///        apply only to the key-signature kind. The layout mirrors
///        SonareEngineMarker so one binding shape serves both.
typedef struct {
  uint32_t id;        ///< Stable marker id; 0 on input allocates a new one.
  uint8_t kind;       ///< A @ref SonareMarkerKind ordinal.
  int8_t key_fifths;  ///< Key signature only: -7..7 (sharps positive).
  uint8_t key_minor;  ///< Key signature only: 0 major / 1 minor.
  double ppq;         ///< Position in quarter notes.
  char name[64];      ///< NUL-terminated UTF-8 name, truncated to 63 bytes.
} SonareProjectMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectMarker) == 80u, "SonareProjectMarker layout drift");
static_assert(offsetof(SonareProjectMarker, ppq) == 8u, "SonareProjectMarker ppq offset");
static_assert(offsetof(SonareProjectMarker, name) == 16u, "SonareProjectMarker name offset");
#endif

/// @brief Adds or replaces a marker. @p marker_id 0 allocates a new id. The
///        marker is created with the default (Marker) kind; use
///        sonare_project_set_marker_ex to set a text / lyric / cue / key
///        signature kind. An explicit @p marker_id of UINT32_MAX is reserved and
///        yields SONARE_ERROR_INVALID_PARAMETER: a project holding one would
///        serialize but could never be deserialized again.
SonareError sonare_project_set_marker(SonareProject* project, uint32_t marker_id, double ppq,
                                      const char* name, uint32_t* out_marker_id);

/// @brief Adds or replaces a marker from a full SonareProjectMarker, including
///        its kind and key signature. @p marker->id 0 allocates a new id and
///        UINT32_MAX is reserved (SONARE_ERROR_INVALID_PARAMETER); the
///        allocated / affected id is returned via @p out_marker_id. For the
///        key-signature kind @p marker->key_fifths must be in -7..7 (the SMF
///        `sf` range); an out-of-range value yields SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_set_marker_ex(SonareProject* project, const SonareProjectMarker* marker,
                                         uint32_t* out_marker_id);

/// @brief Adds or replaces a full marker while accepting an unbounded UTF-8 name.
/// @details @p marker supplies the non-name fields, under the same id rules as
///          sonare_project_set_marker_ex. @p name is copied verbatim; use this
///          API when the fixed 64-byte compatibility field is too small.
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
  uint32_t id;                   ///< Stable track id.
  uint32_t kind;                 ///< A @ref SonareProjectTrackKind ordinal.
  uint32_t midi_destination_id;  ///< Host-instrument destination for MIDI clips; 0 = default.
  float gain;                    ///< Linear playback gain, >= 0 (1 = unity).
  float pan;                     ///< Stereo balance in [-1, +1] (0 = center).
  uint8_t mute;                  ///< Non-zero when muted.
  uint8_t solo;                  ///< Non-zero when soloed.
  uint8_t reserved[2];           ///< Padding; always 0.
  char name[64];                 ///< NUL-terminated UTF-8 name, truncated to 63 bytes.
} SonareProjectTrack;

/// @brief Flat, read-only clip descriptor returned by @ref sonare_project_clip_by_index.
typedef struct {
  uint32_t id;               ///< Stable clip id.
  uint32_t track_id;         ///< Id of the owning track.
  uint32_t source_id;        ///< Id of the audio or MIDI source the clip plays.
  uint32_t source_kind;      ///< 0 = audio source, 1 = MIDI source.
  double start_ppq;          ///< Clip start on the timeline in quarter notes.
  double length_ppq;         ///< Clip length in quarter notes.
  double source_offset_ppq;  ///< Offset into the source in quarter notes.
  float gain;                ///< Linear playback gain, >= 0 (0 = silent).
  uint32_t loop_mode;        ///< A @ref SonareProjectLoopMode ordinal.
  double loop_length_ppq;    ///< Loop length in quarter notes; 0 = loop the whole clip.
} SonareProjectClip;

/// @brief Flat, read-only source descriptor returned by @ref sonare_project_source_by_index.
typedef struct {
  uint32_t id;                 ///< Stable source id.
  uint32_t kind;               ///< 0 = audio, 1 = MIDI.
  uint32_t num_channels;       ///< Audio channel count; 0 when unknown.
  uint32_t storage_handle_id;  ///< Host storage handle id for audio referenced by id; 0 = none.
  double sample_rate_hint;     ///< Source sample rate in Hz; 0 when unknown.
  char name_or_uri[128];       ///< NUL-terminated UTF-8 name or URI, truncated to 127 bytes.
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

/// @brief Heap-owned metadata for an audio source.
///
/// The strings are copied from the source and are owned by the returned
/// descriptor. Release them with @ref sonare_free_project_audio_source_metadata.
/// MIDI sources do not have this metadata and are rejected by
/// @ref sonare_project_get_audio_source_metadata.
typedef struct {
  char* content_hash;  ///< Heap C string: host-supplied content digest; empty = unset.
  char*
      external_stem_role;  ///< Heap C string: stem role from an external separator; empty = unset.
} SonareProjectAudioSourceMetadata;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectAudioSourceMetadata, content_hash) == 0,
              "AudioSourceMetadata.content_hash offset");
static_assert(offsetof(SonareProjectAudioSourceMetadata, external_stem_role) == sizeof(void*),
              "AudioSourceMetadata.external_stem_role offset");
static_assert(sizeof(SonareProjectAudioSourceMetadata) == 2u * sizeof(void*),
              "SonareProjectAudioSourceMetadata layout drift");
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

/// @brief Returns the complete UTF-8 track name by 0-based stored index.
/// @details Free @p out_name with sonare_free_string.
SonareError sonare_project_track_name_by_index(const SonareProject* project, size_t index,
                                               char** out_name);

/// @brief Returns the complete UTF-8 name (MIDI) or URI (audio) of a source by 0-based stored
///        index.
/// @details Free @p out_name_or_uri with sonare_free_string.
SonareError sonare_project_source_name_or_uri_by_index(const SonareProject* project, size_t index,
                                                       char** out_name_or_uri);

/// @brief Reads the owning metadata strings for an audio source id.
///
/// On success both fields are heap-allocated copies, including empty strings,
/// and must be released with @ref sonare_free_project_audio_source_metadata.
/// The output is zeroed before validation and remains zeroed on every failure.
/// Unknown ids and MIDI source ids return @c SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_get_audio_source_metadata(const SonareProject* project,
                                                     uint32_t source_id,
                                                     SonareProjectAudioSourceMetadata* out);

/// @brief Reads an unresolved audio source descriptor by zero-based index.
///
/// Uses the same ordering as @ref sonare_project_unresolved_audio_source_id_by_index.
/// @c name_or_uri is truncated to 127 bytes; read the full URI with
/// @ref sonare_project_get_audio_source_uri. The output is zeroed on every failure.
SonareError sonare_project_unresolved_audio_source_by_index(const SonareProject* project,
                                                            size_t index, SonareProjectSource* out);

/// @brief Reads the full, untruncated URI of an audio source id.
///
/// On success @p out_uri is a heap copy that the caller releases with
/// @ref sonare_free_string; it is NULL on every failure. Unknown ids and MIDI
/// source ids return @c SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_get_audio_source_uri(const SonareProject* project, uint32_t source_id,
                                                char** out_uri);

/// @brief Frees strings returned by
///        @ref sonare_project_get_audio_source_metadata and zeros the struct.
///        NULL is safe.
void sonare_free_project_audio_source_metadata(SonareProjectAudioSourceMetadata* metadata);

/// @brief Replaces the project's mixer scene from scene JSON.
/// @details A key the scene reader does not consume (other than `$`- and `x-`-prefixed ones)
///   fails the call with SONARE_ERROR_INVALID_FORMAT naming it, e.g.
///   `unknown scene key 'strips[2].faderDB'`; the project's scene is left unchanged.
SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* scene_json);

/// @brief Reads the project sample rate (Hz).
SonareError sonare_project_get_sample_rate(const SonareProject* project, double* out_sample_rate);

/// @brief Reads the project overlap policy (SonareProjectOverlapPolicy ordinal).
SonareError sonare_project_get_overlap_policy(const SonareProject* project,
                                              uint32_t* out_overlap_policy);

/// @brief Counts tracks without requiring JSON serialization.
SonareError sonare_project_track_count(const SonareProject* project, size_t* out_count);
/// @brief Counts clips without requiring JSON serialization.
SonareError sonare_project_clip_count(const SonareProject* project, size_t* out_count);
/// @brief Counts sources (audio and MIDI) without requiring JSON serialization.
SonareError sonare_project_source_count(const SonareProject* project, size_t* out_count);
/// @brief Counts markers without requiring JSON serialization.
SonareError sonare_project_marker_count(const SonareProject* project, size_t* out_count);
/// @brief Counts tempo segments; the valid index range for
///        @ref sonare_project_tempo_segment_by_index.
SonareError sonare_project_tempo_segment_count(const SonareProject* project, size_t* out_count);
/// @brief Counts time-signature segments; the valid index range for
///        @ref sonare_project_time_signature_by_index.
SonareError sonare_project_time_signature_count(const SonareProject* project, size_t* out_count);

/// @brief Compiles the project into an RT-readable timeline, surfacing
///        diagnostics. Never throws; bad input yields error diagnostics.
/// @param out Zero-initialized result; free with
///        @ref sonare_free_project_compile_result.
SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out);

/// @brief Token of a @ref SonareProjectDiagnostic code ("danglingSourceRef", "clipOverlap", ...),
///        or NULL when @p code is not one. Contiguous from 0, so a caller lists the codes by
///        counting up until NULL. Static storage, never invalidated; do not free.
const char* sonare_project_diagnostic_code_name(int code);

/// @brief Token of a @ref SonareProjectDiagnostic severity ("error" or "warning"), or NULL when
///        @p severity is not one. Contiguous from 0. Static storage, never invalidated; do not
///        free.
const char* sonare_project_diagnostic_severity_name(int severity);

/// @brief Returns compile diagnostics produced by the most recent
///        @ref sonare_project_bounce / instrument bounce on this project.
/// @details This surfaces non-fatal warnings from the bounce's internal compile
///          step, such as MIDI clips rendering silently without a bound
///          instrument. On a project no bounce has ever run on, the result is
///          empty in full: `has_timeline` is zero and `diagnostic_count` is
///          zero. That state is distinguishable from a failed bounce, because a
///          bounce only loses its timeline through an error diagnostic and so
///          always reports at least one diagnostic when it fails. A bounce
///          rejected for invalid arguments before it compiles clears the
///          recorded result back to that same empty state; that call reports
///          its own error to the caller directly. Free with
///          @ref sonare_free_project_compile_result.
SonareError sonare_project_last_bounce_compile_result(const SonareProject* project,
                                                      SonareProjectCompileResult* out);

/// @brief Frees the heap buffers held by a compile result and zeroes it.
void sonare_free_project_compile_result(SonareProjectCompileResult* result);

/// @brief Compiles + renders the project offline to interleaved float audio.
/// @param options Render options, started from @ref sonare_project_bounce_options_init. May be
///        NULL for those defaults (project sample rate, the scene master's layout width, block
///        128).
/// @param out_interleaved Receives a heap float array (free with
///        @ref sonare_free_floats) of length @p out_len = total_frames * channels.
/// @param out_len Receives the interleaved sample count.
/// @param out_num_channels Receives the width the render was mixed at: the requested
///        num_channels, or the resolved default when it was
///        @ref SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER. Written 0 first, so it is
///        defined on every return.
/// @details Deterministic: the same project + options yields bit-identical
///          output within one build. When options->total_frames is positive,
///          that explicit length is used as-is; mixer FX / instrument tails are
///          auto-extended only when total_frames is
///          @ref SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT.
/// @return SONARE_ERROR_INVALID_PARAMETER for a refused option;
///         SONARE_ERROR_INVALID_STATE when the stored project cannot be
///         rendered (its derived length overflows, or the compiled arrangement
///         does not fit the engine or the scene's mixer).
SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len, int* out_num_channels);

#ifdef __cplusplus
}
#endif
