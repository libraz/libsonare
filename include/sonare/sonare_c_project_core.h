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
/// A project keeps musical positions only; sample positions are derived by
/// tempo-map normalization when the project is compiled.
typedef struct {
  double start_ppq;  ///< Segment start in quarter notes, [0, 1e12].
  double bpm;        ///< Tempo in beats per minute, finite and in [1e-6, 100000].
  double end_bpm;    ///< Tempo reached at the segment end in BPM; 0 = constant tempo.
} SonareProjectTempoSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTempoSegment, start_ppq) == 0, "TempoSegment.start_ppq offset");
static_assert(offsetof(SonareProjectTempoSegment, bpm) == 8, "TempoSegment.bpm offset");
static_assert(offsetof(SonareProjectTempoSegment, end_bpm) == 16, "TempoSegment.end_bpm offset");
static_assert(sizeof(SonareProjectTempoSegment) == 3u * sizeof(double),
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

/// @brief Owned copy of the project's stored tempo segments.
typedef struct {
  SonareProjectTempoSegment* items;  ///< Stored-order segments owned by this list.
  size_t count;                      ///< Number of entries in items.
} SonareProjectTempoSegmentList;

/// @brief Owned copy of the project's stored time-signature segments.
typedef struct {
  SonareProjectTimeSignatureSegment* items;  ///< Stored-order segments owned by this list.
  size_t count;                              ///< Number of entries in items.
} SonareProjectTimeSignatureList;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTempoSegmentList, items) == 0, "TempoSegmentList.items offset");
static_assert(offsetof(SonareProjectTempoSegmentList, count) == sizeof(void*),
              "TempoSegmentList.count offset");
static_assert(sizeof(SonareProjectTempoSegmentList) == sizeof(void*) + sizeof(size_t),
              "SonareProjectTempoSegmentList layout drift");
static_assert(offsetof(SonareProjectTimeSignatureList, items) == 0,
              "TimeSignatureList.items offset");
static_assert(offsetof(SonareProjectTimeSignatureList, count) == sizeof(void*),
              "TimeSignatureList.count offset");
static_assert(sizeof(SonareProjectTimeSignatureList) == sizeof(void*) + sizeof(size_t),
              "SonareProjectTimeSignatureList layout drift");
#endif

/// @brief Effective project position at one PPQ coordinate.
/// @details @c bar_index is zero-based; @c beat is one-based and
///          @c beat_fraction lies in [0, 1). @c time_signature is the normalized
///          segment active at the position.
typedef struct {
  double bpm;                                        ///< Instantaneous tempo at this position.
  SonareProjectTimeSignatureSegment time_signature;  ///< Effective normalized time signature.
  double bar_start_ppq;                              ///< Start of the bar containing the position.
  int64_t bar_index;                                 ///< Zero-based bar index.
  int64_t beat;                                      ///< One-based beat within the bar.
  double beat_fraction;                              ///< Position within the beat, in [0, 1).
} SonareProjectPosition;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectPosition, bpm) == 0u, "ProjectPosition.bpm offset");
static_assert(offsetof(SonareProjectPosition, time_signature) == sizeof(double),
              "ProjectPosition.time_signature offset");
static_assert(offsetof(SonareProjectPosition, bar_start_ppq) ==
                  sizeof(double) + sizeof(SonareProjectTimeSignatureSegment),
              "ProjectPosition.bar_start_ppq offset");
static_assert(offsetof(SonareProjectPosition, bar_index) == 4u * sizeof(double),
              "ProjectPosition.bar_index offset");
static_assert(offsetof(SonareProjectPosition, beat) == 5u * sizeof(double),
              "ProjectPosition.beat offset");
static_assert(offsetof(SonareProjectPosition, beat_fraction) == 6u * sizeof(double),
              "ProjectPosition.beat_fraction offset");
