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
  const char* name;
  const char* role;
  uint32_t layout; /* SonareExternalStemLayout */
  const float* const* planar_samples;
  int64_t frame_count;
  int64_t start_frame;
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
                  offsetof(SonareExternalStemDesc, frame_count) + sizeof(int64_t),
              "ExternalStemDesc.start_frame offset");
#endif

/// Versioned request for @ref sonare_project_import_external_stems.
/// Zero-initialize for the current layout; @c struct_version 0 and 1 are
/// accepted. @p sample_rate must exactly equal the target project's rate: this
/// API never resamples, retimes, phase-aligns, normalizes, or compensates gain.
typedef struct {
  int struct_version;
  int sample_rate;
  const SonareExternalStemDesc* stems;
  size_t stem_count;
} SonareExternalStemImportRequest;

#ifdef __cplusplus
static_assert(offsetof(SonareExternalStemImportRequest, struct_version) == 0,
              "ExternalStemImportRequest.struct_version offset");
static_assert(offsetof(SonareExternalStemImportRequest, sample_rate) == sizeof(int),
              "ExternalStemImportRequest.sample_rate offset");
static_assert(offsetof(SonareExternalStemImportRequest, stems) == 2u * sizeof(int),
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
  uint32_t* track_ids;
  uint32_t* clip_ids;
  size_t count;
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
