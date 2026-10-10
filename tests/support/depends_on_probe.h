/// @file depends_on_probe.h
/// @brief Sibling values that keep a probed parameter inside the relations its descriptor declares.
///
/// A catalog `choices` list or bound names the values a parameter takes at SOME setting of its
/// siblings, and states the coupling in `dependsOn` rather than in the bound. A test that builds
/// the processor at such a value therefore has to move the coupled siblings off their defaults
/// first; this derives that from the descriptors, with no per-processor list.

#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/json.h"

namespace sonare::test {

/// Sibling (key, value) pairs that satisfy every `dependsOn` relation joining @p key at @p value
/// to another parameter of @p name, read from both ends of the relation. A `le` / `ge` sibling
/// moves only when its default would violate the relation; `lt` / `gt` are not adjusted.
inline std::vector<std::pair<std::string, double>> depends_on_siblings(const std::string& name,
                                                                       const std::string& key,
                                                                       double value) {
  namespace json = sonare::util::json;
  const json::Value info = json::parse_strict(mastering::api::insert_param_info_json(name));
  const auto default_of = [&](const std::string& sibling, double* out) {
    for (const json::Value& entry : info.as_array()) {
      if (entry["name"].as_string() == sibling && entry["default"].is_number()) {
        *out = entry["default"].as_number();
        return true;
      }
    }
    return false;
  };
  std::vector<std::pair<std::string, double>> moved;
  const auto constrain = [&](const std::string& sibling, const std::string& relation,
                             double bound) {
    double current = 0.0;
    if (!default_of(sibling, &current)) return;
    // `relation` is the sibling's relation to @p bound.
    if (relation == "le") current = std::min(current, bound);
    if (relation == "ge") current = std::max(current, bound);
    moved.emplace_back(sibling, current);
  };
  for (const json::Value& entry : info.as_array()) {
    const std::string self = entry["name"].as_string();
    for (const json::Value& dependency : entry["dependsOn"].as_array()) {
      const std::string other = dependency["key"].as_string();
      const std::string relation = dependency["relation"].as_string();
      const double factor = dependency["factor"].as_number();
      if (self == key) {
        // key <relation> factor * other  =>  other <inverse relation> key / factor.
        constrain(other, relation == "le" ? "ge" : relation == "ge" ? "le" : "", value / factor);
      } else if (other == key) {
        // self <relation> factor * key.
        constrain(self, relation, factor * value);
      }
    }
  }
  return moved;
}

}  // namespace sonare::test
