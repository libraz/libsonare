#pragma once

#include "sonare_c_project_core.h"
#include "sonare_c_vocal_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SONARE_VOCAL_PROJECT_API_VERSION 1u
#define SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY 96u

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_REHYDRATE_REHYDRATED = 0,
  SONARE_VOCAL_REHYDRATE_ALREADY_READY = 1,
  SONARE_VOCAL_REHYDRATE_UNRESOLVED = 2
} SonareProjectVocalRehydrateStatus;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t clip_id;
  uint32_t take_id; /* 0 = clip base binding */
  uint32_t expected_source_id;
  uint32_t expected_source_sample_rate;
  int64_t expected_source_sample_count;
  uint8_t expected_source_sha256[32];
  double expected_clip_length_ppq;   /* start_ppq deliberately excluded: move is legal */
  double expected_source_offset_ppq; /* target take offset, or clip offset for take 0 */
  const float* rendered_mono;
  int64_t rendered_sample_count;
  uint32_t rendered_sample_rate;
  int64_t rendered_start_sample; /* v1 requires 0 */
  SonareVocalStateToken render_token;
  const uint8_t* sve1;
  uint64_t sve1_size;
} SonareProjectVocalEditApplyDesc;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t clip_id;
  uint32_t take_id;
  uint32_t original_source_id;
  uint32_t derived_source_id;
  uint64_t committed_revision;
  uint32_t profile_id;
  uint8_t derived_source_sha256[32];
  char sidecar_key[SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY];
} SonareProjectVocalEditApplyResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t clip_id;
  uint32_t take_id;
  uint32_t original_source_id;
  uint32_t derived_source_id;
  uint32_t source_sample_rate;
  uint32_t profile_id;
  int64_t source_sample_count;
  uint64_t committed_revision;
  uint8_t original_source_sha256[32];
  uint8_t derived_source_sha256[32];
  uint32_t original_pcm_available;
  uint32_t derived_pcm_available;
  uint32_t reason; /* SonareVocalReason; NONE means structurally valid */
  char sidecar_key[SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY];
} SonareProjectVocalEditDependency;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  SonareProjectVocalEditDependency* dependencies;
  uint64_t dependency_count;
} SonareProjectVocalEditDependenciesResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t source_id;
  const float* mono;
  int64_t sample_count;
  uint32_t sample_rate;
} SonareProjectVocalOriginalSource;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t clip_id;
  uint32_t take_id;
  uint32_t derived_source_id;
  uint32_t status; /* SonareProjectVocalRehydrateStatus */
  uint32_t reason; /* SonareVocalReason */
} SonareProjectVocalRehydrateItem;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  SonareProjectVocalRehydrateItem* items;
  uint64_t item_count;
} SonareProjectVocalRehydrateResult;

void sonare_project_vocal_edit_apply_desc_init(SonareProjectVocalEditApplyDesc* value);
void sonare_project_vocal_edit_apply_result_init(SonareProjectVocalEditApplyResult* value);
void sonare_project_vocal_edit_dependencies_result_init(
    SonareProjectVocalEditDependenciesResult* value);
void sonare_project_vocal_original_source_init(SonareProjectVocalOriginalSource* value);
void sonare_project_vocal_rehydrate_result_init(SonareProjectVocalRehydrateResult* value);

SonareError sonare_project_apply_vocal_edit(SonareProject* project,
                                            const SonareProjectVocalEditApplyDesc* desc,
                                            SonareProjectVocalEditApplyResult* result);

SonareError sonare_project_get_vocal_edit_dependencies(
    const SonareProject* project, SonareProjectVocalEditDependenciesResult* result);

SonareError sonare_project_rehydrate_vocal_edits(SonareProject* project,
                                                 const SonareProjectVocalOriginalSource* originals,
                                                 uint64_t original_count,
                                                 SonareVocalCancelCallback cancel, void* user_data,
                                                 SonareProjectVocalRehydrateResult* result);

void sonare_project_free_vocal_edit_dependencies(SonareProjectVocalEditDependenciesResult* result);
void sonare_project_free_vocal_rehydrate_result(SonareProjectVocalRehydrateResult* result);

#ifdef __cplusplus
}
#endif
