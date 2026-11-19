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
#include "util/db.h"

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

/// The declip threshold's key: dBFS on the wire, linear in `DeclipConfig::clip_threshold`.
inline constexpr const char* kDeclipThresholdKey = "clipThresholdDb";
inline constexpr ParamMeta kDeclipThresholdMeta = display_range(kDbfs, -6.0206, 0);

inline repair::DeclipConfig declip_config(const ParamMap& params) {
  repair::DeclipConfig config;
  SONARE_FIELDS_DECLIP(SONARE_READ_FIELD)
  float threshold_db = sonare::linear_to_db(config.clip_threshold);
  read_field(params, kDeclipThresholdKey, threshold_db, kDeclipThresholdMeta);
  if (params.find(kDeclipThresholdKey) != params.end()) {
    config.clip_threshold = repair::clip_threshold_from_db(threshold_db, "clip_threshold_db");
  }
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

/// The trim threshold's key: dBFS peak on the wire, linear in `TrimSilenceConfig::threshold`.
inline constexpr const char* kTrimThresholdKey = "thresholdDb";
/// Its default, the linear 0.001 the config holds when the key is absent.
inline constexpr float kTrimThresholdDefaultDb = -60.0f;
inline constexpr ParamMeta kTrimThresholdMeta = display_range(kDbfs, -144, -20);

inline repair::TrimSilenceConfig trim_silence_config(const ParamMap& params) {
  repair::TrimSilenceConfig config;
  SONARE_FIELDS_TRIM_SILENCE(SONARE_READ_FIELD)
  float threshold_db = kTrimThresholdDefaultDb;
  read_field(params, kTrimThresholdKey, threshold_db, kTrimThresholdMeta);
  if (params.find(kTrimThresholdKey) != params.end()) {
    config.threshold = repair::trim_threshold_from_db(threshold_db, kTrimThresholdKey);
  }
  repair::validate_config(config);
  return config;
}

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLICK, repair::DeclickConfig, 0);
// clip_threshold is the one field the table leaves out: it is read as `clipThresholdDb` above.
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLIP, repair::DeclipConfig, 1);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECRACKLE, repair::DecrackleConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEHUM, repair::DehumConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DENOISE_CLASSICAL, repair::DenoiseClassicalConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEREVERB_CLASSICAL, repair::DereverbClassicalConfig, 0);
// threshold is left out the same way: it is read as `thresholdDb` above.
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRIM_SILENCE, repair::TrimSilenceConfig, 1);

}  // namespace sonare::mastering::api::detail
