#pragma once

/// @file part_rig_json.h
/// @brief The inserts_json reader and chain validation shared by the project and
///        engine part-rig entry points.

#include <sonare/sonare_c_types.h>

#include <string>
#include <vector>

#include "midi/part_rig.h"
#include "sonare_c_internal.h"
#include "util/json_budget.h"

#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif

namespace sonare_c_detail {

/// Parses the inserts array into chain stages. Shape only; names are resolved by
/// validate_part_rig_chain. A document that does not parse or has the wrong shape
/// is INVALID_FORMAT, as at every other C-ABI JSON entry point.
inline SonareError parse_part_rig_inserts(const char* inserts_json,
                                          std::vector<sonare::midi::PartRigStage>* stages) {
  namespace json = sonare::util::json;
  json::Value root;
  try {
    root = json::admit_strict(inserts_json);
  } catch (const json::JsonError& ex) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, ex.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  const auto bad_shape = [] {
    set_last_error(SONARE_ERROR_INVALID_FORMAT,
                   "inserts_json must be an array of {\"processor\": string, \"params\": string}");
    return SONARE_ERROR_INVALID_FORMAT;
  };
  if (!root.is_array()) return bad_shape();
  for (const json::Value& item : root.as_array()) {
    if (!item.is_object()) return bad_shape();
    const json::Value* processor = item.find("processor");
    if (processor == nullptr || !processor->is_string()) return bad_shape();
    sonare::midi::PartRigStage stage;
    stage.processor = processor->as_string();
    if (const json::Value* params = item.find("params")) {
      if (!params->is_string()) return bad_shape();
      stage.params_json = params->as_string();
    } else {
      stage.params_json = "{}";
    }
    stages->push_back(std::move(stage));
  }
  return SONARE_OK;
}

#if defined(SONARE_WITH_MASTERING)
/// Every stage must build, accept its params, and stay within the latency bound
/// at the reference rate.
inline bool validate_part_rig_chain(const std::vector<sonare::midi::PartRigStage>& stages) {
  constexpr double kValidationSampleRate = 48000.0;
  constexpr int kValidationBlockSize = 512;
  for (const sonare::midi::PartRigStage& stage : stages) {
    try {
      auto insert = sonare::mastering::api::make_insert(stage.processor, stage.params_json);
      if (!insert) return false;
      insert->prepare(kValidationSampleRate, kValidationBlockSize);
      if (insert->latency_samples_q8() > (sonare::midi::kMaxPartRigLatencySamples << 8)) {
        return false;
      }
    } catch (const sonare::SonareException&) {
      return false;
    }
  }
  return true;
}
#endif

}  // namespace sonare_c_detail