static_assert(sizeof(SonareProjectPosition) == 7u * sizeof(double),
              "SonareProjectPosition layout drift");
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
/// @param document Project JSON as @ref sonare_project_serialize writes it (need not be
///        NUL-terminated; @p len bytes are read).
/// @param len  Byte length of @p document.
/// @param out  Receives the new handle on success.
/// @param out_diag Optional; receives a heap C string with the joined diagnostic
///        messages, or NULL when the load produced none. A SUCCESSFUL load can
///        set it: a document with a dropped warp map, a duplicate automation
///        lane or an unparsable MIDI content key loads with warnings, and those
///        warnings are what this reports. So a non-NULL string does NOT mean
///        failure -- the return code alone does -- and the caller owns the string
///        on every return code and must release it with @ref sonare_free_string.
///        A key no record of the document reads (other than `$`- and `x-`-prefixed ones)
///        fails the load as `invalid_format`, naming its path: `unknown key clips[3].bogus`, or
///        `unknown key scene.strips[2].faderDB` inside the embedded mixer scene. Insert `params`
///        bags and the MIDI content map's keys are data and are not checked. Pass NULL to ignore
///        the diagnostics entirely. Malformed input returns an error WITHOUT crashing.
SonareError sonare_project_deserialize(const char* document, size_t len, SonareProject** out,
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

/// @brief Adds an unresolved audio source and returns its new ID.
/// @details The project copies the nonempty URI. Repeated URIs receive distinct IDs.
SonareError sonare_project_add_audio_source(SonareProject* project, const char* uri,
                                            uint32_t* out_source_id);

/// @brief Sets an audio source's URI and makes it unresolved as one undoable edit.
/// @details Clips keep their source ID. Undo restores the previous metadata and decoded PCM.
SonareError sonare_project_relink_source(SonareProject* project, uint32_t source_id,
                                         const char* uri);

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

/// @brief Sets the project sample rate in Hz. Must be an integer in [8000, 384000];
///        anything else returns SONARE_ERROR_INVALID_PARAMETER.
/// @details Applied through the edit history like every other setter, so it is
///          undoable and takes its place in the undo stack.
SonareError sonare_project_set_sample_rate(SonareProject* project, int sample_rate);

/// @brief Sets the project's clip-overlap policy.
SonareError sonare_project_set_overlap_policy(SonareProject* project, uint32_t overlap_policy);

/// @brief Replaces the project tempo segment list.
/// @details Takes the same segments as its engine counterpart
///          (@ref sonare_engine_set_tempo_segments): @c start_ppq in
///          [0, 1e12] and strictly increasing across the list, @c bpm finite and
///          in [1e-6, 100000], @c end_bpm either 0 (no ramp) or finite and in
///          the same tempo range. Anything else returns
///          SONARE_ERROR_INVALID_PARAMETER; the
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

/// @brief Converts a project PPQ position to seconds from PPQ zero.
/// @details @p ppq must be in [0, 1e12]. The result is independent of the
///          project's sample rate.
SonareError sonare_project_ppq_to_sec(const SonareProject* project, double ppq, double* out_sec);

/// @brief Converts seconds from PPQ zero to a project PPQ position.
/// @details @p sec must be in [0, the time at PPQ 1e12] for this project's
///          tempo map. The endpoint maps exactly to 1e12.
SonareError sonare_project_sec_to_ppq(const SonareProject* project, double sec, double* out_ppq);

/// @brief Reads the effective tempo, meter, bar, and beat at @p ppq.
/// @details @p ppq must be in [0, 1e12]. The bar index must fit in int64_t and
///          its start must round-trip through double PPQ. Otherwise the call
///          returns SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_position_at_ppq(const SonareProject* project, double ppq,
                                           SonareProjectPosition* out);

/// @brief Reads the PPQ start of a zero-based bar.
/// @details The requested bar must have a representable PPQ start in [0, 1e12]
///          and round-trip to the same bar index; otherwise the call returns
///          SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_bar_start_ppq(const SonareProject* project, int64_t bar_index,
                                         double* out_ppq);

/// @brief Reads the end in PPQ of the last clip on an audible track.
/// @details A track is audible when it is not muted and, while any track is soloed, is soloed
///          itself. A clip with a valid source and range counts whatever it holds, so an
///          event-less MIDI clip counts as a silent audio clip does. Returns zero when no
///          audible track holds a clip. Render tails are excluded.
SonareError sonare_project_end_ppq(const SonareProject* project, double* out_end_ppq);

/// @brief Project timeline marker with its kind and (for key signatures) the
///        structured key. @c kind is a SonareMarkerKind ordinal; the key fields
///        apply only to the key-signature kind. The name travels beside the
///        record: @ref sonare_project_set_marker takes it, and
///        @ref sonare_project_marker_name_by_index reads it.
typedef struct {
  uint32_t id;        ///< Stable marker id; 0 on input allocates a new one.
  uint8_t kind;       ///< A @ref SonareMarkerKind ordinal.
  int8_t key_fifths;  ///< Key signature only: -7..7 (sharps positive).
  uint8_t key_minor;  ///< Key signature only: 0 major / 1 minor.
  double ppq;         ///< Position in quarter notes.
} SonareProjectMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareProjectMarker) == 16u, "SonareProjectMarker layout drift");
static_assert(offsetof(SonareProjectMarker, ppq) == 8u, "SonareProjectMarker ppq offset");
#endif

