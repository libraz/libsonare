#pragma once

#include "sonare_c_project_core.h"
#include "sonare_c_vocal_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version of the vocal project structs and functions; raised when a layout or signature
/// changes.
#define SONARE_VOCAL_PROJECT_ABI_VERSION 1u
/// Capacity in bytes, including the terminating NUL, of the sidecar_key char arrays.
#define SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY 96u

/// Outcome of one entry of a rehydrate.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_REHYDRATE_REHYDRATED = 0,     ///< Derived PCM was re-rendered and stored.
  SONARE_VOCAL_REHYDRATE_ALREADY_READY = 1,  ///< Derived PCM was already present and matches.
  SONARE_VOCAL_REHYDRATE_UNRESOLVED = 2      ///< Not rehydrated; see the item's reason.
} SonareProjectVocalRehydrateStatus;

/// @brief Describes the rendered PCM and state to bind.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by the init function.
  uint32_t clip_id;      ///< Clip receiving the derived source; must exist on an audio track.
  uint32_t take_id;      ///< Take of the clip to bind; 0 binds the clip's base source.
  uint32_t
      expected_source_id;  ///< The root original source, even when the binding is already derived.
  uint32_t expected_source_sample_rate;  ///< Sample rate of the original, in [8000, 384000] Hz.
  int64_t expected_source_sample_count;  ///< Length of the original in frames; must be positive.
  uint8_t expected_source_sha256[32];    ///< SHA-256 digest of the original PCM.
  double
      expected_clip_length_ppq;  ///< Clip length in quarter notes; start position is not checked.
  double
      expected_source_offset_ppq;  ///< Source offset of the target take (clip offset for take 0).
  const float* rendered_mono;      ///< Rendered mono PCM; must not be NULL.
  int64_t rendered_sample_count;   ///< Frames in rendered_mono; must equal the original's count.
  uint32_t rendered_sample_rate;   ///< Sample rate in Hz; must equal the original's rate.
  int64_t rendered_start_sample;   ///< Start offset of the render in the source; must be 0.
  SonareVocalStateToken render_token;  ///< State token the render was produced from.
  const uint8_t* sve1;                 ///< Serialized vocal-edit state bytes; must not be NULL.
  uint64_t sve1_size;                  ///< Size of sve1 in bytes; must be nonzero.
} SonareProjectVocalEditApplyDesc;

/// @brief Outcome of @ref sonare_project_apply_vocal_edit. Initialize with
///        @ref sonare_project_vocal_edit_apply_result_init before the call.
typedef struct {
  uint32_t struct_size;         ///< sizeof of this struct; set by the init function.
  uint32_t clip_id;             ///< Clip that was edited.
  uint32_t take_id;             ///< Take that was edited; 0 for the clip's base source.
  uint32_t original_source_id;  ///< Root original source recorded in the envelope.
  uint32_t derived_source_id;   ///< Newly stored derived source now bound to the clip or take.
  uint64_t committed_revision;  ///< Revision of the committed vocal-edit state.
  uint32_t profile_id;          ///< Profile identifier carried by the committed state.
  uint8_t derived_source_sha256[32];  ///< SHA-256 digest of the derived PCM.
  char sidecar_key[SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY];  ///< NUL-terminated sidecar key.
} SonareProjectVocalEditApplyResult;

/// @brief The recorded envelope of one vocal-edit sidecar and whether its PCM is present.
typedef struct {
  uint32_t struct_size;         ///< sizeof of this struct; set by the init function.
  uint32_t clip_id;             ///< Clip the sidecar belongs to.
  uint32_t take_id;             ///< Take the sidecar belongs to; 0 for the clip's base source.
  uint32_t original_source_id;  ///< Root original source id.
  uint32_t derived_source_id;   ///< Derived source id produced by the edit.
  uint32_t source_sample_rate;  ///< Sample rate of both sources, in Hz.
  uint32_t profile_id;          ///< Profile identifier carried by the recorded state.
  int64_t source_sample_count;  ///< Length of both sources in frames.
  uint64_t committed_revision;  ///< Revision of the recorded vocal-edit state.
  uint8_t original_source_sha256[32];  ///< Expected SHA-256 digest of the original PCM.
  uint8_t derived_source_sha256[32];   ///< Expected SHA-256 digest of the derived PCM.
  uint32_t original_pcm_available;     ///< 1 when the project holds matching original PCM, else 0.
  uint32_t derived_pcm_available;      ///< 1 when the project holds matching derived PCM, else 0.
  uint32_t reason;  ///< A SonareVocalReason; NONE means the envelope is structurally valid.
  char sidecar_key[SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY];  ///< NUL-terminated sidecar key.
} SonareProjectVocalEditDependency;

