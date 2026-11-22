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
#include "util/json_schema.h"

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
  namespace schema = sonare::util::json::schema;
  const auto refuse = [](const std::string& message) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, message.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  };
  if (!root.is_array()) return refuse(schema::must_be("", schema::Kind::Array));
  std::size_t index = 0;
  for (const json::Value& item : root.as_array()) {
    const std::string at = schema::index_path("", index++);
    if (!item.is_object()) return refuse(schema::must_be(at, schema::Kind::Object));
    const std::string processor_at = schema::child_path(at, "processor");
    const json::Value* processor = item.find("processor");
    if (processor == nullptr) return refuse(schema::missing_key(processor_at));
    if (!processor->is_string()) return refuse(schema::must_be(processor_at, schema::Kind::String));
    if (processor->as_string().empty()) {
      return refuse(schema::refusal(processor_at, "must not be empty"));
    }
    sonare::midi::PartRigStage stage;
    stage.processor = processor->as_string();
    if (const json::Value* params = item.find("params")) {
      if (!params->is_object()) {
        return refuse(schema::must_be(schema::child_path(at, "params"), schema::Kind::Object));
      }
      stage.params_json = json::dump(*params);
    } else {
      stage.params_json = "{}";
    }
    stages->push_back(std::move(stage));
  }
  return SONARE_OK;
}

/// The refusal of a rig validate_part_rig rejects once its part has been admitted:
/// what is left is the stage count of the inserts document.
inline SonareError refuse_part_rig_stage_count() {
  set_last_error(
      SONARE_ERROR_INVALID_FORMAT,
      sonare::util::json::schema::refusal(
          "the document", "must hold 1 to " + std::to_string(sonare::midi::kMaxPartRigStages) +
                              " inserts for a chain rig")
          .c_str());
  return SONARE_ERROR_INVALID_FORMAT;
}

#if defined(SONARE_WITH_MASTERING)
/// Every stage must build, accept its params, and stay within the latency bound
/// at the reference rate.
inline bool validate_part_rig_chain(const std::vector<sonare::midi::PartRigStage>& stages) {
  constexpr double kValidationSampleRate = 48000.0;
  constexpr int kValidationBlockSize = 512;
  namespace schema = sonare::util::json::schema;
  const auto refuse = [](const std::string& message) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, message.c_str());
    return false;
  };
  for (std::size_t i = 0; i < stages.size(); ++i) {
    const sonare::midi::PartRigStage& stage = stages[i];
    const std::string at = schema::index_path("", i);
    try {
      auto insert = sonare::mastering::api::make_insert(stage.processor, stage.params_json);
      if (!insert) {
        return refuse(schema::refusal(schema::child_path(at, "processor"),
                                      "must name a processor that can be built"));
      }
      insert->prepare(kValidationSampleRate, kValidationBlockSize);
      if (insert->latency_samples_q8() > (sonare::midi::kMaxPartRigLatencySamples << 8)) {
        return refuse(schema::refusal(
            at, "must report a latency of at most " +
                    std::to_string(sonare::midi::kMaxPartRigLatencySamples) + " samples"));
      }
    } catch (const sonare::SonareParameterRefusal& ex) {
      return refuse(schema::relabel_under(ex, schema::child_path(at, "params"), ex.code()).what());
    } catch (const sonare::SonareException& ex) {
      return refuse(schema::refusal(at, std::string("is refused: ") + ex.what()));
    }
  }
  return true;
}
#endif

}  // namespace sonare_c_detail
