#include "c_api/eq_band_json.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mastering/eq/eq_band_value.h"
#include "sonare_c_internal.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/json_schema.h"

namespace {

[[noreturn]] void invalid_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidParameter,
                                         "sonare_eq_set_band: " + message);
}

// A refused band document, malformed or carrying a bad field, exits as InvalidFormat across the
// whole C ABI (midi_fx_json.h, parse_scene_json); only the caller's own arguments are
// InvalidParameter.
[[noreturn]] void malformed_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidFormat,
                                         "sonare_eq_set_band: " + message);
}

// A band fragment refuses its first unknown key by name; `$` and `x-` keys are exempt as they
// are in a scene.
void refuse_unknown_band_keys(const sonare::util::json::Value& json) {
  if (!json.is_object()) return;
  const auto& known = sonare::mastering::eq::eq_band_known_keys();
  for (const auto& entry : json.as_object()) {
    const std::string& key = entry.first;
    if (std::find(known.begin(), known.end(), key) != known.end() ||
        sonare::util::json::is_annotation_key(key)) {
      continue;
    }
    malformed_eq_json(sonare::util::json::schema::unknown_key(
        sonare::util::json::schema::child_path(std::string(), key)));
  }
}

}  // namespace

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) invalid_eq_json("band_json must not be null");
  sonare::util::json::Value json;
  try {
    json = sonare::util::json::admit_strict(band_json);
  } catch (const sonare::util::json::JsonError& ex) {
    malformed_eq_json(ex.what());
  }
  // Field parsing and validation are shared with the scene walker; it runs first so an earlier
  // spelling of a known key is refused naming the current one rather than as an unknown key.
  const sonare::mastering::eq::EqBand band =
      sonare::mastering::eq::eq_band_from_value(json, std::string());
  refuse_unknown_band_keys(json);
  return band;
}

}  // namespace sonare::c_api