/// @brief One entry per vocal-edit sidecar of a project.
///   Library-allocated arrays; release them with
///   sonare_free_project_vocal_edit_dependencies_result, which also resets the struct.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by the init function.
  SonareProjectVocalEditDependency* dependencies;  ///< Library-owned array; NULL when empty.
  uint64_t dependency_count;                       ///< Number of entries in dependencies.
} SonareProjectVocalEditDependenciesResult;

/// @brief Caller-supplied original PCM for rehydrate.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by the init function.
  uint32_t source_id;    ///< Project source id this PCM is the original of.
  const float* mono;     ///< Mono PCM of the original; borrowed, copied during the call.
  int64_t sample_count;  ///< Frames in mono.
  uint32_t sample_rate;  ///< Sample rate of mono in Hz.
} SonareProjectVocalOriginalSource;

/// @brief Outcome of rehydrating one vocal-edit sidecar.
typedef struct {
  uint32_t struct_size;        ///< sizeof of this struct; set by the init function.
  uint32_t clip_id;            ///< Clip the sidecar belongs to.
  uint32_t take_id;            ///< Take the sidecar belongs to; 0 for the clip's base source.
  uint32_t derived_source_id;  ///< Derived source id the entry restores.
  uint32_t status;             ///< A @ref SonareProjectVocalRehydrateStatus.
  uint32_t reason;             ///< A SonareVocalReason; NONE unless status is UNRESOLVED.
} SonareProjectVocalRehydrateItem;

/// @brief Per-entry outcome of a rehydrate.
///   Library-allocated arrays; release them with sonare_free_project_vocal_rehydrate_result, which
///   also resets the struct.
typedef struct {
  uint32_t struct_size;                    ///< sizeof of this struct; set by the init function.
  SonareProjectVocalRehydrateItem* items;  ///< Library-owned array; NULL when empty.
  uint64_t item_count;                     ///< Number of entries in items.
} SonareProjectVocalRehydrateResult;

/// @brief Initializes @p value with the current struct size and schema version.
void sonare_project_vocal_edit_apply_desc_init(SonareProjectVocalEditApplyDesc* value);
/// @brief Initializes @p value with the current struct size and schema version.
void sonare_project_vocal_edit_apply_result_init(SonareProjectVocalEditApplyResult* value);
/// @brief Initializes @p value with the current struct size and schema version.
void sonare_project_vocal_edit_dependencies_result_init(
    SonareProjectVocalEditDependenciesResult* value);
/// @brief Initializes @p value with the current struct size and schema version.
void sonare_project_vocal_original_source_init(SonareProjectVocalOriginalSource* value);
/// @brief Initializes @p value with the current struct size and schema version.
void sonare_project_vocal_rehydrate_result_init(SonareProjectVocalRehydrateResult* value);

/// @brief Binds rendered PCM as a derived source of a clip or take and records its envelope.
/// @details When the binding is already a vocal-edit derived source, the edit is re-based on that
///   envelope's root original: the state must be restored against the original, and the envelope
///   is replaced by original -> new derived. A state authored against the derived source returns
///   SONARE_ERROR_INVALID_STATE. Also SONARE_ERROR_INVALID_STATE while the project is inside
///   sonare_project_rehydrate_vocal_edits.
SonareError sonare_project_apply_vocal_edit(SonareProject* project,
                                            const SonareProjectVocalEditApplyDesc* desc,
                                            SonareProjectVocalEditApplyResult* result);

/// @brief Lists every vocal-edit sidecar of the project with its envelope and PCM availability.
/// @param result Initialized with @ref sonare_project_vocal_edit_dependencies_result_init; on
///   success holds a library-allocated array released with
///   @ref sonare_free_project_vocal_edit_dependencies_result.
SonareError sonare_project_get_vocal_edit_dependencies(
    const SonareProject* project, SonareProjectVocalEditDependenciesResult* result);

/// @brief Re-renders missing derived PCM from the recorded envelopes.
/// @param cancel Optional; @p cancel and @p user_data are used only during the call and are not
///   retained afterwards.
/// @details @p cancel runs between entries. If it changes the project's vocal edits, staged PCM
///   is discarded and SONARE_ERROR_INVALID_STATE is returned; vocal apply and nested rehydrate on
///   the same project are refused with SONARE_ERROR_INVALID_STATE while this call runs.
SonareError sonare_project_rehydrate_vocal_edits(SonareProject* project,
                                                 const SonareProjectVocalOriginalSource* originals,
                                                 uint64_t original_count,
                                                 SonareVocalCancelCallback cancel, void* user_data,
                                                 SonareProjectVocalRehydrateResult* result);

/// @brief Releases the array in @p result and resets it; safe on an empty result.
void sonare_free_project_vocal_edit_dependencies_result(
    SonareProjectVocalEditDependenciesResult* result);
/// @brief Releases the array in @p result and resets it; safe on an empty result.
void sonare_free_project_vocal_rehydrate_result(SonareProjectVocalRehydrateResult* result);

#ifdef __cplusplus
}
#endif
