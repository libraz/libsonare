#pragma once

/// @file schema_paths.h
/// @brief Collect the dotted field paths a serialized JSON value actually
///        carries, for comparison against the list its producer publishes.
///
/// Several results cross to user code as a JSON string each facade parses and
/// casts, so nothing type-checks them on arrival. Each producer publishes the
/// paths it emits, and the check that gives that list its authority is a set
/// equality against a serialized fixture: every emitted path must be listed and
/// every listed path must be emitted, so the list cannot drift in either
/// direction. Equality is the whole point — a one-sided containment check passes
/// while half the schema goes unlisted.
///
/// An array contributes its element type's paths under a `[]` segment and
/// nothing of its own, so a fixture must populate every array it wants covered:
/// an empty one is indistinguishable from a field that does not exist.

#include <set>
#include <string>

#include "util/json.h"

namespace sonare::test {

/// Insert every dotted path reachable under `prefix` into `out`.
inline void collect_schema_paths(const sonare::util::json::Value& value, const std::string& prefix,
                                 std::set<std::string>& out) {
  if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      const std::string path = prefix.empty() ? key : prefix + "." + key;
      out.insert(path);
      collect_schema_paths(child, path, out);
    }
    return;
  }
  if (value.is_array() && value.size() > 0) {
    collect_schema_paths(value[static_cast<std::size_t>(0)], prefix + "[]", out);
  }
}

/// Every dotted path in `json`, parsed and walked from the root.
inline std::set<std::string> schema_paths_of(const std::string& json) {
  std::set<std::string> out;
  collect_schema_paths(sonare::util::json::parse(json), "", out);
  return out;
}

}  // namespace sonare::test
