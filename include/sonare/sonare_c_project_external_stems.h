#pragma once

/// @file sonare_c_project_external_stems.h
/// @brief Host-provided separated PCM stem import for a headless project.
///        Included via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Channel layouts accepted by @ref sonare_project_import_external_stems.
typedef enum SONARE_ENUM_BASE {
  SONARE_EXTERNAL_STEM_MONO = 1,
  SONARE_EXTERNAL_STEM_STEREO = 2,
} SonareExternalStemLayout;

/// One decoded output from a host-owned external separator.
///
/// @p name is required, NUL-terminated UTF-8, and unique within the input set.
/// @p role is an optional NUL-terminated UTF-8 label; it is retained as source
/// metadata and never changes DSP. @p planar_samples contains one non-null
/// pointer per @p layout channel. Each plane has @p frame_count frames at the
/// enclosing request's sample rate. All input is copied before this function
/// returns, so the caller retains ownership of every pointer.
typedef struct {
  const char* name;  ///< Stem name; required, NUL-terminated UTF-8, unique within the request.
  const char* role;  ///< Optional NUL-terminated UTF-8 label kept as source metadata; may be NULL.
  uint32_t layout;   ///< A @ref SonareExternalStemLayout giving the number of planes.
  const float* const* planar_samples;  ///< One non-NULL plane pointer per channel of layout.
  size_t frame_count;                  ///< Frames in each plane; must be positive.
  int64_t start_frame;  ///< Clip start on the project timeline in sample frames; non-negative.
} SonareExternalStemDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareExternalStemDesc, name) == 0, "ExternalStemDesc.name offset");
static_assert(offsetof(SonareExternalStemDesc, role) == sizeof(void*),
              "ExternalStemDesc.role offset");
static_assert(offsetof(SonareExternalStemDesc, layout) == 2u * sizeof(void*),
              "ExternalStemDesc.layout offset");
static_assert(offsetof(SonareExternalStemDesc, planar_samples) ==
                  ((2u * sizeof(void*) + sizeof(uint32_t) + sizeof(void*) - 1u) / sizeof(void*) *
                   sizeof(void*)),
              "ExternalStemDesc.planar_samples offset");
static_assert(offsetof(SonareExternalStemDesc, frame_count) ==
                  offsetof(SonareExternalStemDesc, planar_samples) + sizeof(void*),
              "ExternalStemDesc.frame_count offset");
static_assert(offsetof(SonareExternalStemDesc, start_frame) ==
                  (offsetof(SonareExternalStemDesc, frame_count) + sizeof(size_t) +
                   alignof(int64_t) - 1u) /
                      alignof(int64_t) * alignof(int64_t),
              "ExternalStemDesc.start_frame offset");
#endif

/// Request for @ref sonare_project_import_external_stems. Start from
/// @ref sonare_external_stem_import_request_init, then set the fields; every field is read
/// literally. @p sample_rate must exactly equal the target project's rate: this API never
/// resamples, retimes, phase-aligns, normalizes, or compensates gain.
typedef struct {
  /// sizeof(SonareExternalStemImportRequest); set by the init function.
  uint32_t struct_size;
  /// Sample rate of every stem; must equal the project's. Default 0, which no project has, so the
  /// caller always sets it.
  int sample_rate;
  /// The stems, @p stem_count entries. Default NULL.
  const SonareExternalStemDesc* stems;
  /// Number of stems; at least 1. Default 0.
  size_t stem_count;
} SonareExternalStemImportRequest;

/// @brief Writes every default into @p request and sets its @c struct_size; NULL is ignored.
///        Allocates nothing; @c stems is left NULL, so the request does not transfer ownership.
void sonare_external_stem_import_request_init(SonareExternalStemImportRequest* request);

#ifdef __cplusplus
static_assert(offsetof(SonareExternalStemImportRequest, struct_size) == 0,
              "ExternalStemImportRequest.struct_size offset");
static_assert(offsetof(SonareExternalStemImportRequest, sample_rate) == sizeof(uint32_t),
              "ExternalStemImportRequest.sample_rate offset");
static_assert(offsetof(SonareExternalStemImportRequest, stems) == 2u * sizeof(uint32_t),
              "ExternalStemImportRequest.stems offset");
static_assert(offsetof(SonareExternalStemImportRequest, stem_count) ==
                  offsetof(SonareExternalStemImportRequest, stems) + sizeof(void*),
              "ExternalStemImportRequest.stem_count offset");
static_assert(sizeof(SonareExternalStemImportRequest) ==
                  offsetof(SonareExternalStemImportRequest, stem_count) + sizeof(size_t),
              "ExternalStemImportRequest layout drift");
#endif

/// Heap-owned IDs allocated by @ref sonare_project_import_external_stems.
/// Release with @ref sonare_free_external_stem_import_result.
typedef struct {
  uint32_t* track_ids;  ///< Created track ids, in input order; heap-owned, count entries.
  uint32_t* clip_ids;   ///< Created clip ids, in input order; heap-owned, count entries.
  size_t count;         ///< Number of stems imported.
} SonareExternalStemImportResult;

#ifdef __cplusplus
static_assert(offsetof(SonareExternalStemImportResult, track_ids) == 0,
              "ExternalStemImportResult.track_ids offset");
static_assert(offsetof(SonareExternalStemImportResult, clip_ids) == sizeof(void*),
              "ExternalStemImportResult.clip_ids offset");
static_assert(offsetof(SonareExternalStemImportResult, count) == 2u * sizeof(void*),
              "ExternalStemImportResult.count offset");
static_assert(sizeof(SonareExternalStemImportResult) == 2u * sizeof(void*) + sizeof(size_t),
              "ExternalStemImportResult layout drift");
#endif

/// Import one ordinary audio track and clip for every externally separated
/// stem. The operation is all-or-nothing: on failure the project and its audio
/// content are unchanged. Output tracks use the normal master routing and
/// mixer/bounce path. This CONTROL-THREAD-ONLY structural import does not run
/// a separator and does not perform file or device I/O.
SonareError sonare_project_import_external_stems(SonareProject* project,
                                                 const SonareExternalStemImportRequest* request,
                                                 SonareExternalStemImportResult* out);

/// Frees ID arrays returned by @ref sonare_project_import_external_stems.
/// Safe on a zero-initialized result.
void sonare_free_external_stem_import_result(SonareExternalStemImportResult* result);

#ifdef __cplusplus
}
#endif