/// @brief Adds or replaces a marker from a SonareProjectMarker, including
///        its kind and key signature, with an unbounded UTF-8 name.
/// @details @p marker->id 0 allocates a new id and UINT32_MAX is reserved
///          (SONARE_ERROR_INVALID_PARAMETER); the allocated / affected id is
///          returned via @p out_marker_id. For the key-signature kind
///          @p marker->key_fifths must be in -7..7 (the SMF `sf` range); an
///          out-of-range value yields SONARE_ERROR_INVALID_PARAMETER.
///          @p name is copied verbatim; NULL is the empty name.
SonareError sonare_project_set_marker(SonareProject* project, const SonareProjectMarker* marker,
                                      const char* name, uint32_t* out_marker_id);

/// @brief Reads a project marker by index (0-based, in stored order); the name
///        comes from @ref sonare_project_marker_name_by_index. An index
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

/// @brief Reads the route of a track by zero-based index.
/// @details An unset route field is returned as NULL. Release each returned string with
///          @ref sonare_free_string. The two output pointers must be distinct.
SonareError sonare_project_track_route_by_index(const SonareProject* project, size_t index,
                                                char** out_channel_strip_ref,
                                                char** out_output_target);

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

/// @brief Replaces the project's mixer scene from a scene document.
/// @details @p document is a mixer scene as @ref sonare_mixer_to_scene_json writes it
///   (schemas/mixer-scene.schema.json). A key the scene reader does not consume (other than `$`-
///   and `x-`-prefixed ones) fails the call with SONARE_ERROR_INVALID_FORMAT naming it, e.g.
///   `unknown key strips[2].faderDB`; the project's scene is left unchanged.
///   Pass NULL to clear the scene.
SonareError sonare_project_set_mixer_scene_json(SonareProject* project, const char* document);

/// @brief Reads the project's mixer scene as a scene document.
/// @details An unset scene returns NULL. Release a returned document with @ref sonare_free_string.
SonareError sonare_project_mixer_scene_json(const SonareProject* project, char** out_json);

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
/// @brief Reads the project's stored tempo segments in stored order.
/// @details This returns only stored segments, without inserting a default. An empty list has
///          NULL items and zero count. Release the copy with
///          @ref sonare_free_project_tempo_segment_list.
SonareError sonare_project_tempo_segments(const SonareProject* project,
                                          SonareProjectTempoSegmentList* out);

/// @brief Releases a tempo-segment list and clears it. NULL is safe.
void sonare_free_project_tempo_segment_list(SonareProjectTempoSegmentList* list);

/// @brief Reads the project's stored time-signature segments in stored order.
/// @details This returns only stored segments, without inserting a default. An empty list has
///          NULL items and zero count. Release the copy with
///          @ref sonare_free_project_time_signature_list.
SonareError sonare_project_time_signatures(const SonareProject* project,
                                           SonareProjectTimeSignatureList* out);

/// @brief Releases a time-signature list and clears it. NULL is safe.
void sonare_free_project_time_signature_list(SonareProjectTimeSignatureList* list);

/// @brief Opaque compiled-timeline handle. Create with @ref sonare_project_compile, destroy
///        with @ref sonare_project_timeline_destroy.
typedef struct SonareProjectTimeline SonareProjectTimeline;

/// @brief Compiles the project into an RT-readable timeline, surfacing
///        diagnostics, and optionally keeps the resulting timeline.
/// @details Bad input yields error diagnostics, not an error return. When @p out_timeline is
///          non-NULL and compilation produced a timeline (@c out->has_timeline non-zero),
///          @p *out_timeline receives a new handle; otherwise it is NULL and the call still
///          returns @c SONARE_OK, with the error diagnostics in @p out. A NULL @p out_timeline
///          compiles for diagnostics only. Both outputs are zeroed on entry. CONTROL thread.
/// @param out Zero-initialized result; free with
///        @ref sonare_free_project_compile_result.
/// @param out_timeline Receives the new timeline handle, or NULL; may itself be NULL.
/// @return @c SONARE_ERROR_INVALID_PARAMETER for a NULL @p project or @p out.
SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out,
                                   SonareProjectTimeline** out_timeline);

/// @brief Token of a @ref SonareProjectDiagnostic code ("danglingSourceRef", "clipOverlap", ...),
///        or NULL when @p code is not one. Contiguous from 0, so a caller lists the codes by
///        counting up until NULL. Static storage, never invalidated; do not free.
const char* sonare_project_diagnostic_code_name(int code);

/// @brief Token of a @ref SonareProjectDiagnostic severity ("error" or "warning"), or NULL when
///        @p severity is not one. Contiguous from 0. Static storage, never invalidated; do not
///        free.
const char* sonare_project_diagnostic_severity_name(int severity);

/// @brief Returns compile diagnostics produced by the most recent
///        @ref sonare_project_bounce on this project.
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

#ifdef __cplusplus
}
#endif
