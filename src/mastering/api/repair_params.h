#pragma once

/// @file repair_params.h
/// @brief Internal: config builders for the repair stages, read from a flat parameter map.

#include "mastering/api/param_field_tables.h"
#include "mastering/api/param_map.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"

namespace sonare::mastering::api::detail {

// ---------------------------------------------------------------------------
// Repair
//
// Each builder validates what it read, so a value the stage would refuse is
// refused where it is first accepted, on the named path and in the catalog's
// measurement alike.
// ---------------------------------------------------------------------------

inline repair::DeclickConfig declick_config(const ParamMap& params) {
  repair::DeclickConfig config;
  SONARE_FIELDS_DECLICK(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DeclipConfig declip_config(const ParamMap& params) {
  repair::DeclipConfig config;
  SONARE_FIELDS_DECLIP(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DecrackleConfig decrackle_config(const ParamMap& params) {
  repair::DecrackleConfig config;
  SONARE_FIELDS_DECRACKLE(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DehumConfig dehum_config(const ParamMap& params) {
  repair::DehumConfig config;
  SONARE_FIELDS_DEHUM(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DenoiseClassicalConfig denoise_classical_config(const ParamMap& params) {
  repair::DenoiseClassicalConfig config;
  SONARE_FIELDS_DENOISE_CLASSICAL(SONARE_READ_FIELD)
  note_pair_order(params, "hopLength", Relation::Le, "nFft", 0.5);
  repair::validate_config(config);
  return config;
}

inline repair::DereverbClassicalConfig dereverb_classical_config(const ParamMap& params) {
  repair::DereverbClassicalConfig config;
  SONARE_FIELDS_DEREVERB_CLASSICAL(SONARE_READ_FIELD)
  note_pair_order(params, "hopLength", Relation::Le, "nFft", 0.5);
  repair::validate_config(config);
  return config;
}

inline repair::TrimSilenceConfig trim_silence_config(const ParamMap& params) {
  repair::TrimSilenceConfig config;
  SONARE_FIELDS_TRIM_SILENCE(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLICK, repair::DeclickConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLIP, repair::DeclipConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECRACKLE, repair::DecrackleConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEHUM, repair::DehumConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DENOISE_CLASSICAL, repair::DenoiseClassicalConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEREVERB_CLASSICAL, repair::DereverbClassicalConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRIM_SILENCE, repair::TrimSilenceConfig, 0);

}  // namespace sonare::mastering::api::detail
