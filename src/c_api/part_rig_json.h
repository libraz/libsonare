#pragma once

/// @file part_rig_json.h
/// @brief The inserts_json reader and chain validation shared by the project and
///        engine part-rig setters.
/// @details inserts_json is a per-call argument, not a document the library writes and
///          reads back, so every refusal here is a SonareParameterRefusal under
///          InvalidParameter rooted at @c inserts, the name every surface gives the
///          argument; a wrong type, a missing processor and an unknown key are shape
///          refusals. Subjects are already in the
///          caller's spelling and marked so. The project document's own
///          part-rig reader lives in the serializer. Each function throws, so it is
///          called inside an entry's exception translation.

#include <sonare/sonare_c_types.h>

#include <string>
#include <vector>

#include "midi/part_rig.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_schema.h"

namespace sonare_c_detail {

inline constexpr const char* kPartRigInsertsSubject = "inserts";

[[noreturn]] inline void refuse_part_rig(const std::string& subject, const std::string& predicate) {
  throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter, subject,
                                                  predicate)
      .as_caller_spelled();
}

[[noreturn]] inline void refuse_part_rig_type(const std::string& subject,
                                              sonare::util::json::schema::Kind kind) {
  throw sonare::SonareParameterRefusal::wrong_type(
      sonare::ErrorCode::InvalidParameter, subject,
      std::string("must be ") + sonare::util::json::schema::kind_text(kind))
      .as_caller_spelled();
}

/// Reads the inserts array into chain stages. Shape only; names, params and latency are
/// resolved by mastering::api::validate_part_rig_chain.
inline std::vector<sonare::midi::PartRigStage> read_part_rig_inserts(const char* inserts_json) {
  namespace json = sonare::util::json;
  namespace schema = sonare::util::json::schema;
  json::Value root;
  try {
    root = json::admit_strict(inserts_json);
  } catch (const json::JsonError& ex) {
    refuse_part_rig(kPartRigInsertsSubject, std::string("must be valid JSON: ") + ex.what());
  }
  if (!root.is_array()) refuse_part_rig_type(kPartRigInsertsSubject, schema::Kind::Array);
  std::vector<sonare::midi::PartRigStage> stages;
  std::size_t index = 0;
  for (const json::Value& item : root.as_array()) {
    const std::string at = schema::index_path(kPartRigInsertsSubject, index++);
    if (!item.is_object()) refuse_part_rig_type(at, schema::Kind::Object);
    for (const auto& entry : item.as_object()) {
      const std::string& key = entry.first;
      if (key == "processor" || key == "params" || json::is_annotation_key(key)) continue;
      throw sonare::SonareParameterRefusal::unknown(sonare::ErrorCode::InvalidParameter,
                                                    schema::child_path(at, key), std::string(),
                                                    "processor, params")
          .as_caller_spelled();
    }
    const std::string processor_at = schema::child_path(at, "processor");
    const json::Value* processor = item.find("processor");
    if (processor == nullptr) {
      throw sonare::SonareParameterRefusal::wrong_type(sonare::ErrorCode::InvalidParameter,
                                                       processor_at, "must be given")
          .as_caller_spelled();
    }
    if (!processor->is_string()) refuse_part_rig_type(processor_at, schema::Kind::String);
    if (processor->as_string().empty()) refuse_part_rig(processor_at, "must not be empty");
    sonare::midi::PartRigStage stage;
    stage.processor = processor->as_string();
    if (const json::Value* params = item.find("params")) {
      if (!params->is_object()) {
        refuse_part_rig_type(schema::child_path(at, "params"), schema::Kind::Object);
      }
      stage.params_json = json::dump(*params);
    } else {
      stage.params_json = "{}";
    }
    stages.push_back(std::move(stage));
  }
  return stages;
}

/// The refusal of a rig validate_part_rig rejects once its part has been admitted:
/// what is left is the stage count of the inserts.
[[noreturn]] inline void refuse_part_rig_stage_count() {
  refuse_part_rig(kPartRigInsertsSubject, "must hold 1 to " +
                                              std::to_string(sonare::midi::kMaxPartRigStages) +
                                              " entries for a chain rig");
}

}  // namespace sonare_c_detail
