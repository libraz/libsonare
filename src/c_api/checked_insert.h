#pragma once

/// @file checked_insert.h
/// @brief Insert construction that refuses parameters no processor reads.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/param_map.h"
#include "mastering/api/retired_names.h"
#include "mixing/api/scene.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/resource_limits.h"

namespace sonare::mastering::api {

/// Builds an insert and refuses its first unread parameter under @p params_path.
/// Unknown processor names retain make_insert()'s nullptr result.
inline std::unique_ptr<sonare::rt::ProcessorBase> make_checked_insert(
    const std::string& name, const std::string& params_json, const std::string& params_path,
    ErrorCode code, double build_sample_rate = 0.0) {
  namespace schema = sonare::util::json::schema;
  const auto spell_subject = [&params_path](const std::string& subject) {
    return params_path.empty() ? subject : schema::child_path(params_path, subject);
  };
  std::vector<std::string> unread;
  std::unique_ptr<sonare::rt::ProcessorBase> processor;
  try {
    processor = make_insert(name, params_json, &unread,
                            resource::kDefaultProjectImportResourceLimits, build_sample_rate);
  } catch (const SonareParameterRefusal& refusal) {
    if (refusal.code() != ErrorCode::InvalidParameter) throw;
    throw refusal.respelled(code, spell_subject, code == ErrorCode::InvalidParameter);
  } catch (const SonareException& error) {
    if (error.code() != ErrorCode::InvalidParameter) throw;
    // An unstructured refusal is a params-object fault (parse, budget, shape); root it at the path.
    const std::string subject = params_path.empty() ? std::string("params") : params_path;
    const std::string predicate =
        std::string("must be a valid insert parameter object: ") + error.what();
    const SonareParameterRefusal refusal =
        error.reason() ? SonareParameterRefusal::predicate_with_reason(code, subject, predicate,
                                                                       *error.reason())
                       : SonareParameterRefusal::predicate(code, subject, predicate);
    throw refusal.respelled(
        code, [](const std::string& same) { return same; }, code == ErrorCode::InvalidParameter);
  }
  if (!processor || unread.empty()) return processor;

  try {
    detail::refuse_unread_params(unread, insert_param_names(name));
  } catch (const SonareParameterRefusal& refusal) {
    throw refusal.respelled(code, spell_subject, code == ErrorCode::InvalidParameter);
  }
  return processor;
}

/// Refuses a processor name the factory does not know, at the document path of its key.
[[noreturn]] inline void refuse_unknown_scene_processor(const std::string& insert_path,
                                                        const std::string& name) {
  namespace schema = sonare::util::json::schema;
  throw SonareException(
      ErrorCode::InvalidFormat,
      schema::refusal(schema::child_path(insert_path, "processor"),
                      "must name a known processor" + retired_processor_note(name)));
}

/// Refuses unknown processors and unread insert parameters anywhere in a parsed scene document.
/// @p path is empty for a scene document or `scene` inside a project document.
inline void validate_scene_insert_params(const mixing::api::Scene& scene, std::string_view path,
                                         double build_sample_rate) {
  namespace schema = sonare::util::json::schema;
  const auto validate = [build_sample_rate](const mixing::api::Insert& insert,
                                            const std::string& insert_path) {
    if (!make_checked_insert(insert.processor_name, insert.params_json,
                             schema::child_path(insert_path, "params"), ErrorCode::InvalidFormat,
                             build_sample_rate)) {
      refuse_unknown_scene_processor(insert_path, insert.processor_name);
    }
  };

  for (std::size_t strip_index = 0; strip_index < scene.strips.size(); ++strip_index) {
    const std::string strip_path =
        schema::index_path(schema::child_path(path, "strips"), strip_index);
    const auto& strip = scene.strips[strip_index];
    for (std::size_t insert_index = 0; insert_index < strip.inserts.size(); ++insert_index) {
      const std::string insert_path =
          schema::index_path(schema::child_path(strip_path, "inserts"), insert_index);
      validate(strip.inserts[insert_index], insert_path);
    }
  }

  for (std::size_t bus_index = 0; bus_index < scene.buses.size(); ++bus_index) {
    const std::string bus_path = schema::index_path(schema::child_path(path, "buses"), bus_index);
    const auto& bus = scene.buses[bus_index];
    for (std::size_t insert_index = 0; insert_index < bus.inserts.size(); ++insert_index) {
      const std::string insert_path =
          schema::index_path(schema::child_path(bus_path, "inserts"), insert_index);
      validate(bus.inserts[insert_index], insert_path);
    }
  }
}

}  // namespace sonare::mastering::api
