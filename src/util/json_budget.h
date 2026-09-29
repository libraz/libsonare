#pragma once

/// @file json_budget.h
/// @brief Admits caller-supplied JSON text under a project-import-scaled
///        resource budget.
/// @details The one place a C-ABI entry point's own JSON text -- or a string
///          it lifts out of a request it already admitted -- is parsed. A
///          generic parser (util/json.h) has no opinion on how much of it a
///          caller may hand over, and the budget policy (util/resource_limits.h)
///          has no opinion on JSON; this header is the glue between the two so
///          neither depends on the other.

#include <string>

#include "util/json.h"
#include "util/resource_limits.h"

namespace sonare::util::json {

/// @brief Parses @p text as a strict JSON document under @p limits.
/// @details A byte precheck (so an oversized document is refused before the
///          parser allocates anything), then a duplicate-key-rejecting parse
///          bounded by the node and string-byte budget. Does not check that
///          the result is an object -- that is schema, and stays the caller's;
///          this function only decides how much text may be admitted at all.
/// @throws JsonResourceError naming the exceeded axis (byte, node or
///         string-byte budget) if @p text is too large to admit.
/// @throws JsonError for a text that is syntactically malformed once admitted.
inline Value admit_strict(const std::string& text,
                          const sonare::resource::ProjectImportResourceLimits& limits =
                              sonare::resource::kDefaultProjectImportResourceLimits) {
  if (text.size() > limits.max_json_bytes) {
    throw JsonResourceError("JSON byte budget exceeded", text.size());
  }
  return Parser(text, kDefaultMaxDepth, /*reject_duplicate_keys=*/true,
                ParseResourceLimits{limits.max_json_nodes, limits.max_string_bytes})
      .parse_document();
}

}  // namespace sonare::util::json
